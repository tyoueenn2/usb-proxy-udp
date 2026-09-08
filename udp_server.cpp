#include "udp_server.h"
#include "host-raw-gadget.h"
#include "mouse_protocol.h"
#include "usb_capture.h"
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <sstream>
#include <cstdlib>
#include <array>
#include <random>
#include "telemetry_protocol.h"

namespace {
using Clock=std::chrono::steady_clock;
constexpr uint16_t INJECTED=0x8000;
constexpr uint16_t LATEST=0x4000;
uint64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
std::mutex registry_mutex;
struct Endpoint {
    thread_info* info=nullptr;
    int interface_number=0;
    MouseProfile profile;
    std::vector<uint8_t> latest;
    uint8_t physical=0,synthetic=0;
    uint64_t generation=0;
};
uint64_t next_generation=0;
std::map<int,Endpoint> endpoints;
std::map<int,std::vector<MouseProfile>> descriptors;
std::map<int,std::vector<MouseProfile>> report_descriptors;
std::map<int,bool> boot_protocol;
int selected=-1;
bool ready() {
    std::lock_guard<std::mutex> guard(registry_mutex);
    auto it=endpoints.find(selected);
    return it!=endpoints.end() && !it->second.latest.empty();
}
bool enqueue(const MouseCommand& c, bool latest=false) {
    std::lock_guard<std::mutex> guard(registry_mutex);
    auto found=endpoints.find(selected);
    if(found==endpoints.end() || found->second.latest.empty())return false;
    auto& e=found->second;
    auto parts=split_mouse_command(e.profile,c);
    if(parts.empty())return false;
    std::lock_guard<std::mutex> lock(*e.info->data_mutex);
    auto& queue=*e.info->data_queue;
    // Only replace consecutive aiming corrections with the same button snapshot.
    // Physical reports, wheel events and button transitions are ordering barriers.
    size_t keep=queue.size();
    if(latest && !c.wheel && !c.pan) while(keep) {
        auto& tail=queue[keep-1];
        if(!(tail.inner.flags&LATEST) ||
           e.profile.button_mask(reinterpret_cast<const uint8_t*>(tail.data))!=c.buttons)break;
        --keep;
    }
    if(keep+parts.size()>32)return false;
    queue.resize(keep);
    for(const auto& part:parts) {
        usb_raw_transfer_io io{};
        io.inner.ep=e.info->ep_num;io.inner.flags=INJECTED;
        if(latest&&!c.wheel&&!c.pan)io.inner.flags|=LATEST;
        io.inner.length=e.profile.size;
        io.injection_deadline_ns=latest ? now_ns()+25000000 : 0;
        io.mouse_generation=e.generation;
        std::memcpy(io.data,e.latest.data(),e.latest.size());
        e.profile.encode(part,reinterpret_cast<uint8_t*>(io.data));
        queue.push_back(io);
    }
    e.info->data_cond->notify_all();
    return true;
}
std::string state() {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto it=endpoints.find(selected);
    if(it==endpoints.end()||it->second.latest.empty())return "not_ready";
    auto& e=it->second;
    return "state "+std::to_string(e.physical)+" "+std::to_string(e.synthetic)+" "+
        std::to_string(e.physical|e.synthetic)+" "+std::to_string(e.profile.x.minimum)+" "+
        std::to_string(e.profile.x.maximum)+" "+std::to_string(e.profile.y.minimum)+" "+
        std::to_string(e.profile.y.maximum);
}
proxy_telemetry::Snapshot telemetry_snapshot() {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto it=endpoints.find(selected);
    if(it==endpoints.end()||it->second.latest.empty())return {};
    auto& e=it->second;
    return {true,e.physical,e.profile.x.minimum,e.profile.x.maximum,e.profile.y.minimum,e.profile.y.maximum};
}
}
// Caller holds registry_mutex. Invalidate reports encoded for the previous layout.
static void invalidate_interface(int number) {
    for(auto& item:endpoints)if(item.second.interface_number==number) {
        item.second.latest.clear(); item.second.physical=item.second.synthetic=0;
        item.second.generation=++next_generation;
        auto* info=item.second.info;
        std::lock_guard<std::mutex> queue_lock(*info->data_mutex);
        auto& q=*info->data_queue;
        q.erase(std::remove_if(q.begin(),q.end(),[](const usb_raw_transfer_io& io){return io.inner.flags&INJECTED;}),q.end());
        info->data_cond->notify_all();
    }
}
void learn_mouse_descriptor(int number,const uint8_t* data,unsigned length) {
    auto profiles=parse_mouse_descriptor(data,length);
    std::lock_guard<std::mutex> lock(registry_mutex);
    report_descriptors[number]=profiles;
    if(!boot_protocol[number]) {descriptors[number]=std::move(profiles);invalidate_interface(number);}
}
void set_mouse_protocol(int number,bool boot) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    boot_protocol[number]=boot;
    if(boot) {
        MouseProfile p;p.size=3;
        p.x={8,8,-127,127};p.y={16,8,-127,127};p.relative={p.x,p.y};
        for(int i=0;i<3;++i)p.buttons[i+1]={i,1,0,1};
        descriptors[number]={p};
    } else descriptors[number]=report_descriptors[number];
    invalidate_interface(number);
}
void register_mouse_endpoint(thread_info* info,int number) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    Endpoint e;e.info=info;e.interface_number=number;e.generation=++next_generation;
    endpoints[info->endpoint.bEndpointAddress]=e;
}
void unregister_mouse_endpoint(thread_info* info) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    int address=info->endpoint.bEndpointAddress;
    endpoints.erase(address);
    if(selected==address)selected=-1;
}
bool merge_mouse_report(uint8_t address,usb_raw_transfer_io& io) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    bool injected=(io.inner.flags&INJECTED)!=0;
    io.inner.flags&=~(INJECTED|LATEST);
    auto found=endpoints.find(address);if(found==endpoints.end())return !injected;
    auto& e=found->second;auto* p=reinterpret_cast<uint8_t*>(io.data);
    if(injected && io.mouse_generation!=e.generation)return false;
    if(!injected) {
        for(auto& profile:descriptors[e.interface_number]) if(profile.matches(p,io.inner.length) &&
            (e.latest.empty() || profile.id==e.profile.id)) {
            if(e.latest.empty()) {
                e.profile=profile;
                capture_report(address,e.interface_number,p,io.inner.length);
            }
            e.latest.assign(p,p+io.inner.length);
            e.physical=profile.button_mask(p);
            if(selected<0)selected=address;
            break;
        }
    }
    if(e.latest.empty() || !e.profile.matches(p,io.inner.length))return !injected;
    if(injected)e.synthetic=e.profile.button_mask(p);
    if(injected && io.injection_deadline_ns && now_ns()>io.injection_deadline_ns)
        for(const auto& field:e.profile.relative)field.put(p,0);
    e.profile.put_buttons(p,e.physical|e.synthetic);
    return true;
}
bool UdpServer::start() {
    if(running)return true;
    const char* peer=std::getenv("USB_PROXY_PEER");in_addr allowed{};
    if(peer&&inet_pton(AF_INET,peer,&allowed)!=1){fprintf(stderr,"Invalid USB_PROXY_PEER\n");return false;}
    sockfd=socket(AF_INET,SOCK_DGRAM,0);if(sockfd<0){perror("socket");return false;}
    int size=65536;setsockopt(sockfd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);
    const char* bind_ip=std::getenv("USB_PROXY_BIND");
    if(inet_pton(AF_INET,bind_ip?bind_ip:"0.0.0.0",&address.sin_addr)!=1 ||
       bind(sockfd,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0) {
        perror("UDP bind");close(sockfd);sockfd=-1;return false;
    }
    running=true;server_thread=std::thread(&UdpServer::server_loop,this);
    return true;
}
void UdpServer::stop() {
    running=false;join(); // poll wakes within 2 ms; never close a descriptor under recvfrom.
    if(sockfd>=0){close(sockfd);sockfd=-1;}
}
void UdpServer::join(){if(server_thread.joinable())server_thread.join();}
void UdpServer::server_loop() {
    std::random_device random;
    const uint64_t telemetry_session=(uint64_t(random())<<32|random())|1;
    proxy_telemetry::Subscription subscription;
    proxy_telemetry::Snapshot previous_snapshot;
    sockaddr_in subscriber{};
    uint64_t last_telemetry=0;
    uint8_t requested_buttons=0;
    sockaddr_in owner{};bool owned=false,have_sequence=false;uint32_t last_sequence=0;
    auto last=Clock::now();std::array<Clock::time_point,8> release_at{};
    in_addr allowed{};const char* peer=std::getenv("USB_PROXY_PEER");
    if(peer&&inet_pton(AF_INET,peer,&allowed)!=1){fprintf(stderr,"Invalid USB_PROXY_PEER\n");return;}
    while(running) {
        // Network publication lives on this worker, never on the USB writer.
        // A physical state change is observed on the next loop (poll <= 2 ms).
        auto telemetry_now=now_ns();
        if(subscription.alive(telemetry_now)) {
            auto snapshot=telemetry_snapshot();
            if(!(snapshot==previous_snapshot)||telemetry_now-last_telemetry>=10000000) {
                auto packet=subscription.packet(telemetry_session,snapshot);
                sendto(sockfd,packet.data(),packet.size(),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&subscriber),sizeof(subscriber));
                previous_snapshot=snapshot;last_telemetry=telemetry_now;
            }
        }
        if(!ready()) {requested_buttons=0;release_at={};owned=false;have_sequence=false;}
        auto now=Clock::now();
        uint8_t desired=requested_buttons;
        for(int i=0;i<8;++i)if(release_at[i]!=Clock::time_point{}&&now>=release_at[i]) {
            desired&=~(1u<<i);
        }
        bool expired=owned&&now-last>std::chrono::milliseconds(250);
        if(expired)desired=0;
        if(desired!=requested_buttons) {
            MouseCommand c;c.buttons=desired;
            if(enqueue(c)) {
                requested_buttons=desired;
                for(int i=0;i<8;++i)if(!(desired&(1u<<i)))release_at[i]={};
            }
        }
        if(expired && requested_buttons==0){owned=false;have_sequence=false;}
        pollfd poller{sockfd,POLLIN,0};if(poll(&poller,1,2)<=0)continue;
        uint8_t data[1024];sockaddr_in from{};socklen_t length=sizeof(from);
        int n=recvfrom(sockfd,data,sizeof(data),MSG_TRUNC,reinterpret_cast<sockaddr*>(&from),&length);
        if(n<=0||n>int(sizeof(data)))continue;
        if(peer&&from.sin_addr.s_addr!=allowed.s_addr)continue;
        auto reply=[&](const std::string& s){sendto(sockfd,s.data(),s.size(),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&from),length);};
        if(n>=4&&std::memcmp(data,"UPS1",4)==0) {
            if(subscription.alive(now_ns())&&(subscriber.sin_addr.s_addr!=from.sin_addr.s_addr||subscriber.sin_port!=from.sin_port)){reply("busy");continue;}
            if(subscription.accept(data,size_t(n),now_ns())) {subscriber=from;last_telemetry=0;}
            continue; // Does not acquire or renew the movement ownership lease.
        }
        std::string text(reinterpret_cast<char*>(data),n);
        while(!text.empty()&&(text.back()=='\n'||text.back()=='\r'))text.pop_back();
        if(text=="+state") {reply(state());continue;}
        if(owned&&(from.sin_addr.s_addr!=owner.sin_addr.s_addr||from.sin_port!=owner.sin_port)) {reply("busy");continue;}
        MouseCommand c;c.buttons=requested_buttons;
        bool valid=false,binary=false;uint32_t seq=0;int click=-1, cancel_timer=-1;
        // UPX1, sequence:u32be, dx:i16be, dy:i16be, wheel:i8, pan:i8, buttons:u8, reserved:0.
        if(n==16&&std::memcmp(data,"UPX1",4)==0) {
            binary=true;seq=read_be32(data+4);
            if(data[15] || (have_sequence&&!sequence_newer(seq,last_sequence)))continue;
            c.x=read_i16(data+8);c.y=read_i16(data+10);
            c.wheel=data[12]<128?data[12]:int(data[12])-256;
            c.pan=data[13]<128?data[13]:int(data[13])-256;c.buttons=data[14];valid=true;
        } else {
            // A documented ASCII subset; not MAKCU V2 wire compatibility.
            if(text.rfind("km.",0)==0)text=text.substr(2);
            if(!text.empty()&&text[0]=='.') {
                auto at=text.find('(');
                if(at==std::string::npos||text.back()!=')'){reply("error syntax");continue;}
                text="+"+text.substr(1,at-1)+" "+text.substr(at+1,text.size()-at-2);
                std::replace(text.begin(),text.end(),',',' ');
            }
            std::istringstream stream(text);std::string cmd,extra;stream>>cmd;
            int button=1,value=0;
            if(cmd=="+move")valid=bool(stream>>c.x>>c.y);
            else if(cmd=="+wheel")valid=bool(stream>>c.wheel);
            else if(cmd=="+pan")valid=bool(stream>>c.pan);
            else if(cmd=="+release") {c.buttons=0;valid=true;}
            else if(cmd=="+mousedown"||cmd=="+mouseup"||cmd=="+click") {
                stream>>std::ws;
                if(stream.eof())stream.clear();
                else if(!(stream>>button)){reply("error button");continue;}
                valid=button>=1&&button<=8;
                if(valid) {
                    if(cmd=="+mouseup")c.buttons&=~(1u<<(button-1));
                    else c.buttons|=1u<<(button-1);
                    if(cmd=="+click") {click=button-1;if(requested_buttons&(1u<<click))valid=false;}
                    else cancel_timer=button-1;
                }
            } else {
                const char* names[]={"+left","+right","+middle","+side1","+side2"};
                for(int i=0;i<5;++i)if(cmd==names[i]) {
                    valid=bool(stream>>value)&&(value==0||value==1);
                    if(valid){c.buttons=uint8_t((c.buttons&~(1u<<i))|(unsigned(value)<<i));cancel_timer=i;}
                }
            }
            if(stream>>extra)valid=false;
        }
        if(!valid){reply("error command");continue;}
        if(!enqueue(c,binary)){reply("error not_ready_range_or_queue_full");continue;}
        requested_buttons=c.buttons;owner=from;owned=true;last=Clock::now();
        if(binary){last_sequence=seq;have_sequence=true;release_at={};}
        else {
            for(int i=0;i<8;++i)if(!(c.buttons&(1u<<i)))release_at[i]={};
            if(cancel_timer>=0)release_at[cancel_timer]={};
            if(click>=0)release_at[click]=last+std::chrono::milliseconds(10);
            reply("ok");
        }
    }
    MouseCommand release;enqueue(release);requested_buttons=0;
}
