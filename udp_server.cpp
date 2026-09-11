#include "udp_server.h"
#include "host-raw-gadget.h"
#include "mouse_protocol.h"
#include "usb_capture.h"
#include "click_protocol.h"
#include "telemetry_protocol.h"
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <sstream>
#include <cstdlib>
#include <array>
#include <list>
#include <map>
#include <deque>
#include <algorithm>
#include <limits>

namespace {
using Clock=std::chrono::steady_clock;
constexpr uint16_t INJECTED=0x8000;
constexpr uint16_t LATEST=0x4000;
constexpr size_t MAX_USB_QUEUE=32;
constexpr size_t MAX_SYNTHETIC_QUEUE=8;
constexpr size_t MAX_CLICK_COMMANDS=64;
constexpr size_t MAX_CACHE_RECORDS=256;
constexpr size_t MAX_CLIENT_SESSIONS=32;
enum ReportKind:uint8_t {REPORT_NONE,REPORT_MOVEMENT,REPORT_PERSISTENT,REPORT_CLICK_PRESS,
                         REPORT_CLICK_RELEASE,REPORT_RELEASE_ALL};
enum class EnqueueResult {Ok,NotReady,Unsupported,QueueFull};
uint64_t now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
uint32_t saturate(uint64_t value){return uint32_t(std::min<uint64_t>(value,std::numeric_limits<uint32_t>::max()));}

std::mutex registry_mutex;
struct Endpoint {
    thread_info* info=nullptr;
    int interface_number=0;
    MouseProfile profile;
    std::vector<uint8_t> latest;
    uint8_t physical_buttons=0,persistent_buttons=0,scheduled_buttons=0;
    int physical_dx=0,physical_dy=0;
    uint64_t generation=0;
};
uint64_t next_generation=0;
std::map<int,Endpoint> endpoints;
std::map<int,std::vector<MouseProfile>> descriptors;
std::map<int,std::vector<MouseProfile>> report_descriptors;
std::map<int,bool> boot_protocol;
int selected=-1;

struct EndpointSnapshot {
    bool ready=false;uint8_t physical=0,persistent=0,scheduled=0,supported=0;
    int xmin=0,xmax=0,ymin=0,ymax=0,physical_dx=0,physical_dy=0;
    uint32_t poll_us=1000;uint64_t generation=0;
};
EndpointSnapshot endpoint_snapshot() {
    std::lock_guard<std::mutex> lock(registry_mutex);EndpointSnapshot s;s.generation=next_generation;
    auto it=endpoints.find(selected);if(it==endpoints.end()||it->second.latest.empty())return s;
    const auto& e=it->second;s.ready=true;s.physical=e.physical_buttons;s.persistent=e.persistent_buttons;
    s.scheduled=e.scheduled_buttons;s.xmin=e.profile.x.minimum;s.xmax=e.profile.x.maximum;
    s.ymin=e.profile.y.minimum;s.ymax=e.profile.y.maximum;s.physical_dx=e.physical_dx;s.physical_dy=e.physical_dy;
    s.poll_us=std::max<uint32_t>(1,e.info->mouse_poll_interval_us);s.generation=e.generation;
    for(const auto& b:e.profile.buttons)s.supported|=uint8_t(1u<<(b.first-1));
    return s;
}
std::string state() {
    auto s=endpoint_snapshot();if(!s.ready)return "not_ready";
    uint8_t synthetic=s.persistent|s.scheduled;
    return "state "+std::to_string(s.physical)+" "+std::to_string(synthetic)+" "+
        std::to_string(s.physical|synthetic)+" "+std::to_string(s.xmin)+" "+
        std::to_string(s.xmax)+" "+std::to_string(s.ymin)+" "+std::to_string(s.ymax);
}

EnqueueResult enqueue_report(MouseCommand command,uint8_t persistent,uint8_t scheduled,
                             ReportKind kind,bool latest,uint64_t session=0,uint64_t command_id=0,
                             uint32_t click_index=0,uint8_t click_button=0) {
    std::lock_guard<std::mutex> guard(registry_mutex);
    auto found=endpoints.find(selected);if(found==endpoints.end()||found->second.latest.empty())return EnqueueResult::NotReady;
    auto& e=found->second;command.buttons=persistent|scheduled;
    auto parts=split_mouse_command(e.profile,command);if(parts.empty())return EnqueueResult::Unsupported;
    std::lock_guard<std::mutex> lock(*e.info->data_mutex);auto& queue=*e.info->data_queue;
    size_t keep=queue.size();
    // A movement can replace only consecutive movement reports with the same persistent snapshot.
    if(latest)while(keep) {
        const auto& tail=queue[keep-1];
        if(!(tail.inner.flags&LATEST)||tail.mouse_report_kind!=REPORT_MOVEMENT||
           tail.persistent_buttons!=persistent)break;
        --keep;
    }
    size_t synthetic=0;for(size_t i=0;i<keep;++i)if(queue[i].inner.flags&INJECTED)++synthetic;
    if(keep+parts.size()>MAX_USB_QUEUE||synthetic+parts.size()>MAX_SYNTHETIC_QUEUE)return EnqueueResult::QueueFull;
    queue.resize(keep);
    for(const auto& part:parts) {
        usb_raw_transfer_io io{};io.inner.ep=e.info->ep_num;io.inner.flags=INJECTED|(latest?LATEST:0);
        io.inner.length=e.profile.size;io.injection_deadline_ns=latest?now_ns()+25000000:0;
        io.mouse_generation=e.generation;io.persistent_buttons=persistent;io.scheduled_buttons=scheduled;
        io.mouse_report_kind=kind;io.click_session=session;io.click_command=command_id;
        io.click_index=click_index;io.click_button=click_button;
        std::memcpy(io.data,e.latest.data(),e.latest.size());
        if(!e.profile.encode(part,reinterpret_cast<uint8_t*>(io.data)))return EnqueueResult::Unsupported;
        queue.push_back(io);
    }
    e.info->data_cond->notify_all();return EnqueueResult::Ok;
}

EndpointSnapshot clear_synthetic_state() {
    std::lock_guard<std::mutex> guard(registry_mutex);EndpointSnapshot result;result.generation=next_generation;
    auto found=endpoints.find(selected);if(found==endpoints.end())return result;
    auto& e=found->second;e.persistent_buttons=e.scheduled_buttons=0;e.generation=++next_generation;
    std::lock_guard<std::mutex> lock(*e.info->data_mutex);auto& q=*e.info->data_queue;
    q.erase(std::remove_if(q.begin(),q.end(),[](const usb_raw_transfer_io& io){return io.inner.flags&INJECTED;}),q.end());
    e.info->data_cond->notify_all();result.ready=!e.latest.empty();result.physical=e.physical_buttons;
    result.generation=e.generation;result.poll_us=std::max<uint32_t>(1,e.info->mouse_poll_interval_us);return result;
}

struct WriterEvent {
    uint64_t session=0,command=0,generation=0,written_ns=0;uint32_t index=0;
    uint8_t kind=0,button=0,blocked=0,final_buttons=0;bool success=false;
};
std::mutex writer_mutex;
std::deque<WriterEvent> writer_events;
std::atomic<bool> writer_overflow{false};
std::vector<WriterEvent> take_writer_events() {
    std::lock_guard<std::mutex> lock(writer_mutex);
    std::vector<WriterEvent> out(writer_events.begin(),writer_events.end());writer_events.clear();return out;
}

struct Key {uint64_t session=0,command=0;bool operator==(const Key& b)const{return session==b.session&&command==b.command;}};
struct CachedCommand {
    Key key;uint8_t button=0;uint32_t accepted=0,completed=0;
    click_protocol::Status status=click_protocol::Status::Accepted;bool terminal=false;
    sockaddr_in peer{};
};
enum class ClickPhase {AwaitPress,PressInflight,Holding,AwaitRelease,ReleaseInflight};
struct ClickWork {
    Key key;uint8_t button=0;uint32_t count=0,press_us=0,interval_us=0,index=0;
    uint64_t press_written_ns=0,due_ns=0;ClickPhase phase=ClickPhase::AwaitPress;
    bool blocked=false,binary=false;
};
struct SessionState {uint64_t highest=0;};
}

// Caller holds registry_mutex. Invalidate all queued reports encoded for the prior layout.
static void invalidate_interface(int number) {
    for(auto& item:endpoints)if(item.second.interface_number==number) {
        auto& e=item.second;e.latest.clear();e.persistent_buttons=e.scheduled_buttons=0;e.generation=++next_generation;
        std::lock_guard<std::mutex> queue_lock(*e.info->data_mutex);auto& q=*e.info->data_queue;
        q.erase(std::remove_if(q.begin(),q.end(),[](const usb_raw_transfer_io& io){return io.inner.flags&INJECTED;}),q.end());
        e.info->data_cond->notify_all();
    }
}
void learn_mouse_descriptor(int number,const uint8_t* data,unsigned length) {
    auto profiles=parse_mouse_descriptor(data,length);std::lock_guard<std::mutex> lock(registry_mutex);
    report_descriptors[number]=profiles;if(!boot_protocol[number]){descriptors[number]=std::move(profiles);invalidate_interface(number);}
}
void set_mouse_protocol(int number,bool boot) {
    std::lock_guard<std::mutex> lock(registry_mutex);boot_protocol[number]=boot;
    if(boot) {
        MouseProfile p;p.size=3;p.x={8,8,-127,127};p.y={16,8,-127,127};p.relative={p.x,p.y};
        for(int i=0;i<3;++i)p.buttons[i+1]={i,1,0,1};descriptors[number]={p};
    } else descriptors[number]=report_descriptors[number];
    invalidate_interface(number);
}
void register_mouse_endpoint(thread_info* info,int number) {
    std::lock_guard<std::mutex> lock(registry_mutex);Endpoint e;e.info=info;e.interface_number=number;e.generation=++next_generation;
    if(!info->mouse_poll_interval_us)info->mouse_poll_interval_us=1000;endpoints[info->endpoint.bEndpointAddress]=e;
}
void unregister_mouse_endpoint(thread_info* info) {
    std::lock_guard<std::mutex> lock(registry_mutex);int address=info->endpoint.bEndpointAddress;
    auto found=endpoints.find(address);if(found!=endpoints.end()) {
        found->second.persistent_buttons=found->second.scheduled_buttons=0;
        std::lock_guard<std::mutex> queue_lock(*info->data_mutex);auto& q=*info->data_queue;
        q.erase(std::remove_if(q.begin(),q.end(),[](const usb_raw_transfer_io& io){return io.inner.flags&INJECTED;}),q.end());
        endpoints.erase(found);++next_generation;
    }
    if(selected==address)selected=-1;
}
bool merge_mouse_report(uint8_t address,usb_raw_transfer_io& io) {
    std::lock_guard<std::mutex> lock(registry_mutex);bool injected=(io.inner.flags&INJECTED)!=0;
    io.inner.flags&=~(INJECTED|LATEST);auto found=endpoints.find(address);if(found==endpoints.end())return !injected;
    auto& e=found->second;auto* p=reinterpret_cast<uint8_t*>(io.data);
    if(injected&&io.mouse_generation!=e.generation)return false;
    if(!injected) {
        for(auto& profile:descriptors[e.interface_number])if(profile.matches(p,io.inner.length)&&
            (e.latest.empty()||profile.id==e.profile.id)) {
            if(e.latest.empty()){e.profile=profile;capture_report(address,e.interface_number,p,io.inner.length);}
            e.latest.assign(p,p+io.inner.length);e.physical_buttons=profile.button_mask(p);
            e.physical_dx=profile.x.get(p);e.physical_dy=profile.y.get(p);if(selected<0)selected=address;break;
        }
    }
    if(e.latest.empty()||!e.profile.matches(p,io.inner.length))return !injected;
    if(injected) {
        if(io.mouse_report_kind==REPORT_CLICK_PRESS&&io.click_button)
            io.click_blocked=((e.physical_buttons|io.persistent_buttons)&(1u<<(io.click_button-1)))!=0;
        e.persistent_buttons=io.persistent_buttons;e.scheduled_buttons=io.scheduled_buttons;
        if(io.injection_deadline_ns&&now_ns()>io.injection_deadline_ns)
            for(const auto& field:e.profile.relative)field.put(p,0);
    }
    io.mouse_final_buttons=e.physical_buttons|e.persistent_buttons|e.scheduled_buttons;
    e.profile.put_buttons(p,io.mouse_final_buttons);return true;
}
void notify_mouse_report_written(uint8_t,const usb_raw_transfer_io& io,bool success) {
    if(io.mouse_report_kind!=REPORT_CLICK_PRESS&&io.mouse_report_kind!=REPORT_CLICK_RELEASE&&
       io.mouse_report_kind!=REPORT_RELEASE_ALL)return;
    WriterEvent event{io.click_session,io.click_command,io.mouse_generation,now_ns(),io.click_index,
        io.mouse_report_kind,io.click_button,io.click_blocked,io.mouse_final_buttons,success};
    std::lock_guard<std::mutex> lock(writer_mutex);
    if(writer_events.size()>=16)writer_overflow=true;else writer_events.push_back(event);
}

bool UdpServer::start() {
    if(running)return true;const char* peer=std::getenv("USB_PROXY_PEER");in_addr allowed{};
    if(peer&&inet_pton(AF_INET,peer,&allowed)!=1){fprintf(stderr,"Invalid USB_PROXY_PEER\n");return false;}
    sockfd=socket(AF_INET,SOCK_DGRAM,0);if(sockfd<0){perror("socket");return false;}
    int size=65536;setsockopt(sockfd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);
    const char* bind_ip=std::getenv("USB_PROXY_BIND");
    if(inet_pton(AF_INET,bind_ip?bind_ip:"0.0.0.0",&address.sin_addr)!=1||
       bind(sockfd,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0) {
        perror("UDP bind");close(sockfd);sockfd=-1;return false;
    }
    running=true;server_thread=std::thread(&UdpServer::server_loop,this);return true;
}
void UdpServer::stop(){running=false;join();if(sockfd>=0){close(sockfd);sockfd=-1;}}
void UdpServer::join(){if(server_thread.joinable())server_thread.join();}

void UdpServer::server_loop() {
    const uint64_t server_epoch=(now_ns()^(uint64_t(getpid())<<32)^uint64_t(reinterpret_cast<uintptr_t>(this)))|1;
    uint8_t persistent_buttons=0,scheduled_buttons=0;uint64_t tracked_generation=0;
    sockaddr_in owner{};bool owned=false,have_sequence=false;uint32_t last_sequence=0;uint64_t owner_session=0;
    auto last=Clock::now();std::list<CachedCommand> cache;std::deque<ClickWork> work;
    std::map<uint64_t,SessionState> sessions;uint64_t accepted_total=0,completed_total=0;
    bool release_pending=false,release_inflight=false;Key release_key{};
    proxy_telemetry::Subscription subscription;proxy_telemetry::Snapshot previous_telemetry;
    sockaddr_in subscriber{};uint64_t last_telemetry=0;
    in_addr allowed{};const char* peer_filter=std::getenv("USB_PROXY_PEER");
    if(peer_filter&&inet_pton(AF_INET,peer_filter,&allowed)!=1)return;

    auto same_peer=[](const sockaddr_in& a,const sockaddr_in& b){return a.sin_addr.s_addr==b.sin_addr.s_addr&&a.sin_port==b.sin_port;};
    auto depth=[&](){return uint16_t(std::min<size_t>(65535,work.size()+(release_pending?1:0)));};
    auto find_cache=[&](const Key& key)->CachedCommand* {for(auto& c:cache)if(c.key==key)return &c;return nullptr;};
    auto send_packet=[&](const sockaddr_in& to,const void* bytes,size_t size){
        sendto(sockfd,bytes,size,MSG_DONTWAIT,reinterpret_cast<const sockaddr*>(&to),sizeof(to));
    };
    auto send_ack=[&](const sockaddr_in& to,const Key& key,uint8_t button,click_protocol::Status status,
                      uint32_t accepted,uint32_t completed) {
        click_protocol::Ack ack{status,button,key.session,key.command,server_epoch,accepted,completed,depth()};
        auto packet=click_protocol::packet(ack);send_packet(to,packet.data(),packet.size());
    };
    auto send_cached=[&](CachedCommand& command,click_protocol::Status override_status) {
        send_ack(command.peer,command.key,command.button,override_status,command.accepted,command.completed);
    };
    auto terminal=[&](const Key& key,click_protocol::Status status) {
        if(auto* c=find_cache(key)){c->status=status;c->terminal=true;send_cached(*c,status);}
    };
    auto cancel_work=[&]() {
        for(const auto& click:work)if(click.binary)terminal(click.key,click_protocol::Status::Cancelled);
        work.clear();scheduled_buttons=0;
    };
    auto begin_reset=[&](bool attach_release,const Key& key) {
        if(release_key.session&&(!attach_release||!(release_key==key)))
            terminal(release_key,click_protocol::Status::Cancelled);
        cancel_work();persistent_buttons=scheduled_buttons=0;
        auto cleared=clear_synthetic_state();tracked_generation=cleared.generation;
        release_inflight=false;release_pending=cleared.ready;
        if(attach_release) {
            release_key=key;
            if(!cleared.ready){release_pending=false;terminal(key,click_protocol::Status::Completed);}
        } else release_key={};
    };
    auto remove_old_cache=[&]() {
        while(cache.size()>=MAX_CACHE_RECORDS) {
            auto it=std::find_if(cache.begin(),cache.end(),[](const CachedCommand& c){return c.terminal;});
            if(it==cache.end())break;cache.erase(it);
        }
    };
    auto accept_record=[&](const click_protocol::Request& request,const sockaddr_in& from,uint32_t accepted)->CachedCommand* {
        remove_old_cache();if(cache.size()>=MAX_CACHE_RECORDS)return nullptr;
        CachedCommand c;c.key={request.session,request.command};c.button=request.button;c.accepted=accepted;c.peer=from;
        cache.push_back(c);sessions[request.session].highest=request.command;return &cache.back();
    };
    auto finish_front=[&](click_protocol::Status status) {
        if(work.empty())return;auto click=work.front();work.pop_front();scheduled_buttons=0;
        if(click.binary)terminal(click.key,status);
    };
    auto scheduler_tick=[&](uint64_t now) {
        if(release_pending&&!release_inflight) {
            MouseCommand release;
            auto result=enqueue_report(release,0,0,REPORT_RELEASE_ALL,false,release_key.session,release_key.command);
            if(result==EnqueueResult::Ok)release_inflight=true;
        }
        if(work.empty()||release_pending)return;
        auto& click=work.front();uint8_t bit=uint8_t(1u<<(click.button-1));
        if(click.phase==ClickPhase::AwaitPress&&now>=click.due_ns) {
            auto snapshot=endpoint_snapshot();
            if(!snapshot.ready||snapshot.generation!=tracked_generation){begin_reset(false,{});return;}
            if((snapshot.physical|persistent_buttons|scheduled_buttons)&bit){finish_front(click_protocol::Status::ButtonActive);return;}
            MouseCommand edge;uint8_t next=scheduled_buttons|bit;
            auto result=enqueue_report(edge,persistent_buttons,next,REPORT_CLICK_PRESS,false,
                                       click.key.session,click.key.command,click.index,click.button);
            if(result==EnqueueResult::Ok){scheduled_buttons=next;click.phase=ClickPhase::PressInflight;}
            else if(result==EnqueueResult::Unsupported)begin_reset(false,{});
        } else if((click.phase==ClickPhase::Holding||click.phase==ClickPhase::AwaitRelease)&&now>=click.due_ns) {
            MouseCommand edge;uint8_t next=scheduled_buttons&~bit;
            auto result=enqueue_report(edge,persistent_buttons,next,REPORT_CLICK_RELEASE,false,
                                       click.key.session,click.key.command,click.index,click.button);
            if(result==EnqueueResult::Ok){scheduled_buttons=next;click.phase=ClickPhase::ReleaseInflight;}
            else if(result==EnqueueResult::Unsupported)begin_reset(false,{});
            else click.phase=ClickPhase::AwaitRelease;
        }
    };
    auto process_writer_events=[&]() {
        for(const auto& event:take_writer_events()) {
            if(event.kind==REPORT_RELEASE_ALL&&release_inflight&&event.session==release_key.session&&event.command==release_key.command) {
                release_inflight=false;
                if(event.success){release_pending=false;if(release_key.session)terminal(release_key,click_protocol::Status::Completed);release_key={};}
                continue;
            }
            if(work.empty())continue;auto& click=work.front();
            if(event.session!=click.key.session||event.command!=click.key.command||event.index!=click.index)continue;
            if(!event.success){begin_reset(false,{});continue;}
            uint8_t bit=uint8_t(1u<<(click.button-1));
            if(event.kind==REPORT_CLICK_PRESS&&click.phase==ClickPhase::PressInflight) {
                click.press_written_ns=event.written_ns;click.blocked=event.blocked;
                click.due_ns=event.written_ns+(click.blocked?0:uint64_t(click.press_us)*1000);
                click.phase=click.blocked?ClickPhase::AwaitRelease:ClickPhase::Holding;
            } else if(event.kind==REPORT_CLICK_RELEASE&&click.phase==ClickPhase::ReleaseInflight) {
                if(click.blocked||(event.final_buttons&bit)){finish_front(click_protocol::Status::ButtonActive);continue;}
                ++click.index;++completed_total;if(auto* c=find_cache(click.key))c->completed=click.index;
                if(click.index>=click.count){finish_front(click_protocol::Status::Completed);continue;}
                click.phase=ClickPhase::AwaitPress;click.blocked=false;
                click.due_ns=click.press_written_ns+uint64_t(click.interval_us)*1000;
            }
        }
    };

    while(running) {
        process_writer_events();auto snapshot=endpoint_snapshot();
        if(!tracked_generation&&snapshot.ready)tracked_generation=snapshot.generation;
        if(tracked_generation&&snapshot.generation!=tracked_generation) {
            begin_reset(false,{});owned=false;owner_session=0;have_sequence=false;
            snapshot=endpoint_snapshot();tracked_generation=snapshot.generation;
        }
        if(writer_overflow.exchange(false)) {
            begin_reset(false,{});owned=false;owner_session=0;have_sequence=false;
        }
        auto now=Clock::now();
        if(owned&&now-last>std::chrono::milliseconds(250)) {
            begin_reset(false,{});owned=false;owner_session=0;have_sequence=false;
        }
        scheduler_tick(now_ns());

        auto telemetry_now=now_ns();
        if(subscription.alive(telemetry_now)) {
            auto e=endpoint_snapshot();proxy_telemetry::Snapshot t;
            t.ready=e.ready;t.physical=e.physical;t.persistent=e.persistent;t.scheduled=e.scheduled;
            t.xmin=e.xmin;t.xmax=e.xmax;t.ymin=e.ymin;t.ymax=e.ymax;
            t.physical_dx=e.physical_dx;t.physical_dy=e.physical_dy;t.accepted=saturate(accepted_total);
            t.completed=saturate(completed_total);t.active=work.empty()?0:1;
            t.queued=uint16_t(work.size()>1?std::min<size_t>(65535,work.size()-1):0);t.generation=e.generation;
            if(!(t==previous_telemetry)||telemetry_now-last_telemetry>=10000000) {
                if(subscription.version==1){auto packet=subscription.packet1(server_epoch,t);send_packet(subscriber,packet.data(),packet.size());}
                else {auto packet=subscription.packet2(server_epoch,t);send_packet(subscriber,packet.data(),packet.size());}
                previous_telemetry=t;last_telemetry=telemetry_now;
            }
        }

        pollfd poller{sockfd,POLLIN,0};if(poll(&poller,1,2)<=0)continue;
        uint8_t data[1024];sockaddr_in from{};socklen_t length=sizeof(from);
        int n=recvfrom(sockfd,data,sizeof(data),MSG_TRUNC,reinterpret_cast<sockaddr*>(&from),&length);
        if(n<=0||n>int(sizeof(data)))continue;if(peer_filter&&from.sin_addr.s_addr!=allowed.s_addr)continue;
        auto reply=[&](const std::string& reply_text){send_packet(from,reply_text.data(),reply_text.size());};

        if(n>=4&&(!std::memcmp(data,"UPS1",4)||!std::memcmp(data,"UPS2",4))) {
            if(subscription.alive(now_ns())&&!same_peer(subscriber,from)){reply("busy");continue;}
            if(subscription.accept(data,size_t(n),now_ns())){subscriber=from;last_telemetry=0;}
            continue;
        }
        if(n>=4&&!std::memcmp(data,"UPC1",4)) {
            click_protocol::Request request;
            if(!click_protocol::parse(data,size_t(n),request)) {
                Key key{};if(n>=24)key={click_protocol::get64(data+8),click_protocol::get64(data+16)};
                send_ack(from,key,n>6?data[6]:0,click_protocol::Status::Invalid,0,0);continue;
            }
            Key key{request.session,request.command};
            if(owned&&(!same_peer(owner,from)||(owner_session&&owner_session!=request.session))) {
                send_ack(from,key,request.button,click_protocol::Status::Busy,0,0);continue;
            }
            if(auto* prior=find_cache(key)) {
                owner=from;owned=true;owner_session=request.session;last=Clock::now();
                send_ack(from,prior->key,prior->button,prior->terminal?prior->status:click_protocol::Status::Duplicate,
                         prior->accepted,prior->completed);continue;
            }
            auto session=sessions.find(request.session);
            if(session!=sessions.end()&&request.command<=session->second.highest) {
                send_ack(from,key,request.button,click_protocol::Status::StaleCommand,0,0);continue;
            }
            if(session==sessions.end()&&sessions.size()>=MAX_CLIENT_SESSIONS) {
                send_ack(from,key,request.button,click_protocol::Status::QueueFull,0,0);continue;
            }
            auto e=endpoint_snapshot();
            if(request.operation==click_protocol::Operation::Schedule) {
                if(!e.ready){send_ack(from,key,request.button,click_protocol::Status::NotReady,0,0);continue;}
                if(request.button<1||request.button>8||!(e.supported&(1u<<(request.button-1)))) {
                    send_ack(from,key,request.button,click_protocol::Status::UnsupportedButton,0,0);continue;
                }
                uint64_t minimum_interval=uint64_t(request.press_us)+e.poll_us;
                if(!request.count||request.count>10000||request.press_us<e.poll_us||request.press_us>5000000||
                   request.interval_us<minimum_interval||request.interval_us>60000000) {
                    send_ack(from,key,request.button,click_protocol::Status::Invalid,0,0);continue;
                }
                uint8_t bit=uint8_t(1u<<(request.button-1));
                if((e.physical|persistent_buttons)&bit) {
                    send_ack(from,key,request.button,click_protocol::Status::ButtonActive,0,0);continue;
                }
                if(work.size()>=MAX_CLICK_COMMANDS) {
                    send_ack(from,key,request.button,click_protocol::Status::QueueFull,0,0);continue;
                }
                auto* record=accept_record(request,from,request.count);
                if(!record){send_ack(from,key,request.button,click_protocol::Status::QueueFull,0,0);continue;}
                ClickWork click;click.key=key;click.button=request.button;click.count=request.count;
                click.press_us=request.press_us;click.interval_us=request.interval_us;click.due_ns=now_ns();click.binary=true;
                work.push_back(click);accepted_total+=request.count;
                owner=from;owned=true;owner_session=request.session;last=Clock::now();tracked_generation=e.generation;
                send_cached(*record,click_protocol::Status::Accepted);continue;
            }
            if(release_pending&&release_key.session) {
                send_ack(from,key,0,click_protocol::Status::QueueFull,0,0);continue;
            }
            auto* record=accept_record(request,from,0);
            if(!record){send_ack(from,key,0,click_protocol::Status::QueueFull,0,0);continue;}
            owner=from;owned=true;owner_session=request.session;last=Clock::now();
            send_cached(*record,click_protocol::Status::Accepted);begin_reset(true,key);continue;
        }

        std::string text(reinterpret_cast<char*>(data),n);
        while(!text.empty()&&(text.back()=='\n'||text.back()=='\r'))text.pop_back();
        if(text=="+state"){reply(state());continue;}
        if(owned&&!same_peer(owner,from)){reply("busy");continue;}

        if(n==16&&!std::memcmp(data,"UPX1",4)) {
            uint32_t sequence=read_be32(data+4);if(data[15]||(have_sequence&&!sequence_newer(sequence,last_sequence)))continue;
            MouseCommand command;command.x=read_i16(data+8);command.y=read_i16(data+10);
            command.wheel=data[12]<128?data[12]:int(data[12])-256;
            command.pan=data[13]<128?data[13]:int(data[13])-256;uint8_t next=data[14];
            auto e=endpoint_snapshot();if(!e.ready||next&~e.supported)continue;
            bool transition=next!=persistent_buttons;bool movement=command.x||command.y;
            EnqueueResult result=EnqueueResult::Ok;
            if(transition||movement||command.wheel||command.pan)
                result=enqueue_report(command,next,scheduled_buttons,transition?REPORT_PERSISTENT:REPORT_MOVEMENT,
                    !transition&&movement&&!command.wheel&&!command.pan);
            if(result==EnqueueResult::Ok) {
                persistent_buttons=next;last_sequence=sequence;have_sequence=true;
                owner=from;owned=true;last=Clock::now();tracked_generation=e.generation;
            }
            continue;
        }

        if(text.rfind("km.",0)==0)text=text.substr(2);
        if(!text.empty()&&text[0]=='.') {
            auto at=text.find('(');if(at==std::string::npos||text.back()!=')'){reply("error syntax");continue;}
            text="+"+text.substr(1,at-1)+" "+text.substr(at+1,text.size()-at-2);std::replace(text.begin(),text.end(),',',' ');
        }
        std::istringstream stream(text);std::string command_name,extra;stream>>command_name;
        if(command_name=="+release") {
            if(stream>>extra){reply("error command");continue;}
            begin_reset(false,{});owner=from;owned=true;owner_session=0;last=Clock::now();reply("ok");continue;
        }
        MouseCommand command;int button=1,value=0;bool valid=false,click=false;uint8_t next=persistent_buttons;
        if(command_name=="+move")valid=bool(stream>>command.x>>command.y);
        else if(command_name=="+wheel")valid=bool(stream>>command.wheel);
        else if(command_name=="+pan")valid=bool(stream>>command.pan);
        else if(command_name=="+mousedown"||command_name=="+mouseup"||command_name=="+click") {
            stream>>std::ws;if(stream.eof())stream.clear();else if(!(stream>>button)){reply("error button");continue;}
            valid=button>=1&&button<=8;click=command_name=="+click";
            if(valid&&!click)next=command_name=="+mousedown"?uint8_t(next|(1u<<(button-1))):uint8_t(next&~(1u<<(button-1)));
        } else {
            const char* names[]={"+left","+right","+middle","+side1","+side2"};
            for(int i=0;i<5;++i)if(command_name==names[i]) {
                valid=bool(stream>>value)&&(value==0||value==1);button=i+1;
                if(valid)next=uint8_t((next&~(1u<<i))|(unsigned(value)<<i));
            }
        }
        if(stream>>extra)valid=false;if(!valid){reply("error command");continue;}
        auto e=endpoint_snapshot();if(!e.ready){reply("error not_ready_range_or_queue_full");continue;}
        if(button<1||button>8||(next&~e.supported)||(click&&!(e.supported&(1u<<(button-1))))) {
            reply("error button");continue;
        }
        if(click) {
            uint8_t bit=uint8_t(1u<<(button-1));
            if((e.physical|persistent_buttons)&bit){reply("error button_active");continue;}
            if(work.size()>=MAX_CLICK_COMMANDS){reply("error queue_full");continue;}
            ClickWork operation;operation.button=button;operation.count=1;
            operation.press_us=std::max<uint32_t>(10000,e.poll_us);
            operation.interval_us=operation.press_us+e.poll_us;operation.due_ns=now_ns();work.push_back(operation);
            owner=from;owned=true;last=Clock::now();tracked_generation=e.generation;reply("ok");continue;
        }
        bool transition=next!=persistent_buttons;
        auto result=enqueue_report(command,next,scheduled_buttons,transition?REPORT_PERSISTENT:REPORT_MOVEMENT,false);
        if(result!=EnqueueResult::Ok){reply("error not_ready_range_or_queue_full");continue;}
        persistent_buttons=next;owner=from;owned=true;last=Clock::now();tracked_generation=e.generation;reply("ok");
    }
    cancel_work();persistent_buttons=scheduled_buttons=0;clear_synthetic_state();
    enqueue_report(MouseCommand{},0,0,REPORT_RELEASE_ALL,false);
}
