#include "mouse_mixer.h"
#ifdef MOUSE_MIXER_TEST_STUBS
#include "tests/mixer_test_stubs.h"
#else
#include "host-raw-gadget.h"
#endif
#include "usb_capture.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>

namespace {
using Clock=std::chrono::steady_clock;
constexpr size_t MAX_SYNTHETIC_ITEMS=8;
constexpr size_t MAX_WRITER_EVENTS=32;
constexpr size_t MAX_RESIDENCE_SAMPLES=1024;
#ifdef MOUSE_MIXER_TEST_STUBS
uint64_t (*test_clock)()=nullptr;
#endif

uint64_t monotonic_ns() {
#ifdef MOUSE_MIXER_TEST_STUBS
    if(test_clock)return test_clock();
#endif
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
uint32_t saturate32(uint64_t value) {
    return uint32_t(std::min<uint64_t>(value,std::numeric_limits<uint32_t>::max()));
}
uint16_t saturate16(size_t value) {
    return uint16_t(std::min<size_t>(value,std::numeric_limits<uint16_t>::max()));
}
int64_t saturating_add(int64_t value,int delta) {
    const int64_t change=delta;
    if(change>0&&value>std::numeric_limits<int64_t>::max()-change)return std::numeric_limits<int64_t>::max();
    if(change<0&&value<std::numeric_limits<int64_t>::min()-change)return std::numeric_limits<int64_t>::min();
    return value+delta;
}
bool no_motion(const MouseCommand& command) {
    return !command.x&&!command.y&&!command.wheel&&!command.pan;
}

struct SyntheticItem {
    uint64_t id=0,generation=0,deadline_ns=0,physical_barrier=0;
    uint64_t session=0,command_id=0;
    uint32_t click_index=0;
    MouseCommand remaining;
    uint8_t persistent=0,scheduled=0,click_button=0;
    ReportKind kind=REPORT_NONE;
    bool latest=false,buttons_committed=false;
};

struct Endpoint {
    thread_info* info=nullptr;
    int interface_number=0;
    MouseProfile profile;
    std::vector<uint8_t> latest,submitted_template;
    std::deque<SyntheticItem> synthetic;
    std::deque<uint64_t> residence_ns;
    uint8_t physical_buttons=0,submitted_physical_buttons=0;
    uint8_t persistent_buttons=0,scheduled_buttons=0;
    int physical_dx=0,physical_dy=0;
    int64_t cumulative_x=0,cumulative_y=0;
    uint64_t generation=0,instance=0,next_item=1,physical_barrier=0,last_motion_ns=0;
    uint64_t physical_received=0,physical_submitted=0,superseded=0,writer_failures=0;
    uint8_t physical_successes_since_standalone=0;
    bool inflight=false;
    uint64_t inflight_id=0;
};

std::mutex registry_mutex;
uint64_t next_generation=0;
uint64_t next_endpoint_instance=0;
std::map<int,Endpoint> endpoints;
std::map<int,std::vector<MouseProfile>> descriptors;
std::map<int,std::vector<MouseProfile>> report_descriptors;
std::map<int,bool> boot_protocol;
int selected=-1;

std::mutex event_mutex;
std::deque<WriterEvent> writer_events;
bool writer_overflow=false;

void update_pending(Endpoint& endpoint) {
    if(endpoint.info&&endpoint.info->mouse_synthetic_pending)
        endpoint.info->mouse_synthetic_pending->store(!endpoint.synthetic.empty(),std::memory_order_release);
}
MouseProfile* matching_profile(Endpoint& endpoint,const uint8_t* data,size_t size) {
    for(auto& profile:descriptors[endpoint.interface_number])
        if(profile.matches(data,size)&&(endpoint.latest.empty()||profile.id==endpoint.profile.id))return &profile;
    return nullptr;
}
bool field_fits(const HidField& field,int physical,int synthetic) {
    if(!synthetic)return true;
    if(!field.bits)return false;
    int64_t result=int64_t(physical)+synthetic;
    return result>=field.minimum&&result<=field.maximum;
}
bool motion_fits(const MouseProfile& profile,const usb_raw_transfer_io& io,const MouseCommand& command) {
    return field_fits(profile.x,io.physical_x,command.x)&&
        field_fits(profile.y,io.physical_y,command.y)&&
        field_fits(profile.wheel,io.physical_wheel,command.wheel)&&
        field_fits(profile.pan,io.physical_pan,command.pan);
}
void apply_known_fields(const MouseProfile& profile,usb_raw_transfer_io& io,
                        int x,int y,int wheel,int pan,uint8_t buttons) {
    auto* bytes=reinterpret_cast<uint8_t*>(io.data);
    profile.x.put(bytes,x);profile.y.put(bytes,y);
    profile.wheel.put(bytes,wheel);profile.pan.put(bytes,pan);
    profile.put_buttons(bytes,buttons);
}
void describe_synthetic(Endpoint& endpoint,SyntheticItem& item,usb_raw_transfer_io& io,
                        const MouseCommand& applied,bool apply_buttons,bool standalone,bool competing) {
    io.mouse_managed=1;io.mouse_generation=endpoint.generation;io.synthetic_item_id=item.id;
    io.mouse_endpoint_instance=endpoint.instance;
    io.click_session=item.session;io.click_command=item.command_id;io.click_index=item.click_index;
    io.click_button=item.click_button;io.mouse_report_kind=item.kind;
    io.persistent_buttons=item.persistent;io.scheduled_buttons=item.scheduled;
    io.synthetic_x=applied.x;io.synthetic_y=applied.y;
    io.synthetic_wheel=applied.wheel;io.synthetic_pan=applied.pan;
    io.synthetic_apply_buttons=apply_buttons;io.mouse_standalone=standalone;
    io.mouse_competing_standalone=competing;
    if(item.kind==REPORT_CLICK_PRESS&&item.click_button) {
        uint8_t previously_visible=endpoint.submitted_physical_buttons|
            endpoint.persistent_buttons|endpoint.scheduled_buttons;
        io.click_blocked=(previously_visible&(1u<<(item.click_button-1)))!=0;
    }
    endpoint.inflight=true;endpoint.inflight_id=item.id;
}
void remove_finished_front(Endpoint& endpoint) {
    while(!endpoint.synthetic.empty()) {
        auto& item=endpoint.synthetic.front();
        if(no_motion(item.remaining)&&item.buttons_committed)endpoint.synthetic.pop_front();
        else break;
    }
    update_pending(endpoint);
}
void expire_front(Endpoint& endpoint,uint64_t now) {
    while(!endpoint.synthetic.empty()) {
        auto& item=endpoint.synthetic.front();
        if(item.deadline_ns&&now>item.deadline_ns) {
            item.remaining.x=item.remaining.y=item.remaining.wheel=item.remaining.pan=0;
        }
        bool needs_buttons=!item.buttons_committed&&
            (item.persistent!=endpoint.persistent_buttons||item.scheduled!=endpoint.scheduled_buttons||
             item.kind==REPORT_RELEASE_ALL);
        if(no_motion(item.remaining)&&!needs_buttons)endpoint.synthetic.pop_front();
        else break;
    }
    update_pending(endpoint);
}

// Caller holds registry_mutex. Invalidate synthetic state encoded for the prior layout.
void invalidate_interface(int number) {
    for(auto& pair:endpoints)if(pair.second.interface_number==number) {
        auto& endpoint=pair.second;
        endpoint.latest.clear();endpoint.submitted_template.clear();endpoint.synthetic.clear();
        endpoint.persistent_buttons=endpoint.scheduled_buttons=0;
        endpoint.inflight=false;endpoint.inflight_id=0;endpoint.generation=++next_generation;
        update_pending(endpoint);if(endpoint.info)endpoint.info->data_cond->notify_all();
    }
}
}

void learn_mouse_descriptor(int number,const uint8_t* data,unsigned length) {
    auto profiles=parse_mouse_descriptor(data,length);std::lock_guard<std::mutex> lock(registry_mutex);
    report_descriptors[number]=profiles;
    if(!boot_protocol[number]){descriptors[number]=std::move(profiles);invalidate_interface(number);}
}
void set_mouse_protocol(int number,bool boot) {
    std::lock_guard<std::mutex> lock(registry_mutex);boot_protocol[number]=boot;
    if(boot) {
        MouseProfile profile;profile.size=3;profile.x={8,8,-127,127};profile.y={16,8,-127,127};
        profile.relative={profile.x,profile.y};
        for(int i=0;i<3;++i)profile.buttons[i+1]={i,1,0,1};
        descriptors[number]={profile};
    } else descriptors[number]=report_descriptors[number];
    invalidate_interface(number);
}
void register_mouse_endpoint(thread_info* info,int number) {
    std::lock_guard<std::mutex> lock(registry_mutex);Endpoint endpoint;
    endpoint.info=info;endpoint.interface_number=number;endpoint.generation=++next_generation;
    endpoint.instance=++next_endpoint_instance;
    if(!info->mouse_poll_interval_us)info->mouse_poll_interval_us=1000;
    info->mouse_endpoint=true;
    if(info->mouse_synthetic_pending)info->mouse_synthetic_pending->store(false,std::memory_order_release);
    endpoints[info->endpoint.bEndpointAddress]=std::move(endpoint);
}
void unregister_mouse_endpoint(thread_info* info) {
    std::lock_guard<std::mutex> lock(registry_mutex);int address=info->endpoint.bEndpointAddress;
    auto found=endpoints.find(address);
    if(found!=endpoints.end()) {
        found->second.synthetic.clear();found->second.persistent_buttons=found->second.scheduled_buttons=0;
        update_pending(found->second);endpoints.erase(found);++next_generation;
    }
    info->mouse_endpoint=false;
    if(info->mouse_synthetic_pending)info->mouse_synthetic_pending->store(false,std::memory_order_release);
    if(selected==address)selected=-1;
}

void queue_physical_mouse_report(uint8_t address,usb_raw_transfer_io& io) {
    std::lock_guard<std::mutex> lock(registry_mutex);auto found=endpoints.find(address);
    if(found==endpoints.end())return;
    auto& endpoint=found->second;auto* bytes=reinterpret_cast<uint8_t*>(io.data);
    io.mouse_managed=1;io.mouse_physical=1;io.physical_queued_ns=monotonic_ns();
    io.mouse_generation=endpoint.generation;io.mouse_endpoint_instance=endpoint.instance;++endpoint.physical_barrier;
    auto* profile=matching_profile(endpoint,bytes,io.inner.length);
    if(!profile)return;
    if(endpoint.latest.empty()) {
        endpoint.profile=*profile;capture_report(address,endpoint.interface_number,bytes,io.inner.length);
    }
    endpoint.latest.assign(bytes,bytes+io.inner.length);io.mouse_physical_match=1;
    io.physical_buttons=profile->button_mask(bytes);io.physical_x=profile->x.get(bytes);io.physical_y=profile->y.get(bytes);
    io.physical_wheel=profile->wheel.get(bytes);io.physical_pan=profile->pan.get(bytes);
    endpoint.physical_buttons=io.physical_buttons;endpoint.physical_dx=io.physical_x;endpoint.physical_dy=io.physical_y;
    endpoint.cumulative_x=saturating_add(endpoint.cumulative_x,io.physical_x);
    endpoint.cumulative_y=saturating_add(endpoint.cumulative_y,io.physical_y);
    if(io.physical_x||io.physical_y)endpoint.last_motion_ns=io.physical_queued_ns;
    ++endpoint.physical_received;if(selected<0)selected=address;
}

void seed_mouse_report(uint8_t address,usb_raw_transfer_io& io) {
    std::lock_guard<std::mutex> lock(registry_mutex);auto found=endpoints.find(address);
    if(found==endpoints.end())return;
    auto& endpoint=found->second;auto* bytes=reinterpret_cast<uint8_t*>(io.data);
    auto* profile=matching_profile(endpoint,bytes,io.inner.length);if(!profile)return;
    endpoint.profile=*profile;endpoint.latest.assign(bytes,bytes+io.inner.length);
    for(const auto& field:endpoint.profile.relative)field.put(endpoint.latest.data(),0);
    endpoint.profile.put_buttons(endpoint.latest.data(),0);endpoint.submitted_template=endpoint.latest;
    std::memcpy(io.data,endpoint.latest.data(),endpoint.latest.size());endpoint.physical_buttons=0;
    endpoint.submitted_physical_buttons=0;if(selected<0)selected=address;
}

EndpointSnapshot endpoint_snapshot() {
    std::lock_guard<std::mutex> lock(registry_mutex);EndpointSnapshot snapshot;
    snapshot.generation=next_generation;snapshot.snapshot_ns=monotonic_ns();
    auto found=endpoints.find(selected);if(found==endpoints.end()||found->second.latest.empty())return snapshot;
    auto& endpoint=found->second;snapshot.ready=true;snapshot.physical=endpoint.physical_buttons;
    snapshot.persistent=endpoint.persistent_buttons;snapshot.scheduled=endpoint.scheduled_buttons;
    snapshot.xmin=endpoint.profile.x.minimum;snapshot.xmax=endpoint.profile.x.maximum;
    snapshot.ymin=endpoint.profile.y.minimum;snapshot.ymax=endpoint.profile.y.maximum;
    snapshot.physical_dx=endpoint.physical_dx;snapshot.physical_dy=endpoint.physical_dy;
    snapshot.poll_us=std::max<uint32_t>(1,endpoint.info->mouse_poll_interval_us);snapshot.generation=endpoint.generation;
    snapshot.physical_received=saturate32(endpoint.physical_received);
    snapshot.physical_submitted=saturate32(endpoint.physical_submitted);
    snapshot.synthetic_pending=saturate16(endpoint.synthetic.size());snapshot.superseded=saturate32(endpoint.superseded);
    snapshot.writer_failures=saturate32(endpoint.writer_failures);snapshot.cumulative_x=endpoint.cumulative_x;
    snapshot.cumulative_y=endpoint.cumulative_y;snapshot.last_motion_ns=endpoint.last_motion_ns;
    if(endpoint.last_motion_ns&&snapshot.snapshot_ns>=endpoint.last_motion_ns)
        snapshot.physical_age_us=saturate32((snapshot.snapshot_ns-endpoint.last_motion_ns)/1000);
    for(const auto& button:endpoint.profile.buttons)snapshot.supported|=uint8_t(1u<<(button.first-1));
    if(endpoint.info&&endpoint.info->data_queue&&endpoint.info->data_mutex) {
        std::lock_guard<std::mutex> queue_lock(*endpoint.info->data_mutex);
        snapshot.output_queue=saturate16(endpoint.info->data_queue->size());
    }
    if(!endpoint.residence_ns.empty()) {
        std::vector<uint64_t> samples(endpoint.residence_ns.begin(),endpoint.residence_ns.end());
        size_t index=(samples.size()*99+99)/100-1;std::nth_element(samples.begin(),samples.begin()+index,samples.end());
        snapshot.p99_residence_ns=samples[index];
    }
    return snapshot;
}

std::string mouse_state_text() {
    auto snapshot=endpoint_snapshot();if(!snapshot.ready)return "not_ready";
    uint8_t synthetic=snapshot.persistent|snapshot.scheduled;
    return "state "+std::to_string(snapshot.physical)+" "+std::to_string(synthetic)+" "+
        std::to_string(snapshot.physical|synthetic)+" "+std::to_string(snapshot.xmin)+" "+
        std::to_string(snapshot.xmax)+" "+std::to_string(snapshot.ymin)+" "+std::to_string(snapshot.ymax);
}

EnqueueResult enqueue_report(MouseCommand command,uint8_t persistent,uint8_t scheduled,
                             ReportKind kind,bool latest,uint64_t session,uint64_t command_id,
                             uint32_t click_index,uint8_t click_button) {
    std::lock_guard<std::mutex> lock(registry_mutex);auto found=endpoints.find(selected);
    if(found==endpoints.end()||found->second.latest.empty())return EnqueueResult::NotReady;
    auto& endpoint=found->second;command.buttons=persistent|scheduled;
    if(split_mouse_command(endpoint.profile,command).empty())return EnqueueResult::Unsupported;
    if(latest)while(!endpoint.synthetic.empty()) {
        const auto& tail=endpoint.synthetic.back();
        if((endpoint.inflight&&tail.id==endpoint.inflight_id)||!tail.latest||tail.kind!=REPORT_MOVEMENT||
           tail.persistent!=persistent||tail.physical_barrier!=endpoint.physical_barrier)break;
        endpoint.synthetic.pop_back();++endpoint.superseded;
    }
    if(endpoint.synthetic.size()>=MAX_SYNTHETIC_ITEMS)return EnqueueResult::QueueFull;
    SyntheticItem item;item.id=endpoint.next_item++;item.generation=endpoint.generation;
    item.deadline_ns=latest?monotonic_ns()+25000000:0;item.physical_barrier=endpoint.physical_barrier;
    item.session=session;item.command_id=command_id;item.click_index=click_index;item.remaining=command;
    item.persistent=persistent;item.scheduled=scheduled;item.click_button=click_button;
    item.kind=kind;item.latest=latest;endpoint.synthetic.push_back(item);update_pending(endpoint);
    endpoint.info->data_cond->notify_all();return EnqueueResult::Ok;
}

EndpointSnapshot clear_synthetic_state() {
    std::lock_guard<std::mutex> lock(registry_mutex);EndpointSnapshot result;
    result.generation=next_generation;result.snapshot_ns=monotonic_ns();auto found=endpoints.find(selected);
    if(found==endpoints.end())return result;
    auto& endpoint=found->second;endpoint.synthetic.clear();endpoint.inflight=false;endpoint.inflight_id=0;
    // The scheduler clears desired state immediately. These masks describe the last
    // successful USB write and become zero only when the final release completes.
    update_pending(endpoint);endpoint.info->data_cond->notify_all();result.ready=!endpoint.latest.empty();
    result.physical=endpoint.physical_buttons;result.generation=endpoint.generation;
    result.poll_us=std::max<uint32_t>(1,endpoint.info->mouse_poll_interval_us);return result;
}

bool take_mouse_report(uint8_t address,usb_raw_transfer_io& io) {
    std::lock_guard<std::mutex> lock(registry_mutex);auto found=endpoints.find(address);
    if(found==endpoints.end())return false;auto& endpoint=found->second;
    if(endpoint.inflight)return false;uint64_t now=monotonic_ns();expire_front(endpoint,now);
    std::lock_guard<std::mutex> queue_lock(*endpoint.info->data_mutex);auto& physical=*endpoint.info->data_queue;
    bool have_physical=!physical.empty(),have_synthetic=!endpoint.synthetic.empty();
    if(!have_physical&&!have_synthetic)return false;

    bool fuse=false,buttons_only=false,standalone=false,competing=false;
    if(have_synthetic&&have_physical) {
        auto& item=endpoint.synthetic.front();const auto& candidate=physical.front();
        bool compatible=candidate.mouse_physical_match&&candidate.mouse_generation==endpoint.generation;
        fuse=compatible&&motion_fits(endpoint.profile,candidate,item.remaining);
        buttons_only=compatible&&!item.buttons_committed&&
            (item.persistent!=endpoint.persistent_buttons||item.scheduled!=endpoint.scheduled_buttons||
             item.kind==REPORT_RELEASE_ALL);
        if(physical.size()==1&&!fuse&&!buttons_only&&!endpoint.submitted_template.empty()&&
           endpoint.physical_successes_since_standalone>=3)standalone=competing=true;
    } else if(have_synthetic)standalone=true;

    if(!standalone&&have_physical) {
        io=physical.front();physical.pop_front();endpoint.info->data_cond->notify_all();
        io.mouse_managed=1;io.mouse_physical=1;
        bool compatible=io.mouse_physical_match&&io.mouse_generation==endpoint.generation;
        uint8_t persistent=endpoint.persistent_buttons,scheduled=endpoint.scheduled_buttons;
        MouseCommand applied;
        if(have_synthetic&&compatible&&(fuse||buttons_only)) {
            auto& item=endpoint.synthetic.front();if(fuse)applied=item.remaining;
            persistent=item.persistent;scheduled=item.scheduled;
            describe_synthetic(endpoint,item,io,applied,true,false,false);
        }
        if(compatible) {
            int x=io.physical_x+applied.x,y=io.physical_y+applied.y;
            int wheel=io.physical_wheel+applied.wheel,pan=io.physical_pan+applied.pan;
            io.mouse_final_buttons=io.physical_buttons|persistent|scheduled;
            apply_known_fields(endpoint.profile,io,x,y,wheel,pan,io.mouse_final_buttons);
        }
        return true;
    }

    auto& item=endpoint.synthetic.front();const auto& base=endpoint.submitted_template.empty()?endpoint.latest:endpoint.submitted_template;
    if(base.empty())return false;io={};io.inner.ep=endpoint.info->ep_num;io.inner.length=endpoint.profile.size;
    std::memcpy(io.data,base.data(),base.size());
    for(const auto& field:endpoint.profile.relative)field.put(reinterpret_cast<uint8_t*>(io.data),0);
    MouseCommand command=item.remaining;command.buttons=item.persistent|item.scheduled;
    auto parts=split_mouse_command(endpoint.profile,command);if(parts.empty())return false;
    MouseCommand applied=parts.front();endpoint.profile.encode(applied,reinterpret_cast<uint8_t*>(io.data));
    io.mouse_final_buttons=endpoint.submitted_physical_buttons|item.persistent|item.scheduled;
    endpoint.profile.put_buttons(reinterpret_cast<uint8_t*>(io.data),io.mouse_final_buttons);
    describe_synthetic(endpoint,item,io,applied,true,true,competing);return true;
}

void notify_mouse_report_written(uint8_t address,const usb_raw_transfer_io& io,bool success) {
    if(!io.mouse_managed)return;uint64_t written=monotonic_ns();bool emit=false;WriterEvent event;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);auto found=endpoints.find(address);
        if(found==endpoints.end())return;
        auto& endpoint=found->second;if(io.mouse_endpoint_instance!=endpoint.instance)return;
        bool current_layout=io.mouse_generation==endpoint.generation;
        if(!success&&current_layout)++endpoint.writer_failures;
        if(success&&io.mouse_physical) {
            endpoint.physical_successes_since_standalone=std::min<uint8_t>(3,endpoint.physical_successes_since_standalone+1);
            if(io.mouse_physical_match) {
                ++endpoint.physical_submitted;endpoint.submitted_physical_buttons=io.physical_buttons;
                if(current_layout) {
                    usb_raw_transfer_io physical=io;
                    apply_known_fields(endpoint.profile,physical,io.physical_x,io.physical_y,
                                       io.physical_wheel,io.physical_pan,io.physical_buttons);
                    endpoint.submitted_template.assign(reinterpret_cast<const uint8_t*>(physical.data),
                                                       reinterpret_cast<const uint8_t*>(physical.data)+physical.inner.length);
                }
                if(io.physical_queued_ns&&written>=io.physical_queued_ns) {
                    endpoint.residence_ns.push_back(written-io.physical_queued_ns);
                    if(endpoint.residence_ns.size()>MAX_RESIDENCE_SAMPLES)endpoint.residence_ns.pop_front();
                }
            }
        }
        if(success&&io.mouse_competing_standalone)endpoint.physical_successes_since_standalone=0;
        if(current_layout&&io.synthetic_item_id&&endpoint.inflight&&endpoint.inflight_id==io.synthetic_item_id&&
           !endpoint.synthetic.empty()&&endpoint.synthetic.front().id==io.synthetic_item_id) {
            auto& item=endpoint.synthetic.front();
            if(success) {
                if(io.synthetic_apply_buttons) {
                    endpoint.persistent_buttons=io.persistent_buttons;endpoint.scheduled_buttons=io.scheduled_buttons;
                    item.buttons_committed=true;
                }
                item.remaining.x-=io.synthetic_x;item.remaining.y-=io.synthetic_y;
                item.remaining.wheel-=io.synthetic_wheel;item.remaining.pan-=io.synthetic_pan;
            }
            endpoint.inflight=false;endpoint.inflight_id=0;if(success)remove_finished_front(endpoint);else update_pending(endpoint);
        }
        if(current_layout&&(io.mouse_report_kind==REPORT_CLICK_PRESS||io.mouse_report_kind==REPORT_CLICK_RELEASE||
           io.mouse_report_kind==REPORT_RELEASE_ALL||!success)) {
            event={io.click_session,io.click_command,io.mouse_generation,written,io.click_index,
                   io.mouse_report_kind,io.click_button,io.click_blocked,io.mouse_final_buttons,success};emit=true;
        }
    }
    if(emit) {
        std::lock_guard<std::mutex> lock(event_mutex);
        if(writer_events.size()>=MAX_WRITER_EVENTS)writer_overflow=true;else writer_events.push_back(event);
    }
}

std::vector<WriterEvent> take_writer_events() {
    std::lock_guard<std::mutex> lock(event_mutex);
    std::vector<WriterEvent> result(writer_events.begin(),writer_events.end());writer_events.clear();return result;
}
bool take_writer_overflow() {
    std::lock_guard<std::mutex> lock(event_mutex);bool result=writer_overflow;writer_overflow=false;return result;
}

#ifdef MOUSE_MIXER_TEST_STUBS
void set_mouse_mixer_clock_for_test(uint64_t (*clock_fn)()) {
    std::lock_guard<std::mutex> lock(registry_mutex);test_clock=clock_fn;
}
#endif
