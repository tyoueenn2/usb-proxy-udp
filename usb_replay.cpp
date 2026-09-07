#include "host-raw-gadget.h"
#include "udp_server.h"
#include "replay_profile.h"
#include <memory>
#include <thread>

// Separate executable: no physical USB device or libusb connection is required.
int verbose_level=0;
namespace {
volatile sig_atomic_t stop_requested=0;
void stop_signal(int){stop_requested=1;}
void interrupt_signal(int){}
struct Endpoint {
    thread_info info{};
    std::deque<usb_raw_transfer_io> queue;
    std::mutex mutex;std::condition_variable cond;
    std::thread worker;std::atomic<bool> stop{false};
    std::atomic<unsigned> idle{0};
    int interface_number=0;bool mouse=false;
    MouseProfile profile;
    usb_raw_transfer_io last{};
};
void write_endpoint(Endpoint* e) {
    while(!e->stop) {
        usb_raw_transfer_io io{};bool timed=false;
        {
            std::unique_lock<std::mutex> lock(e->mutex);
            unsigned idle=e->idle.load();
            e->cond.wait_for(lock,std::chrono::milliseconds(idle?idle*4:20),[&]{return e->stop||!e->queue.empty();});
            if(e->stop)break;
            if(e->queue.empty()) {
                if(!idle||!e->last.inner.length)continue;
                io=e->last;timed=true;
            } else {io=e->queue.front();e->queue.pop_front();}
        }
        e->cond.notify_all();
        if(!timed&&!merge_mouse_report(e->info.endpoint.bEndpointAddress,io))continue;
        int result=usb_raw_ep_write(e->info.fd,&io.inner);
        if(result<0)break;
        std::lock_guard<std::mutex> lock(e->mutex);
        e->last=io;
        // GET_REPORT and idle repetition must never replay relative movement.
        for(const auto& f:e->profile.relative)f.put(reinterpret_cast<uint8_t*>(e->last.data),0);
    }
}
void read_endpoint(Endpoint* e) {
    while(!e->stop) {
        usb_raw_transfer_io io{};io.inner.ep=e->info.ep_num;io.inner.length=sizeof(io.data);
        if(usb_raw_ep_read(e->info.fd,&io.inner)<0)break;
        // HID output reports are accepted; vendor-specific device effects are not simulated.
    }
}
}

int main(int argc,char** argv) {
    if(argc<2) {fprintf(stderr,"Usage: usb-replay capture.jsonl [--check | UDC_NAME UDC_DRIVER]\n");return 1;}
    try {
        auto profile=ReplayProfile::load(argv[1]);
        if(argc==3 && std::string(argv[2])=="--check") {
            printf("Valid capture: %zu HID interfaces, %zu endpoints, VID:PID %02x%02x:%02x%02x\n",
                profile.interfaces.size(),profile.endpoints.size(),profile.device[9],profile.device[8],profile.device[11],profile.device[10]);return 0;
        }
        if(argc!=4)throw std::runtime_error("Specify UDC name and driver, or --check");
        struct sigaction action{};action.sa_handler=stop_signal;sigaction(SIGINT,&action,nullptr);sigaction(SIGTERM,&action,nullptr);
        action.sa_handler=interrupt_signal;sigaction(SIGUSR1,&action,nullptr);
        int fd=usb_raw_open();usb_raw_init(fd,static_cast<usb_device_speed>(profile.speed),argv[3],argv[2]);usb_raw_run(fd);
        std::vector<std::unique_ptr<Endpoint>> endpoints;
        std::map<int,unsigned> idle,protocol;
        unsigned configuration=0;bool remote_wakeup=false;
        auto disable=[&]() {
            for(auto& e:endpoints){unregister_mouse_endpoint(&e->info);e->stop=true;e->cond.notify_all();}
            for(auto& e:endpoints) {
                if(e->worker.joinable()){pthread_kill(e->worker.native_handle(),SIGUSR1);e->worker.join();}
                usb_raw_ep_disable(fd,e->info.ep_num);
            }
            endpoints.clear();configuration=0;
        };
        auto enable=[&]() {
            for(auto& i:profile.interfaces) {
                learn_mouse_descriptor(i.number,i.report.data(),i.report.size());set_mouse_protocol(i.number,protocol[i.number]==0);
            }
            for(auto& spec:profile.endpoints) {
                auto e=std::make_unique<Endpoint>();e->interface_number=spec.interface_number;
                memcpy(&e->info.endpoint,spec.descriptor.data(),7);
                e->info.fd=fd;e->info.data_queue=&e->queue;e->info.data_mutex=&e->mutex;e->info.data_cond=&e->cond;
                e->info.ep_num=usb_raw_ep_enable(fd,&e->info.endpoint);
                if(spec.descriptor[2]&0x80) {
                    register_mouse_endpoint(&e->info,e->interface_number);
                    for(auto& i:profile.interfaces)if(i.number==e->interface_number) {
                        auto layouts=parse_mouse_descriptor(i.report.data(),i.report.size());
                        if(!layouts.empty()) {
                            e->mouse=true;e->profile=layouts[0];
                            auto sample=profile.samples.find(spec.descriptor[2]);
                            if(sample!=profile.samples.end())for(auto& layout:layouts)
                                if(layout.matches(sample->second.data(),sample->second.size())){e->profile=layout;break;}
                            if(protocol[i.number]==0) {
                                e->profile=MouseProfile{};e->profile.size=3;
                                e->profile.x={8,8,-127,127};e->profile.y={16,8,-127,127};
                                e->profile.relative={e->profile.x,e->profile.y};
                                for(int b=0;b<3;++b)e->profile.buttons[b+1]={b,1,0,1};
                            }
                            e->idle=idle[i.number];
                            auto& io=e->last;io.inner.ep=e->info.ep_num;io.inner.length=e->profile.size;
                            if(sample!=profile.samples.end()&&e->profile.matches(sample->second.data(),sample->second.size()))
                                memcpy(io.data,sample->second.data(),sample->second.size());
                            if(e->profile.id)io.data[0]=e->profile.id;
                            e->profile.encode(MouseCommand{},reinterpret_cast<uint8_t*>(io.data));
                            merge_mouse_report(spec.descriptor[2],io);
                        }
                    }
                    if(e->mouse)e->worker=std::thread(write_endpoint,e.get());
                } else e->worker=std::thread(read_endpoint,e.get());
                endpoints.push_back(std::move(e));
            }
            configuration=profile.config[5];usb_raw_configure(fd);
            usb_raw_vbus_draw(fd,profile.config[8]);
        };
        UdpServer udp(12345);if(!udp.start()){close(fd);return 1;}
        while(!stop_requested) {
            usb_raw_control_event event{};event.inner.length=sizeof(event.ctrl);usb_raw_event_fetch(fd,&event.inner);
            if(event.inner.length==UINT32_MAX)break;
            if(event.inner.type==USB_RAW_EVENT_RESET||event.inner.type==USB_RAW_EVENT_DISCONNECT) {
                disable();remote_wakeup=false;continue;
            }
            if(event.inner.type!=USB_RAW_EVENT_CONTROL)continue;
            auto& s=event.ctrl;usb_raw_transfer_io io{};
            bool ok=false;std::vector<uint8_t> answer;
            if(s.wLength>sizeof(io.data)){usb_raw_ep0_stall(fd);continue;}
            bool known_interface=false;for(auto& i:profile.interfaces)if(i.number==s.wIndex)known_interface=true;
            if((s.bRequestType==0x80||s.bRequestType==0x81)&&s.bRequest==USB_REQ_GET_DESCRIPTOR) {
                auto bytes=profile.lookup(s.bRequestType,s.wValue,s.wIndex);
                if(bytes){answer=*bytes;ok=true;}
            } else if(s.bRequestType==0x80&&s.bRequest==USB_REQ_GET_CONFIGURATION&&s.wValue==0&&s.wIndex==0) {
                answer={uint8_t(configuration)};ok=true;
            } else if(s.bRequestType==0&&s.bRequest==USB_REQ_SET_CONFIGURATION&&s.wIndex==0&&s.wLength==0&&
                      (s.wValue==0||s.wValue==profile.config[5])) {
                disable();for(auto& i:profile.interfaces){idle[i.number]=0;protocol[i.number]=1;}
                if(s.wValue)enable();ok=true;
            } else if(s.bRequestType==0x81&&s.bRequest==USB_REQ_GET_INTERFACE&&configuration&&known_interface&&s.wValue==0) {
                answer={0};ok=true;
            } else if(s.bRequestType==1&&s.bRequest==USB_REQ_SET_INTERFACE&&configuration&&known_interface&&s.wValue==0&&s.wLength==0) {
                ok=true;
            } else if(s.bRequest==USB_REQ_GET_STATUS&&s.wValue==0&&s.wLength==2) {
                if(s.bRequestType==0x80&&s.wIndex==0){answer={uint8_t(((profile.config[7]&0x40)?1:0)|(remote_wakeup?2:0)),0};ok=true;}
                if(s.bRequestType==0x81&&configuration&&known_interface){answer={0,0};ok=true;}
                if(s.bRequestType==0x82)for(auto& e:endpoints)if(e->info.endpoint.bEndpointAddress==s.wIndex){answer={0,0};ok=true;}
            } else if(s.bRequestType==0&&s.wValue==USB_DEVICE_REMOTE_WAKEUP&&s.wIndex==0&&s.wLength==0&&configuration&&
                      (profile.config[7]&0x20)&&(s.bRequest==USB_REQ_SET_FEATURE||s.bRequest==USB_REQ_CLEAR_FEATURE)) {
                remote_wakeup=s.bRequest==USB_REQ_SET_FEATURE;ok=true;
            } else if(configuration&&known_interface&&(s.bRequestType==0xa1||s.bRequestType==0x21)) {
                if(s.bRequestType==0x21&&s.bRequest==0x0a&&s.wLength==0) { // SET_IDLE
                    bool all=(s.wValue&255)==0;
                    for(auto& e:endpoints)if(e->interface_number==s.wIndex&&e->mouse&&(all||e->profile.id==(s.wValue&255))) {
                        idle[s.wIndex]=s.wValue>>8;e->idle=s.wValue>>8;e->cond.notify_all();ok=true;
                    }
                    if(all){idle[s.wIndex]=s.wValue>>8;ok=true;}
                } else if(s.bRequestType==0xa1&&s.bRequest==2&&(s.wValue>>8)==0) {answer={uint8_t(idle[s.wIndex])};ok=true;}
                else if(s.bRequestType==0xa1&&s.bRequest==3&&s.wValue==0) {answer={uint8_t(protocol[s.wIndex])};ok=true;}
                else if(s.bRequestType==0x21&&s.bRequest==0x0b&&s.wLength==0&&s.wValue<=1) {
                    for(auto& i:profile.interfaces)if(i.number==s.wIndex&&i.subclass==1&&i.protocol==2) {
                        disable();protocol[s.wIndex]=s.wValue;enable();ok=true;break;
                    }
                }
                else if(s.bRequestType==0xa1&&s.bRequest==1&&(s.wValue>>8)==1) {
                    for(auto& e:endpoints)if(e->interface_number==s.wIndex&&e->mouse&&e->profile.id==(s.wValue&255)) {
                        std::lock_guard<std::mutex> lock(e->mutex);
                        answer.assign(e->last.data,e->last.data+e->last.inner.length);ok=true;
                    }
                }
            }
            if(!ok){usb_raw_ep0_stall(fd);continue;}
            if(s.bRequestType&USB_DIR_IN) {
                io.inner.length=std::min<size_t>(s.wLength,answer.size());memcpy(io.data,answer.data(),io.inner.length);
                usb_raw_ep0_write(fd,&io.inner);
            } else {io.inner.length=s.wLength;usb_raw_ep0_read(fd,&io.inner);}
        }
        udp.stop();disable();close(fd);return 0;
    } catch(const std::exception& e) {fprintf(stderr,"Replay: %s\n",e.what());return 1;}
}
