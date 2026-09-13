#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

constexpr size_t MAX_TRANSFER_SIZE=4096;
struct usb_raw_ep_io {uint16_t ep=0,flags=0;uint32_t length=0;};
struct usb_endpoint_descriptor {uint8_t bEndpointAddress=0;};
struct usb_raw_transfer_io {
    usb_raw_ep_io inner{};char data[MAX_TRANSFER_SIZE]{};
    uint64_t injection_deadline_ns=0,mouse_generation=0,click_session=0,click_command=0;
    uint64_t synthetic_item_id=0,physical_queued_ns=0;uint32_t click_index=0;
    int32_t synthetic_x=0,synthetic_y=0,synthetic_wheel=0,synthetic_pan=0;
    int32_t physical_x=0,physical_y=0,physical_wheel=0,physical_pan=0;
    uint8_t persistent_buttons=0,scheduled_buttons=0,physical_buttons=0,mouse_report_kind=0;
    uint8_t click_button=0,click_blocked=0,mouse_final_buttons=0,mouse_managed=0;
    uint8_t mouse_physical=0,mouse_physical_match=0,mouse_standalone=0;
    uint8_t mouse_competing_standalone=0,synthetic_apply_buttons=0;
};
struct thread_info {
    int ep_num=0;usb_endpoint_descriptor endpoint{};
    std::deque<usb_raw_transfer_io>* data_queue=nullptr;std::mutex* data_mutex=nullptr;
    std::condition_variable* data_cond=nullptr;uint32_t mouse_poll_interval_us=0;
    bool mouse_endpoint=false;std::atomic<bool>* mouse_synthetic_pending=nullptr;
};
