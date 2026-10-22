#pragma once
#include "measurement_log.hpp"
#include "mouse_mixer.h"
#include <cerrno>
#include <cstdio>

// Call before starting writer threads so opening a file never delays USB output.
inline measurement::Log& usb_measurement() {
    static measurement::Log log("USB_PROXY_MEASUREMENT_LOG", "pi");
    return log;
}
template<class Report>
inline void measure_usb_write(const Report& io, long long start,
                              long long end, int result, int error) {
    auto& log = usb_measurement();
    if (!log.enabled() || !io.mouse_managed || io.mouse_report_kind < REPORT_TRANSACTION_OUT) return;
    char body[1024];
    std::snprintf(body, sizeof(body),
        "\"event\":\"usb_write\",\"session\":\"%llu\",\"command\":\"%llu\","
        "\"generation\":\"%llu\",\"kind\":%u,\"start_ns\":%lld,\"end_ns\":%lld,"
        "\"duration_ns\":%lld,\"result\":%d,\"expected_bytes\":%u,\"errno\":%d,"
        "\"success\":%s,\"counts\":[%d,%d]",
        (unsigned long long)io.click_session, (unsigned long long)io.click_command,
        (unsigned long long)io.mouse_generation, unsigned(io.mouse_report_kind), start, end,
        end-start, result, unsigned(io.inner.length), error,
        result == int(io.inner.length) ? "true" : "false", io.synthetic_x, io.synthetic_y);
    log.emit(body);
}
