#pragma once
#include "mouse_protocol.h"
#include <cstdint>
#include <string>
#include <vector>

struct thread_info;
struct usb_raw_transfer_io;

enum ReportKind:uint8_t {
    REPORT_NONE,
    REPORT_MOVEMENT,
    REPORT_PERSISTENT,
    REPORT_CLICK_PRESS,
    REPORT_CLICK_RELEASE,
    REPORT_RELEASE_ALL
};

enum class EnqueueResult {Ok,NotReady,Unsupported,QueueFull};

struct EndpointSnapshot {
    bool ready=false;
    uint8_t physical=0,persistent=0,scheduled=0,supported=0;
    int xmin=0,xmax=0,ymin=0,ymax=0,physical_dx=0,physical_dy=0;
    uint32_t poll_us=1000,physical_age_us=0xffffffffu;
    uint32_t physical_received=0,physical_submitted=0,superseded=0,writer_failures=0;
    uint16_t output_queue=0,synthetic_pending=0;
    uint64_t generation=0,snapshot_ns=0,last_motion_ns=0,p99_residence_ns=0;
    int64_t cumulative_x=0,cumulative_y=0;
};

struct WriterEvent {
    uint64_t session=0,command=0,generation=0,written_ns=0;
    uint32_t index=0;
    uint8_t kind=0,button=0,blocked=0,final_buttons=0;
    bool success=false;
};

void register_mouse_endpoint(thread_info* info,int interface_number);
void learn_mouse_descriptor(int interface_number,const uint8_t* data,unsigned length);
void set_mouse_protocol(int interface_number,bool boot);
void unregister_mouse_endpoint(thread_info* info);

void queue_physical_mouse_report(uint8_t endpoint,usb_raw_transfer_io& io);
void seed_mouse_report(uint8_t endpoint,usb_raw_transfer_io& io);
bool take_mouse_report(uint8_t endpoint,usb_raw_transfer_io& io);
void notify_mouse_report_written(uint8_t endpoint,const usb_raw_transfer_io& io,bool success);

EndpointSnapshot endpoint_snapshot();
std::string mouse_state_text();
EnqueueResult enqueue_report(MouseCommand command,uint8_t persistent,uint8_t scheduled,
                             ReportKind kind,bool latest,uint64_t session=0,uint64_t command_id=0,
                             uint32_t click_index=0,uint8_t click_button=0);
EndpointSnapshot clear_synthetic_state();
std::vector<WriterEvent> take_writer_events();
bool take_writer_overflow();

#ifdef MOUSE_MIXER_TEST_STUBS
// Deterministic unit-test clock. Passing nullptr restores steady_clock.
void set_mouse_mixer_clock_for_test(uint64_t (*clock_fn)());
#endif
