#include "host-raw-gadget.h"
#include "udp_server.h"
#include <arpa/inet.h>
#include <thread>
#include "telemetry_protocol.h"

// Exercises the real UDP server and endpoint merger without USB hardware.
int main() {
    std::deque<usb_raw_transfer_io> queue;
    std::mutex mutex;std::condition_variable cond;
    thread_info info{};info.ep_num=1;info.endpoint.bEndpointAddress=0x81;
    info.data_queue=&queue;info.data_mutex=&mutex;info.data_cond=&cond;
    register_mouse_endpoint(&info,0);set_mouse_protocol(0,true);
    usb_raw_transfer_io physical{};physical.inner.length=3;physical.data[0]=1;
    assert(merge_mouse_report(0x81,physical));
    setenv("USB_PROXY_BIND","127.0.0.1",1);
    UdpServer server(19345);assert(server.start());
    int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);
    timeval timeout{1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(19345);
    inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
    assert(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    auto command=[&](const char* text) {
        assert(send(fd,text,strlen(text),0)==ssize_t(strlen(text)));
        char response[256];int n=recv(fd,response,sizeof(response),0);assert(n>0);
        return std::string(response,n);
    };
    auto pop=[&]() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cond.wait_for(lock,std::chrono::seconds(1),[&]{return !queue.empty();}));
        auto io=queue.front();queue.pop_front();lock.unlock();cond.notify_all();
        assert(merge_mouse_report(0x81,io));return io;
    };
    assert(command("+mousedown 2")=="ok");assert(pop().data[0]==3);
    physical.data[0]=0;assert(merge_mouse_report(0x81,physical));assert(physical.data[0]==2);
    assert(command("+move 320 -320")=="ok");int x=0,y=0;
    for(int i=0;i<3;++i){auto io=pop();assert(io.data[0]==2);x+=int8_t(io.data[1]);y+=int8_t(io.data[2]);}
    assert(x==320&&y==-320);
    assert(command("+move 4 5 garbage")=="error command");
    assert(command("+click 999999999999999999999999")=="error button");
    assert(command("+wheel 1")=="error not_ready_range_or_queue_full");
    assert(command("+mouseup 2")=="ok");assert(pop().data[0]==0);
    assert(command("+click 1")=="ok");assert(pop().data[0]==1);assert(pop().data[0]==0);
    assert(command("+mousedown 1")=="ok");assert(pop().data[0]==1);
    assert(pop().data[0]==0); // watchdog release after 250 ms with no controller packets
    auto snapshot=[&](uint32_t seq,int dx,uint8_t buttons) {
        uint8_t packet[16]={'U','P','X','1',uint8_t(seq>>24),uint8_t(seq>>16),uint8_t(seq>>8),uint8_t(seq),
            uint8_t(dx>>8),uint8_t(dx),0,0,0,0,buttons,0};
        assert(send(fd,packet,16,0)==16);
        assert(command("+state").find("state ")==0); // receive-loop barrier
    };
    snapshot(1,10,0);snapshot(2,20,0);
    {std::lock_guard<std::mutex> lock(mutex);assert(queue.size()==1);}
    auto newest=pop();assert(newest.data[1]==20);
    snapshot(2,30,0);snapshot(1,40,0);
    {std::lock_guard<std::mutex> lock(mutex);assert(queue.empty());}
    snapshot(3,10,1);snapshot(4,20,0);
    {std::lock_guard<std::mutex> lock(mutex);assert(queue.size()==2);}
    assert(pop().data[0]==1);assert(pop().data[0]==0);
    snapshot(5,50,1);
    {std::lock_guard<std::mutex> lock(mutex);queue.front().injection_deadline_ns=1;}
    auto expired=pop();assert(expired.data[1]==0 && expired.data[0]==1);
    assert(command("+release")=="ok");assert(pop().data[0]==0);
    std::array<uint8_t,24> watch{};std::memcpy(watch.data(),"UPS1",4);
    proxy_telemetry::put64(watch.data()+8,123);proxy_telemetry::put64(watch.data()+16,456);
    assert(send(fd,watch.data(),watch.size(),0)==ssize_t(watch.size()));
    std::array<uint8_t,56> status{};
    assert(recv(fd,status.data(),status.size(),0)==56);
    assert(!std::memcmp(status.data(),"UPT1",4)&&status[4]==1);
    assert(proxy_telemetry::u64(status.data()+8)==123&&proxy_telemetry::u64(status.data()+24)==456);
    physical.data[0]=2;assert(merge_mouse_report(0x81,physical));
    bool observed=false;
    for(int i=0;i<10&&!observed;++i){assert(recv(fd,status.data(),status.size(),0)==56);observed=status[5]==2;}
    assert(observed); // Physical hold appears in telemetry; no injected hold is added.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    while(recv(fd,status.data(),status.size(),MSG_DONTWAIT)>0){}
    timeval short_timeout{0,30000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&short_timeout,sizeof(short_timeout));
    assert(recv(fd,status.data(),status.size(),0)<0); // Subscription expires without renewal.
    server.stop();close(fd);unregister_mouse_endpoint(&info);
    puts("UDP integration: splitting, buttons, timers, watchdog, sequencing and freshness passed");
}
