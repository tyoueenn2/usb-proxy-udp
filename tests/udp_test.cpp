#include "host-raw-gadget.h"
#include "udp_server.h"
#include "click_protocol.h"
#include <arpa/inet.h>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <functional>
#include <iostream>

using click_protocol::Status;
struct Written {
    uint8_t kind=0,buttons=0;int x=0,y=0;uint64_t session=0,command=0;uint32_t index=0;
    bool physical=false;
};
struct Fixture {
    std::deque<usb_raw_transfer_io> queue;
    std::mutex queue_mutex,written_mutex;std::condition_variable cond;
    thread_info info{};UdpServer server{19345};int fd=-1;uint32_t sequence=1;
    std::atomic<bool> stop{false},paused{false},fail_next_synthetic{false},pending{false};std::thread writer;
    std::atomic<bool> hold_release{false},release_held{false},release_gate{false};
    std::atomic<uint64_t> physical_writes{0};
    std::vector<Written> written;std::vector<click_protocol::Ack> inbox;
    Fixture() {
        info.ep_num=1;info.endpoint.bEndpointAddress=0x81;info.endpoint.bInterval=1;
        info.data_queue=&queue;info.data_mutex=&queue_mutex;info.data_cond=&cond;info.mouse_poll_interval_us=1000;
        info.mouse_synthetic_pending=&pending;set_mouse_protocol(0,true);register_mouse_endpoint(&info,0);
        queue_physical(0);usb_raw_transfer_io initial{};assert(take_mouse_report(0x81,initial));
        notify_mouse_report_written(0x81,initial,true);
        setenv("USB_PROXY_BIND","127.0.0.1",1);assert(server.start());
        fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);timeval timeout{0,20000};
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(19345);
        inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
        assert(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
        writer=std::thread([&]{writer_loop();});
    }
    ~Fixture() {
        server.stop();stop=true;paused=false;cond.notify_all();if(writer.joinable())writer.join();
        close(fd);unregister_mouse_endpoint(&info);
    }
    void queue_physical(uint8_t buttons,int x=0,int y=0) {
        usb_raw_transfer_io io{};io.inner.length=3;io.data[0]=buttons;io.data[1]=char(x);io.data[2]=char(y);
        queue_physical_mouse_report(0x81,io);
        {std::lock_guard<std::mutex> lock(queue_mutex);queue.push_back(io);}cond.notify_all();
    }
    usb_raw_transfer_io physical(uint8_t buttons,int x=0,int y=0) {
        auto before=physical_writes.load();queue_physical(buttons,x,y);
        assert(wait_until([&]{return physical_writes.load()>before;}));
        auto rows=since(0);auto found=std::find_if(rows.rbegin(),rows.rend(),[](const Written& row){return row.physical;});
        assert(found!=rows.rend());usb_raw_transfer_io io{};io.inner.length=3;
        io.data[0]=char(found->buttons);io.data[1]=char(found->x);io.data[2]=char(found->y);return io;
    }
    void writer_loop() {
        while(!stop) {
            if(paused){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            usb_raw_transfer_io io{};
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                cond.wait_for(lock,std::chrono::milliseconds(2),[&]{return stop||(!paused&&(!queue.empty()||pending.load()));});
                if(stop)break;if(paused){continue;}lock.unlock();
                if(!take_mouse_report(0x81,io))continue;
            }
            cond.notify_all();bool ok=true;
            if(io.mouse_report_kind==REPORT_CLICK_RELEASE&&hold_release.exchange(false)) {
                release_held=true;
                while(!release_gate.load()&&!stop.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if(ok&&io.mouse_report_kind&&fail_next_synthetic.exchange(false))ok=false;
            if(ok) {
                std::lock_guard<std::mutex> lock(written_mutex);
                written.push_back({io.mouse_report_kind,uint8_t(io.data[0]),int8_t(io.data[1]),int8_t(io.data[2]),
                                   io.click_session,io.click_command,io.click_index,bool(io.mouse_physical)});
                if(io.mouse_physical)++physical_writes;
            }
            notify_mouse_report_written(0x81,io,ok);
        }
    }
    void send_bytes(const void* bytes,size_t size){assert(send(fd,bytes,size,0)==ssize_t(size));}
    void upx(int dx=0,int dy=0,uint8_t buttons=0,int wheel=0,int pan=0) {
        uint32_t s=sequence++;uint8_t p[16]={'U','P','X','1',uint8_t(s>>24),uint8_t(s>>16),uint8_t(s>>8),uint8_t(s),
            uint8_t(dx>>8),uint8_t(dx),uint8_t(dy>>8),uint8_t(dy),uint8_t(wheel),uint8_t(pan),buttons,0};send_bytes(p,sizeof(p));
    }
    void schedule(uint64_t session,uint64_t command,uint8_t button,uint32_t count,uint32_t press_us,uint32_t interval_us,
                  uint8_t version=click_protocol::VERSION1) {
        auto p=click_protocol::request(click_protocol::Operation::Schedule,session,command,button,count,press_us,interval_us,version);
        send_bytes(p.data(),p.size());
    }
    void release_all(uint64_t session,uint64_t command) {
        auto p=click_protocol::request(click_protocol::Operation::ReleaseAll,session,command);send_bytes(p.data(),p.size());
    }
    bool receive(click_protocol::Ack* ack=nullptr,std::string* text=nullptr) {
        uint8_t bytes[256];int n=recv(fd,bytes,sizeof(bytes),0);if(n<=0)return false;
        click_protocol::Ack decoded;
        if(click_protocol::parse_ack(bytes,n,decoded)){if(ack)*ack=decoded;else inbox.push_back(decoded);return true;}
        if(text)text->assign(reinterpret_cast<char*>(bytes),n);return true;
    }
    click_protocol::Ack wait_ack(uint64_t session,uint64_t command,Status wanted,
                                 bool heartbeat=false,std::chrono::milliseconds limit=std::chrono::seconds(3)) {
        auto deadline=std::chrono::steady_clock::now()+limit;auto beat=std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now()<deadline) {
            for(auto it=inbox.begin();it!=inbox.end();++it)if(it->session==session&&it->command==command&&it->status==wanted) {
                auto result=*it;inbox.erase(it);return result;
            }
            click_protocol::Ack ack;
            if(receive(&ack,nullptr)) {
                if(ack.session==session&&ack.command==command&&ack.status==wanted)return ack;
                inbox.push_back(ack);
            }
            if(heartbeat&&std::chrono::steady_clock::now()-beat>std::chrono::milliseconds(50)){upx();beat=std::chrono::steady_clock::now();}
        }
        std::cerr<<"missing ack session="<<session<<" command="<<command<<" status="<<int(wanted)<<"\n";assert(false);return {};
    }
    std::string ascii(const std::string& command) {
        send_bytes(command.data(),command.size());auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<deadline) {
            uint8_t bytes[256];int n=recv(fd,bytes,sizeof(bytes),0);if(n<=0)continue;click_protocol::Ack ack;
            if(click_protocol::parse_ack(bytes,n,ack)){inbox.push_back(ack);continue;}
            return std::string(reinterpret_cast<char*>(bytes),n);
        }
        assert(false);return {};
    }
    bool wait_until(const std::function<bool()>& predicate,std::chrono::milliseconds limit=std::chrono::seconds(2)) {
        auto deadline=std::chrono::steady_clock::now()+limit;
        while(std::chrono::steady_clock::now()<deadline){if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        return predicate();
    }
    size_t written_size(){std::lock_guard<std::mutex> lock(written_mutex);return written.size();}
    std::vector<Written> since(size_t start){std::lock_guard<std::mutex> lock(written_mutex);return {written.begin()+start,written.end()};}
    void barrier(){assert(ascii("+state").rfind("state ",0)==0);}
    void wait_synthetic(unsigned value) {
        assert(wait_until([&]{auto s=ascii("+state");std::istringstream in(s);std::string tag;int physical,synthetic;
            return bool(in>>tag>>physical>>synthetic)&&unsigned(synthetic)==value;}));
    }
};

click_protocol::Ack recv_ack(int fd) {
    uint8_t bytes[128];int n=recv(fd,bytes,sizeof(bytes),0);assert(n>0);
    click_protocol::Ack ack;assert(click_protocol::parse_ack(bytes,size_t(n),ack));return ack;
}

void no_endpoint_statuses() {
    UdpServer server(19346);setenv("USB_PROXY_BIND","127.0.0.1",1);assert(server.start());
    int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);timeval timeout{1,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(19346);
    inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
    assert(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    auto schedule=click_protocol::request(click_protocol::Operation::Schedule,77,1,1,1,1000,0,
                                          click_protocol::VERSION2);
    assert(send(fd,schedule.data(),schedule.size(),0)==ssize_t(schedule.size()));
    auto ack=recv_ack(fd);assert(ack.status==Status::NotReady&&ack.version==2);
    auto release=click_protocol::request(click_protocol::Operation::ReleaseAll,77,2,0,0,0,0,
                                         click_protocol::VERSION2);
    assert(send(fd,release.data(),release.size(),0)==ssize_t(release.size()));
    auto first=recv_ack(fd),second=recv_ack(fd);
    assert(first.status==Status::Accepted&&second.status==Status::Cancelled);
    assert(first.version==2&&second.version==2&&first.server_epoch!=0&&first.server_epoch==second.server_epoch);
    close(fd);server.stop();
}

int main() {
    no_endpoint_statuses();
    Fixture f;
    // Physical right hold is never cleared by a zero-button UPX1 movement.
    f.physical(2);size_t start=f.written_size();f.upx(9,0,0);
    assert(f.wait_until([&]{return f.written_size()>start;}));auto rows=f.since(start);
    assert(rows.back().buttons==2&&rows.back().x==9);

    // Physical release leaves a persistent hold; persistent release leaves a physical hold.
    f.upx(0,0,1);f.wait_synthetic(1);auto released=f.physical(0);assert(uint8_t(released.data[0])==1);
    auto held=f.physical(2);assert(uint8_t(held.data[0])==3);f.upx(0,0,0);f.wait_synthetic(0);
    rows=f.since(0);assert(rows.back().buttons==2);f.physical(0);

    // Movement-only supersession cannot cross a genuine physical-report barrier.
    f.paused=true;auto superseded=endpoint_snapshot().superseded;
    f.upx(1);f.upx(2);f.barrier();
    assert(endpoint_snapshot().superseded==superseded+1&&endpoint_snapshot().synthetic_pending==1);
    f.queue_physical(0);f.upx(3);f.upx(4);f.barrier();
    assert(endpoint_snapshot().synthetic_pending==2);
    start=f.written_size();f.paused=false;f.cond.notify_all();
    assert(f.wait_until([&]{return f.written_size()>=start+2;}));rows=f.since(start);
    assert(rows[0].physical&&rows[0].x==2&&rows[1].x==4);f.wait_synthetic(0);

    const uint64_t session=0x1122334455667788ull;
    // Ownership is source IP/port based; a different UDP endpoint receives Busy.
    f.upx();f.barrier();
    int other=socket(AF_INET,SOCK_DGRAM,0);assert(other>=0);timeval other_timeout{1,0};
    setsockopt(other,SOL_SOCKET,SO_RCVTIMEO,&other_timeout,sizeof(other_timeout));
    sockaddr_in proxy{};proxy.sin_family=AF_INET;proxy.sin_port=htons(19345);inet_pton(AF_INET,"127.0.0.1",&proxy.sin_addr);
    assert(connect(other,reinterpret_cast<sockaddr*>(&proxy),sizeof(proxy))==0);
    auto other_request=click_protocol::request(click_protocol::Operation::Schedule,99,1,1,1,1000,0,
                                               click_protocol::VERSION2);
    assert(send(other,other_request.data(),other_request.size(),0)==ssize_t(other_request.size()));
    auto busy=recv_ack(other);assert(busy.status==Status::Busy&&busy.version==2);close(other);
    // Movement remains inside a scheduled press/release edge pair.
    start=f.written_size();f.schedule(session,10,1,1,50000,60000);
    assert(f.wait_ack(session,10,Status::Accepted).accepted==1);
    assert(f.wait_until([&]{auto v=f.since(start);return std::any_of(v.begin(),v.end(),[](auto& r){return r.kind==3;});}));
    f.upx(7);auto completed=f.wait_ack(session,10,Status::Completed,true);assert(completed.accepted==1&&completed.completed==1);
    rows=f.since(start);size_t press=rows.size(),move=rows.size(),release=rows.size();
    for(size_t i=0;i<rows.size();++i){if(rows[i].kind==3)press=std::min(press,i);if(rows[i].kind==1&&rows[i].x==7)move=std::min(move,i);if(rows[i].kind==4)release=std::min(release,i);}
    assert(press<move&&move<release&&rows[press].buttons==1&&rows[move].buttons==1&&rows[release].buttons==0);

    // Same-ID retry after a lost ACK remains exactly once; reordering is rejected.
    start=f.written_size();f.schedule(session,20,1,3,1000,20000);f.schedule(session,20,1,3,1000,20000);
    assert(f.wait_ack(session,20,Status::Accepted).accepted==3);assert(f.wait_ack(session,20,Status::Duplicate).accepted==3);
    f.schedule(session,20,1,4,1000,20000);assert(f.wait_ack(session,20,Status::Invalid).accepted==0);
    completed=f.wait_ack(session,20,Status::Completed,true);assert(completed.completed==3);
    rows=f.since(start);assert(std::count_if(rows.begin(),rows.end(),[](auto& r){return r.command==20&&r.kind==3;})==3);
    assert(std::count_if(rows.begin(),rows.end(),[](auto& r){return r.command==20&&r.kind==4;})==3);
    start=f.written_size();f.schedule(session,20,1,3,1000,20000);
    assert(f.wait_ack(session,20,Status::Completed).completed==3);
    assert(f.written_size()==start);
    f.schedule(session,19,1,1,1000,20000);assert(f.wait_ack(session,19,Status::StaleCommand).accepted==0);

    // Defined conflicts and validation errors never create synthetic state.
    f.physical(1);f.schedule(session,21,1,1,1000,20000);assert(f.wait_ack(session,21,Status::ButtonActive).accepted==0);f.physical(0);
    f.upx(0,0,1);f.wait_synthetic(1);f.schedule(session,21,1,1,1000,20000);
    assert(f.wait_ack(session,21,Status::ButtonActive).accepted==0);f.upx();f.wait_synthetic(0);
    f.schedule(session,21,8,1,1000,20000);assert(f.wait_ack(session,21,Status::UnsupportedButton).accepted==0);
    f.schedule(session,21,1,1,1,2);assert(f.wait_ack(session,21,Status::Invalid).accepted==0);f.wait_synthetic(0);
    f.hold_release=true;f.release_gate=false;f.release_held=false;
    f.schedule(session,22,1,3,1000,0,click_protocol::VERSION2);
    auto accepted_v2=f.wait_ack(session,22,Status::Accepted);assert(accepted_v2.version==2&&accepted_v2.accepted==3);
    assert(f.wait_until([&]{return f.release_held.load();}));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(endpoint_snapshot().synthetic_pending==1); // release completion gates the next v2 press
    f.release_gate=true;
    auto completed_v2=f.wait_ack(session,22,Status::Completed,true);assert(completed_v2.version==2&&completed_v2.completed==3);

    // A physical hold beginning during a click makes the release non-visible and ends ButtonActive.
    f.schedule(session,23,1,1,50000,0,click_protocol::VERSION2);
    assert(f.wait_ack(session,23,Status::Accepted).accepted==1);
    assert(f.wait_until([&]{auto v=f.since(0);return std::any_of(v.begin(),v.end(),[](auto& row){return row.command==23&&row.kind==3;});}));
    f.physical(1);auto became_active=f.wait_ack(session,23,Status::ButtonActive,true);
    assert(became_active.completed==0);f.physical(0);f.wait_synthetic(0);

    // Bounded scheduler queue reports overload, release-all cancels it, then capacity recovers.
    f.paused=true;
    for(uint64_t id=100;id<164;++id){f.schedule(session,id,1,1,1000,20000);assert(f.wait_ack(session,id,Status::Accepted).accepted==1);}
    f.schedule(session,164,1,1,1000,20000);assert(f.wait_ack(session,164,Status::QueueFull).accepted==0);
    f.paused=false;f.cond.notify_all();assert(f.wait_ack(session,100,Status::Completed,true).completed==1);
    f.schedule(session,164,1,1,1000,20000);assert(f.wait_ack(session,164,Status::Accepted).accepted==1);
    f.paused=true;
    f.release_all(session,200);assert(f.wait_ack(session,200,Status::Accepted).accepted==0);
    // A physical report can remain in the same bounded FIFO and is not purged with synthetic traffic.
    f.queue_physical(0);
    f.paused=false;f.cond.notify_all();assert(f.wait_ack(session,200,Status::Completed,true).completed==0);f.wait_synthetic(0);
    f.schedule(session,199,1,1,1000,20000);assert(f.wait_ack(session,199,Status::StaleCommand).accepted==0);
    f.schedule(session,201,1,1,1000,20000);assert(f.wait_ack(session,201,Status::Accepted).accepted==1);
    assert(f.wait_ack(session,201,Status::Completed,true).completed==1);

    // A USB writer failure cancels accepted work and cannot leave an applied press stuck.
    f.fail_next_synthetic=true;f.schedule(session,250,1,2,1000,20000);
    assert(f.wait_ack(session,250,Status::Accepted).accepted==2);
    auto failed=f.wait_ack(session,250,Status::Cancelled,true);assert(failed.completed==0);
    f.wait_synthetic(0);

    // Timeout during a sequence cancels remaining clicks and emits a release.
    f.schedule(session,300,1,100,1000,20000);assert(f.wait_ack(session,300,Status::Accepted).accepted==100);
    auto cancelled=f.wait_ack(session,300,Status::Cancelled,false,std::chrono::seconds(2));
    assert(cancelled.completed<cancelled.accepted);f.wait_synthetic(0);

    // Layout generation changes cancel accepted work and invalidate queued synthetic edges.
    f.schedule(session,400,1,10,50000,60000);assert(f.wait_ack(session,400,Status::Accepted).accepted==10);
    assert(f.wait_until([&]{auto v=f.since(0);return std::any_of(v.begin(),v.end(),[](auto& r){return r.command==400&&r.kind==3;});}));
    set_mouse_protocol(0,true);
    cancelled=f.wait_ack(session,400,Status::Cancelled,false,std::chrono::seconds(2));assert(cancelled.completed<10);
    start=f.written_size();f.physical(0);
    assert(f.wait_until([&]{auto v=f.since(start);return std::any_of(v.begin(),v.end(),[](auto& r){return r.kind==REPORT_RELEASE_ALL;});}));
    f.wait_synthetic(0);

    // Sustained simulated writer tests at 10, 25, and 50 CPS. These are not hardware rate claims.
    auto rate=[&](uint64_t id,uint32_t cps) {
        constexpr uint32_t count=12;size_t at=f.written_size();uint32_t interval=1000000/cps;
        f.schedule(session,id,1,count,1000,interval);assert(f.wait_ack(session,id,Status::Accepted).accepted==count);
        auto done=f.wait_ack(session,id,Status::Completed,true,std::chrono::seconds(4));
        assert(done.accepted==count&&done.completed==count);auto v=f.since(at);
        assert(std::count_if(v.begin(),v.end(),[&](auto& r){return r.command==id&&r.kind==3;})==count);
        assert(std::count_if(v.begin(),v.end(),[&](auto& r){return r.command==id&&r.kind==4;})==count);
        f.wait_synthetic(0);
    };
    rate(500,10);rate(501,25);rate(502,50);

    // Shutdown clears a persistent hold and leaves no synthetic button stuck.
    f.upx(0,0,1);f.wait_synthetic(1);f.server.stop();
    assert(f.wait_until([&]{std::lock_guard<std::mutex> lock(f.written_mutex);return !f.written.empty()&&f.written.back().buttons==0;}));
    std::cout<<"UDP state, ordering, idempotency, overload, watchdog, generation, and 10/25/50 CPS simulation passed\n";
}
