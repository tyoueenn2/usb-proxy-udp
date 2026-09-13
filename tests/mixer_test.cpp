#ifdef MOUSE_MIXER_TEST_STUBS
#include "mixer_test_stubs.h"
#else
#include "host-raw-gadget.h"
#endif
#include "mouse_mixer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef MOUSE_MIXER_TEST_STUBS
void capture_report(unsigned,unsigned,const void*,unsigned){}
#endif
namespace {
uint64_t fake_now=1000000000ull;
uint64_t fake_clock(){return fake_now;}

std::vector<uint8_t> report_descriptor() {
    return {
        0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,
        0x05,0x09,0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x75,0x01,
        0x95,0x05,0x81,0x02,0x75,0x03,0x95,0x01,0x81,0x01,
        0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7f,0x75,0x08,
        0x95,0x02,0x81,0x06,
        0x09,0x38,0x95,0x01,0x81,0x06,
        0x05,0x0c,0x0a,0x38,0x02,0x95,0x01,0x81,0x06,
        0x06,0x00,0xff,0x09,0x01,0x15,0x00,0x26,0xff,0x00,0x75,0x08,
        0x95,0x01,0x81,0x02,0xc0,0xc0
    };
}

struct Fixture {
    static constexpr uint8_t address=0x81;
    thread_info info{};
    std::deque<usb_raw_transfer_io> physical_queue;
    std::mutex mutex;std::condition_variable cond;std::atomic<bool> pending{false};
    uint64_t received=0;

    explicit Fixture(uint32_t poll_us=1000) {
        auto descriptor=report_descriptor();learn_mouse_descriptor(7,descriptor.data(),descriptor.size());
        info.ep_num=1;info.endpoint.bEndpointAddress=address;info.data_queue=&physical_queue;
        info.data_mutex=&mutex;info.data_cond=&cond;info.mouse_poll_interval_us=poll_us;
        info.mouse_synthetic_pending=&pending;register_mouse_endpoint(&info,7);
        push_physical(0,0,0,0xa0);auto initial=take();complete(initial);
        take_writer_events();take_writer_overflow();
    }
    ~Fixture(){clear_synthetic_state();unregister_mouse_endpoint(&info);take_writer_events();take_writer_overflow();}

    void push_physical(uint8_t buttons,int x,int y,uint8_t vendor) {
        usb_raw_transfer_io io{};io.inner.ep=1;io.inner.length=7;
        io.data[0]=2;io.data[1]=char(buttons);io.data[2]=char(x);io.data[3]=char(y);
        io.data[4]=0;io.data[5]=0;io.data[6]=char(vendor);
        queue_physical_mouse_report(address,io);++received;
        std::lock_guard<std::mutex> lock(mutex);physical_queue.push_back(io);
    }
    usb_raw_transfer_io take() {
        usb_raw_transfer_io io{};assert(take_mouse_report(address,io));return io;
    }
    void complete(const usb_raw_transfer_io& io,bool success=true,uint64_t advance_ns=0) {
        fake_now+=advance_ns;notify_mouse_report_written(address,io,success);
    }
    static int x(const usb_raw_transfer_io& io){return int8_t(io.data[2]);}
    static int y(const usb_raw_transfer_io& io){return int8_t(io.data[3]);}
    static int wheel(const usb_raw_transfer_io& io){return int8_t(io.data[4]);}
    static int pan(const usb_raw_transfer_io& io){return int8_t(io.data[5]);}
    static uint8_t buttons(const usb_raw_transfer_io& io){return uint8_t(io.data[1])&0x1f;}
    static uint8_t vendor(const usb_raw_transfer_io& io){return uint8_t(io.data[6]);}
    void enqueue(MouseCommand command,uint8_t persistent=0,uint8_t scheduled=0,
                 ReportKind kind=REPORT_MOVEMENT,bool latest=false,uint64_t command_id=0,
                 uint32_t index=0,uint8_t button=0) {
        assert(enqueue_report(command,persistent,scheduled,kind,latest,9,command_id,index,button)==EnqueueResult::Ok);
    }
};

void physical_order_and_fusion() {
    Fixture f;MouseCommand movement;movement.x=10;f.enqueue(movement,0,0,REPORT_MOVEMENT,true);
    f.push_physical(2,5,-2,0xa1);auto fused=f.take();
    assert(fused.mouse_physical&&Fixture::x(fused)==15&&Fixture::y(fused)==-2);
    assert(Fixture::buttons(fused)==2&&fused.data[0]==2&&Fixture::vendor(fused)==0xa1);f.complete(fused);

    for(int i=1;i<=3;++i)f.push_physical(uint8_t(i&1),i,-i,uint8_t(0xb0+i));
    for(int i=1;i<=3;++i) {
        auto io=f.take();assert(io.mouse_physical&&Fixture::x(io)==i&&Fixture::y(io)==-i);
        assert(Fixture::vendor(io)==uint8_t(0xb0+i)&&io.data[0]==2);f.complete(io);
    }
    auto s=endpoint_snapshot();assert(s.physical_received==5&&s.physical_submitted==5);
    assert(s.physical_dx==3&&s.physical_dy==-3&&s.cumulative_x==11&&s.cumulative_y==-8);
}

void independent_button_masks_and_edges() {
    Fixture f;f.push_physical(2,0,0,1);auto right=f.take();f.complete(right);
    f.enqueue({},1,0,REPORT_PERSISTENT);auto persistent=f.take();
    assert(!persistent.mouse_physical&&Fixture::buttons(persistent)==3);f.complete(persistent);
    f.push_physical(0,0,0,2);auto physical_release=f.take();
    assert(Fixture::buttons(physical_release)==1);f.complete(physical_release);

    f.push_physical(2,120,0,3);f.push_physical(2,0,0,4);
    MouseCommand overflow;overflow.x=20;f.enqueue(overflow,0,0,REPORT_PERSISTENT);
    auto release=f.take();assert(release.mouse_physical&&Fixture::x(release)==120&&Fixture::buttons(release)==2);
    f.complete(release);auto retained=f.take();assert(Fixture::x(retained)==20&&Fixture::buttons(retained)==2);f.complete(retained);
    auto s=endpoint_snapshot();assert(s.physical==2&&s.persistent==0&&s.scheduled==0);

    f.push_physical(0,0,0,5);auto up=f.take();f.complete(up);
    f.enqueue({},2,0,REPORT_PERSISTENT);auto hold=f.take();f.complete(hold);
    f.enqueue({},2,1,REPORT_CLICK_PRESS,false,10,0,1);auto press=f.take();
    assert(Fixture::buttons(press)==3&&!press.click_blocked);f.complete(press);
    MouseCommand move;move.x=7;f.enqueue(move,2,1,REPORT_MOVEMENT,true);
    f.push_physical(0,2,0,6);auto during=f.take();assert(Fixture::x(during)==9&&Fixture::buttons(during)==3);f.complete(during);
    f.enqueue({},2,0,REPORT_CLICK_RELEASE,false,10,0,1);auto click_release=f.take();
    assert(Fixture::buttons(click_release)==2);f.complete(click_release);
    s=endpoint_snapshot();assert(s.physical==0&&s.persistent==2&&s.scheduled==0);
}

void overflow_supersession_failure_and_capacity() {
    Fixture f;MouseCommand one;one.x=1;MouseCommand two;two.x=2;
    f.enqueue(one,0,0,REPORT_MOVEMENT,true);f.enqueue(two,0,0,REPORT_MOVEMENT,true);
    assert(endpoint_snapshot().superseded==1&&endpoint_snapshot().synthetic_pending==1);
    auto newest=f.take();assert(Fixture::x(newest)==2);f.complete(newest);

    MouseCommand three;three.x=3;MouseCommand four;four.x=4;
    f.enqueue(three,0,0,REPORT_MOVEMENT,true);f.push_physical(0,1,0,0x21);
    f.enqueue(four,0,0,REPORT_MOVEMENT,true);assert(endpoint_snapshot().synthetic_pending==2);
    auto before_barrier=f.take();assert(before_barrier.mouse_physical&&Fixture::x(before_barrier)==4);f.complete(before_barrier);
    auto after_barrier=f.take();assert(Fixture::x(after_barrier)==4);f.complete(after_barrier);

    f.enqueue({},1,0,REPORT_PERSISTENT);auto failed=f.take();assert(Fixture::buttons(failed)==1);f.complete(failed,false);
    auto state=endpoint_snapshot();assert(state.persistent==0&&state.synthetic_pending==1&&state.writer_failures==1);
    auto retry=f.take();assert(Fixture::buttons(retry)==1);f.complete(retry);
    assert(endpoint_snapshot().persistent==1);

    clear_synthetic_state();
}

void supersession_barriers() {
    auto verify=[](MouseCommand barrier,uint8_t persistent,uint8_t scheduled,ReportKind kind,
                   uint64_t command_id=0,uint8_t button=0) {
        Fixture f;MouseCommand before;before.x=1;MouseCommand after;after.x=2;
        f.enqueue(before,0,0,REPORT_MOVEMENT,true);
        f.enqueue(barrier,persistent,scheduled,kind,false,command_id,0,button);
        f.enqueue(after,persistent,scheduled,REPORT_MOVEMENT,true);
        assert(endpoint_snapshot().synthetic_pending==3);
        auto first=f.take();assert(Fixture::x(first)==1);f.complete(first);
        auto middle=f.take();
        assert(middle.mouse_report_kind==kind&&Fixture::wheel(middle)==barrier.wheel&&
               Fixture::pan(middle)==barrier.pan);
        f.complete(middle);
        auto last=f.take();assert(Fixture::x(last)==2);f.complete(last);
    };

    verify({},1,0,REPORT_PERSISTENT); // persistent button transition
    MouseCommand wheel;wheel.wheel=1;verify(wheel,0,0,REPORT_MOVEMENT);
    MouseCommand pan;pan.pan=-1;verify(pan,0,0,REPORT_MOVEMENT);
    MouseCommand ordered;ordered.x=6;verify(ordered,0,0,REPORT_MOVEMENT); // ASCII-style ordered move
    verify({},0,1,REPORT_CLICK_PRESS,40,1);

    Fixture release;release.enqueue({},0,1,REPORT_CLICK_PRESS,false,41,0,1);
    auto initial_press=release.take();release.complete(initial_press);take_writer_events();
    MouseCommand before;before.x=1;MouseCommand after;after.x=2;
    release.enqueue(before,0,1,REPORT_MOVEMENT,true);
    release.enqueue({},0,0,REPORT_CLICK_RELEASE,false,41,0,1);
    release.enqueue(after,0,0,REPORT_MOVEMENT,true);
    assert(endpoint_snapshot().synthetic_pending==3);
    auto first=release.take();assert(Fixture::x(first)==1);release.complete(first);
    auto edge=release.take();assert(edge.mouse_report_kind==REPORT_CLICK_RELEASE);release.complete(edge);
    auto last=release.take();assert(Fixture::x(last)==2);release.complete(last);
}

void bounded_queue_and_cleanup() {
    Fixture f;
    for(int i=0;i<8;++i){MouseCommand command;command.x=i+1;f.enqueue(command,0,0,REPORT_MOVEMENT,false);}
    MouseCommand ninth;ninth.x=9;
    assert(enqueue_report(ninth,0,0,REPORT_MOVEMENT,false)==EnqueueResult::QueueFull);
    auto first=f.take();f.complete(first);
    assert(enqueue_report(ninth,0,0,REPORT_MOVEMENT,false)==EnqueueResult::Ok);
    clear_synthetic_state();assert(endpoint_snapshot().persistent==0&&endpoint_snapshot().scheduled==0&&
                                   endpoint_snapshot().synthetic_pending==0);
    f.enqueue({},0,1,REPORT_CLICK_PRESS,false,20,0,1);auto press=f.take();f.complete(press);
    assert(endpoint_snapshot().scheduled==1);clear_synthetic_state();
    f.enqueue({},0,0,REPORT_RELEASE_ALL);auto release=f.take();assert(Fixture::buttons(release)==0);f.complete(release,false);
    assert(endpoint_snapshot().scheduled==1);auto retry_release=f.take();f.complete(retry_release);
    assert(endpoint_snapshot().persistent==0&&endpoint_snapshot().scheduled==0);
}

void physical_completion_survives_synthetic_reset() {
    Fixture f;
    f.push_physical(2,0,0,1);auto held=f.take();f.complete(held);
    auto generation=endpoint_snapshot().generation;

    // A queued physical release remains valid when synthetic work is cancelled.
    f.push_physical(0,0,0,2);clear_synthetic_state();
    assert(endpoint_snapshot().generation==generation);
    f.enqueue({},0,0,REPORT_RELEASE_ALL);
    auto physical_release=f.take();assert(physical_release.mouse_physical&&Fixture::buttons(physical_release)==0);
    bool fused_release=physical_release.mouse_report_kind==REPORT_RELEASE_ALL;f.complete(physical_release);
    usb_raw_transfer_io final_release{};
    if(!fused_release) {
        final_release=f.take();assert(!final_release.mouse_physical&&Fixture::buttons(final_release)==0);
        f.complete(final_release);
    }

    // The same accounting holds when the physical report was already taken by the writer.
    f.push_physical(2,0,0,3);auto held_again=f.take();f.complete(held_again);
    f.push_physical(0,0,0,4);auto inflight_release=f.take();
    clear_synthetic_state();f.enqueue({},0,0,REPORT_RELEASE_ALL);
    f.complete(inflight_release);
    final_release=f.take();assert(Fixture::buttons(final_release)==0);f.complete(final_release);

    // A failed physical release must preserve the last host-visible physical hold.
    f.push_physical(2,0,0,5);auto visible_hold=f.take();f.complete(visible_hold);
    f.push_physical(0,0,0,6);auto failed_release=f.take();
    clear_synthetic_state();f.enqueue({},0,0,REPORT_RELEASE_ALL);f.complete(failed_release,false);
    auto preserve_hold=f.take();assert(Fixture::buttons(preserve_hold)==2);f.complete(preserve_hold);
    f.push_physical(0,0,0,7);auto recovered_release=f.take();
    assert(Fixture::buttons(recovered_release)==0);f.complete(recovered_release);
    f.enqueue({},0,0,REPORT_RELEASE_ALL);final_release=f.take();
    assert(Fixture::buttons(final_release)==0);f.complete(final_release);
}

void physical_completion_survives_layout_change() {
    Fixture f;
    f.push_physical(2,0,0,1);auto held=f.take();f.complete(held);

    // A genuine report already submitted to USB remains part of the ordered
    // host-visible state even if the HID protocol changes before completion.
    f.push_physical(0,0,0,2);auto old_layout_release=f.take();
    auto old_generation=old_layout_release.mouse_generation;set_mouse_protocol(7,true);
    assert(endpoint_snapshot().generation!=old_generation);f.complete(old_layout_release);

    // Re-establish report protocol and a current template. The final synthetic
    // release must use the completed physical release rather than resurrecting
    // the prior right-button hold.
    set_mouse_protocol(7,false);f.push_physical(0,0,0,3);auto current=f.take();f.complete(current);
    f.enqueue({},0,0,REPORT_RELEASE_ALL);auto final_release=f.take();
    assert(Fixture::buttons(final_release)==0);f.complete(final_release);
}

void stale_endpoint_completion_is_ignored() {
    Fixture f;
    f.push_physical(2,0,0,1);auto old_endpoint_hold=f.take();
    unregister_mouse_endpoint(&f.info);register_mouse_endpoint(&f.info,7);
    f.push_physical(0,0,0,2);auto replacement_release=f.take();f.complete(replacement_release);

    // A completion from an endpoint registration that has already been replaced
    // cannot overwrite the replacement endpoint's host-visible physical state.
    f.complete(old_endpoint_hold);f.enqueue({},0,0,REPORT_RELEASE_ALL);
    auto final_release=f.take();assert(Fixture::buttons(final_release)==0);f.complete(final_release);
}

void bounded_completion_events() {
    Fixture f;
    for(uint32_t index=0;index<20;++index) {
        f.enqueue({},0,1,REPORT_CLICK_PRESS,false,30,index,1);auto press=f.take();f.complete(press);
        f.enqueue({},0,0,REPORT_CLICK_RELEASE,false,30,index,1);auto release=f.take();f.complete(release);
    }
    auto events=take_writer_events();assert(events.size()==32&&take_writer_overflow());
    assert(!take_writer_overflow());
}

void cadence_fairness_and_residence(uint32_t poll_us) {
    Fixture f(poll_us);MouseCommand large;large.x=1000;f.enqueue(large,0,0,REPORT_MOVEMENT,false);
    unsigned physical_outputs=0,standalone_outputs=0;
    for(unsigned opportunity=0;opportunity<16;++opportunity) {
        bool empty;{std::lock_guard<std::mutex> lock(f.mutex);empty=f.physical_queue.empty();}
        if(empty)f.push_physical(0,127,0,uint8_t(opportunity));
        auto io=f.take();
        if(io.mouse_physical){++physical_outputs;assert(Fixture::x(io)==127);}
        else {++standalone_outputs;assert(io.mouse_standalone);}
        f.complete(io,true,uint64_t(poll_us)*1000);
    }
    assert(standalone_outputs<=4&&physical_outputs>=12);
    auto s=endpoint_snapshot();assert(s.p99_residence_ns<=uint64_t(poll_us)*2000);
    assert(s.physical_submitted+f.physical_queue.size()==f.received);
}
}

int main() {
    set_mouse_mixer_clock_for_test(fake_clock);
    physical_order_and_fusion();independent_button_masks_and_edges();
    overflow_supersession_failure_and_capacity();supersession_barriers();
    bounded_queue_and_cleanup();physical_completion_survives_synthetic_reset();
    physical_completion_survives_layout_change();stale_endpoint_completion_is_ignored();
    bounded_completion_events();
    cadence_fairness_and_residence(8000);cadence_fairness_and_residence(1000);cadence_fairness_and_residence(125);
    set_mouse_mixer_clock_for_test(nullptr);
    std::cout<<"Mixer ordering, fusion, masks, overflow, retries, bounds, fairness and fake cadences passed\n";
}
