#include "udp_server.h"
#include "click_protocol.h"
#include "session_tracker.h"
#include "telemetry_protocol.h"
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <sstream>
#include <cstdlib>
#include <array>
#include <list>
#include <deque>
#include <algorithm>
#include <limits>
#include <random>

namespace {
using Clock=std::chrono::steady_clock;
constexpr size_t MAX_CLICK_COMMANDS=64;
constexpr size_t MAX_CACHE_RECORDS=256;
uint64_t now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
uint32_t saturate(uint64_t value){return uint32_t(std::min<uint64_t>(value,std::numeric_limits<uint32_t>::max()));}

struct Key {uint64_t session=0,command=0;bool operator==(const Key& b)const{return session==b.session&&command==b.command;}};
struct CachedCommand {
    Key key;click_protocol::Request request;uint8_t button=0;uint32_t accepted=0,completed=0;
    click_protocol::Status status=click_protocol::Status::Accepted;bool terminal=false;
    sockaddr_in peer{};
};
enum class ClickPhase {AwaitPress,PressInflight,Holding,AwaitRelease,ReleaseInflight};
struct ClickWork {
    Key key;uint8_t button=0;uint32_t count=0,press_us=0,interval_us=0,index=0;
    uint64_t press_written_ns=0,due_ns=0;ClickPhase phase=ClickPhase::AwaitPress;
    uint8_t version=click_protocol::VERSION1;bool blocked=false,binary=false;
};
bool same_request(const click_protocol::Request& a,const click_protocol::Request& b) {
    return a.version==b.version&&a.operation==b.operation&&a.button==b.button&&a.count==b.count&&
        a.press_us==b.press_us&&a.interval_us==b.interval_us;
}
uint64_t random_epoch() {
    std::random_device source;uint64_t value=(uint64_t(source())<<32)^source()^now_ns()^
        (uint64_t(getpid())<<17);
    return value?value:1;
}
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
    const uint64_t server_epoch=random_epoch();
    uint8_t persistent_buttons=0,scheduled_buttons=0;uint64_t tracked_generation=0;
    sockaddr_in owner{};bool owned=false,have_sequence=false;uint32_t last_sequence=0;
    auto last=Clock::now();std::list<CachedCommand> cache;std::deque<ClickWork> work;
    click_sessions::Tracker sessions;uint64_t accepted_total=0,completed_total=0;
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
                      uint32_t accepted,uint32_t completed,uint8_t version) {
        click_protocol::Ack ack{status,button,key.session,key.command,server_epoch,accepted,completed,depth()};
        ack.version=version;
        auto packet=click_protocol::packet(ack);send_packet(to,packet.data(),packet.size());
    };
    auto send_cached=[&](CachedCommand& command,click_protocol::Status override_status) {
        send_ack(command.peer,command.key,command.button,override_status,command.accepted,command.completed,
                 command.request.version);
    };
    auto terminal=[&](const Key& key,click_protocol::Status status) {
        if(auto* c=find_cache(key)){c->status=status;c->terminal=true;send_cached(*c,status);}
    };
    auto cancel_work=[&]() {
        while(!work.empty()) {
            auto click=work.front();work.pop_front();
            if(click.binary)terminal(click.key,click_protocol::Status::Cancelled);
        }
        scheduled_buttons=0;
    };
    auto begin_reset=[&](bool attach_release,const Key& key) {
        if(attach_release&&release_key.session&&!(release_key==key))
            terminal(release_key,click_protocol::Status::Cancelled);
        cancel_work();persistent_buttons=scheduled_buttons=0;
        auto cleared=clear_synthetic_state();tracked_generation=cleared.generation;
        // Release is an obligation, not a one-shot enqueue. Temporary endpoint
        // loss, layout changes, watchdog cleanup and writer failures leave it
        // pending until a full writer completion is observed.
        release_inflight=false;release_pending=true;
        if(attach_release)release_key=key;
    };
    auto remove_old_cache=[&]() {
        while(cache.size()>=MAX_CACHE_RECORDS) {
            auto it=std::find_if(cache.begin(),cache.end(),[](const CachedCommand& c){return c.terminal;});
            if(it==cache.end())break;cache.erase(it);
        }
    };
    auto accept_record=[&](const click_protocol::Request& request,const sockaddr_in& from,uint32_t accepted)->CachedCommand* {
        remove_old_cache();if(cache.size()>=MAX_CACHE_RECORDS)return nullptr;
        CachedCommand c;c.key={request.session,request.command};c.request=request;c.button=request.button;
        c.accepted=accepted;c.peer=from;
        cache.push_back(c);return &cache.back();
    };
    auto session_protected=[&](uint64_t session) {
        return std::any_of(cache.begin(),cache.end(),[&](const CachedCommand& command){
            return command.key.session==session&&!command.terminal;
        });
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
            if(!event.success) {
                begin_reset(false,{});owned=false;have_sequence=false;
                continue;
            }
            if(event.kind==REPORT_RELEASE_ALL&&release_inflight&&event.session==release_key.session&&event.command==release_key.command) {
                release_inflight=false;
                release_pending=false;if(release_key.session)terminal(release_key,click_protocol::Status::Completed);release_key={};
                continue;
            }
            if(work.empty())continue;auto& click=work.front();
            if(event.session!=click.key.session||event.command!=click.key.command||event.index!=click.index)continue;
            uint8_t bit=uint8_t(1u<<(click.button-1));
            if(event.kind==REPORT_CLICK_PRESS&&click.phase==ClickPhase::PressInflight) {
                click.press_written_ns=event.written_ns;click.blocked=event.blocked;
                uint64_t delay=click.blocked?0:uint64_t(click.press_us)*1000;
                click.due_ns=event.written_ns>std::numeric_limits<uint64_t>::max()-delay?
                    std::numeric_limits<uint64_t>::max():event.written_ns+delay;
                click.phase=click.blocked?ClickPhase::AwaitRelease:ClickPhase::Holding;
            } else if(event.kind==REPORT_CLICK_RELEASE&&click.phase==ClickPhase::ReleaseInflight) {
                if(click.blocked||(event.final_buttons&bit)){finish_front(click_protocol::Status::ButtonActive);continue;}
                ++click.index;++completed_total;if(auto* c=find_cache(click.key))c->completed=click.index;
                if(click.index>=click.count){finish_front(click_protocol::Status::Completed);continue;}
                click.phase=ClickPhase::AwaitPress;click.blocked=false;
                uint64_t base=click.version==click_protocol::VERSION2?event.written_ns:click.press_written_ns;
                uint64_t delay=uint64_t(click.interval_us)*1000;
                click.due_ns=base>std::numeric_limits<uint64_t>::max()-delay?
                    std::numeric_limits<uint64_t>::max():base+delay;
            }
        }
    };

    while(running) {
        auto snapshot=endpoint_snapshot();
        if(!tracked_generation&&snapshot.ready)tracked_generation=snapshot.generation;
        if(tracked_generation&&snapshot.generation!=tracked_generation) {
            begin_reset(false,{});owned=false;have_sequence=false;
            snapshot=endpoint_snapshot();tracked_generation=snapshot.generation;
        }
        process_writer_events();
        if(take_writer_overflow()) {
            begin_reset(false,{});owned=false;have_sequence=false;
        }
        auto now=Clock::now();
        if(owned&&now-last>std::chrono::milliseconds(250)) {
            begin_reset(false,{});owned=false;have_sequence=false;
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
            t.poll_us=e.poll_us;t.physical_age_us=e.physical_age_us;t.physical_received=e.physical_received;
            t.physical_submitted=e.physical_submitted;t.output_queue=e.output_queue;
            t.synthetic_pending=e.synthetic_pending;t.superseded=e.superseded;t.writer_failures=e.writer_failures;
            t.snapshot_ns=e.snapshot_ns;t.cumulative_x=e.cumulative_x;t.cumulative_y=e.cumulative_y;
            if(!(t==previous_telemetry)||telemetry_now-last_telemetry>=10000000) {
                if(subscription.version==1){auto packet=subscription.packet1(server_epoch,t);send_packet(subscriber,packet.data(),packet.size());}
                else if(subscription.version==2){auto packet=subscription.packet2(server_epoch,t);send_packet(subscriber,packet.data(),packet.size());}
                else {auto packet=subscription.packet3(server_epoch,t);send_packet(subscriber,packet.data(),packet.size());}
                previous_telemetry=t;last_telemetry=telemetry_now;
            }
        }

        pollfd poller{sockfd,POLLIN,0};if(poll(&poller,1,2)<=0)continue;
        uint8_t data[1024];sockaddr_in from{};socklen_t length=sizeof(from);
        int n=recvfrom(sockfd,data,sizeof(data),MSG_TRUNC,reinterpret_cast<sockaddr*>(&from),&length);
        if(n<=0||n>int(sizeof(data)))continue;if(peer_filter&&from.sin_addr.s_addr!=allowed.s_addr)continue;
        auto reply=[&](const std::string& reply_text){send_packet(from,reply_text.data(),reply_text.size());};

        if(n>=4&&(!std::memcmp(data,"UPS1",4)||!std::memcmp(data,"UPS2",4)||!std::memcmp(data,"UPS3",4))) {
            if(subscription.alive(now_ns())&&!same_peer(subscriber,from)){reply("busy");continue;}
            if(subscription.accept(data,size_t(n),now_ns())){subscriber=from;last_telemetry=0;}
            continue;
        }
        if(n>=4&&!std::memcmp(data,"UPC1",4)) {
            click_protocol::Request request;
            if(!click_protocol::parse(data,size_t(n),request)) {
                Key key{};if(n>=24)key={click_protocol::get64(data+8),click_protocol::get64(data+16)};
                uint8_t version=n>4&&data[4]==click_protocol::VERSION2?click_protocol::VERSION2:click_protocol::VERSION1;
                send_ack(from,key,n>6?data[6]:0,click_protocol::Status::Invalid,0,0,version);continue;
            }
            Key key{request.session,request.command};
            if(owned&&!same_peer(owner,from)) {
                send_ack(from,key,request.button,click_protocol::Status::Busy,0,0,request.version);continue;
            }
            if(auto* prior=find_cache(key)) {
                if(!same_request(prior->request,request)) {
                    send_ack(from,key,request.button,click_protocol::Status::Invalid,0,0,request.version);continue;
                }
                owner=from;owned=true;last=Clock::now();
                prior->peer=from;
                send_ack(from,prior->key,prior->button,prior->terminal?prior->status:click_protocol::Status::Duplicate,
                         prior->accepted,prior->completed,prior->request.version);continue;
            }
            uint64_t request_now=now_ns();
            if(sessions.stale(request.session,request.command,request_now)) {
                send_ack(from,key,request.button,click_protocol::Status::StaleCommand,0,0,request.version);continue;
            }
            auto e=endpoint_snapshot();
            if(request.operation==click_protocol::Operation::Schedule) {
                if(!e.ready){send_ack(from,key,request.button,click_protocol::Status::NotReady,0,0,request.version);continue;}
                if(request.button<1||request.button>8||!(e.supported&(1u<<(request.button-1)))) {
                    send_ack(from,key,request.button,click_protocol::Status::UnsupportedButton,0,0,request.version);continue;
                }
                uint64_t minimum_interval=uint64_t(request.press_us)+uint64_t(e.poll_us);
                bool invalid=!request.count||request.count>10000||request.press_us<e.poll_us||
                    request.press_us>5000000||request.interval_us>60000000;
                if(request.version==click_protocol::VERSION1&&request.interval_us<minimum_interval)invalid=true;
                if(invalid) {
                    send_ack(from,key,request.button,click_protocol::Status::Invalid,0,0,request.version);continue;
                }
                uint8_t bit=uint8_t(1u<<(request.button-1));
                if((e.physical|persistent_buttons)&bit) {
                    send_ack(from,key,request.button,click_protocol::Status::ButtonActive,0,0,request.version);continue;
                }
                if(work.size()>=MAX_CLICK_COMMANDS) {
                    send_ack(from,key,request.button,click_protocol::Status::QueueFull,0,0,request.version);continue;
                }
                remove_old_cache();
                if(cache.size()>=MAX_CACHE_RECORDS||
                   !sessions.admit(request.session,request_now,false,session_protected)) {
                    send_ack(from,key,request.button,click_protocol::Status::QueueFull,0,0,request.version);continue;
                }
                auto* record=accept_record(request,from,request.count);
                if(!record){send_ack(from,key,request.button,click_protocol::Status::QueueFull,0,0,request.version);continue;}
                sessions.accept(request.session,request.command,request_now);
                ClickWork click;click.key=key;click.button=request.button;click.count=request.count;
                click.press_us=request.press_us;click.interval_us=request.interval_us;click.due_ns=now_ns();
                click.version=request.version;click.binary=true;
                work.push_back(click);accepted_total+=request.count;
                owner=from;owned=true;last=Clock::now();tracked_generation=e.generation;
                send_cached(*record,click_protocol::Status::Accepted);continue;
            }
            remove_old_cache();
            if(cache.size()>=MAX_CACHE_RECORDS||
               !sessions.admit(request.session,request_now,true,session_protected)) {
                send_ack(from,key,0,click_protocol::Status::QueueFull,0,0,request.version);continue;
            }
            auto* record=accept_record(request,from,0);
            if(!record){send_ack(from,key,0,click_protocol::Status::QueueFull,0,0,request.version);continue;}
            sessions.accept(request.session,request.command,request_now);
            owner=from;owned=true;last=Clock::now();
            send_cached(*record,click_protocol::Status::Accepted);begin_reset(true,key);continue;
        }

        std::string text(reinterpret_cast<char*>(data),n);
        while(!text.empty()&&(text.back()=='\n'||text.back()=='\r'))text.pop_back();
        if(text=="+state"){reply(mouse_state_text());continue;}
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
            begin_reset(false,{});owner=from;owned=true;last=Clock::now();reply("ok");continue;
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
