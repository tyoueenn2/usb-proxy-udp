#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Mirrored in Receiver/include/receiver/move_protocol.hpp. Network byte order.
namespace move_protocol {
constexpr size_t request_size = 80, ack_size = 96, query_size = 24, capability_size = 80;
enum class Operation : uint8_t { Execute = 1, Continue = 2, Cancel = 3, Query = 4 };
enum class Status : uint8_t {
    Accepted = 1,
    Active = 2,
    Completed = 3,
    Busy = 4,
    QueueFull = 5,
    Invalid = 6,
    NotReady = 7,
    Stale = 8,
    ButtonActive = 9,
    Interrupted = 10,
    Cancelled = 11,
    Timeout = 12,
    WriterFailed = 13,
    EndpointChanged = 14,
    Range = 15
};
enum class Stage : uint8_t {
    Idle = 0,
    Out = 1,
    Settle = 2,
    Waiting = 3,
    Press = 4,
    Hold = 5,
    Release = 6,
    Delay = 7,
    Return = 8,
    Done = 9
};
inline bool terminal(Status s) {
    return s != Status::Accepted && s != Status::Active;
}
inline uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t u64(const uint8_t* p) {
    return uint64_t(u32(p)) << 32 | u32(p + 4);
}
inline int32_t i32(const uint8_t* p) {
    uint32_t n = u32(p);
    return n <= 0x7fffffffu ? int32_t(n) : int32_t(int64_t(n) - 0x100000000ll);
}
inline void put32(uint8_t* p, uint32_t n) {
    for (int i = 3; i >= 0; --i) {
        p[i] = uint8_t(n);
        n >>= 8;
    }
}
inline void put64(uint8_t* p, uint64_t n) {
    put32(p, uint32_t(n >> 32));
    put32(p + 4, uint32_t(n));
}
inline bool zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i])
            return false;
    return true;
}
struct Request {
    Operation operation = Operation::Execute;
    uint8_t flags = 0, button = 0;
    uint64_t session = 0, command = 0, epoch = 0, generation = 0;
    int32_t dx = 0, dy = 0;
    uint32_t settle_us = 1000, press_us = 10000, return_us = 0, budget_us = 2000000, wait_us = 1000000;
};
inline std::array<uint8_t, request_size> packet(const Request& r) {
    std::array<uint8_t, request_size> p{};
    std::memcpy(p.data(), "UPM1", 4);
    p[4] = 1;
    p[5] = uint8_t(r.operation);
    p[6] = r.flags;
    p[7] = r.button;
    put64(p.data() + 8, r.session);
    put64(p.data() + 16, r.command);
    put64(p.data() + 24, r.epoch);
    put64(p.data() + 32, r.generation);
    put32(p.data() + 40, uint32_t(r.dx));
    put32(p.data() + 44, uint32_t(r.dy));
    put32(p.data() + 48, r.settle_us);
    put32(p.data() + 52, r.press_us);
    put32(p.data() + 56, r.return_us);
    put32(p.data() + 60, r.budget_us);
    put32(p.data() + 64, r.wait_us);
    return p;
}
inline bool parse(const uint8_t* p, size_t n, Request& r) {
    if (n != request_size || std::memcmp(p, "UPM1", 4) || p[4] != 1 || p[5] < 1 || p[5] > 4 || (p[6] & ~1u) ||
        p[7] > 8 || !zero(p + 68, 12))
        return false;
    r.operation = Operation(p[5]);
    r.flags = p[6];
    r.button = p[7];
    r.session = u64(p + 8);
    r.command = u64(p + 16);
    r.epoch = u64(p + 24);
    r.generation = u64(p + 32);
    r.dx = i32(p + 40);
    r.dy = i32(p + 44);
    r.settle_us = u32(p + 48);
    r.press_us = u32(p + 52);
    r.return_us = u32(p + 56);
    r.budget_us = u32(p + 60);
    r.wait_us = u32(p + 64);
    return r.session && r.command && r.epoch && r.generation && r.dx >= -32767 && r.dx <= 32767 &&
           r.dy >= -32767 && r.dy <= 32767 && r.settle_us <= 1000000 && r.press_us <= 1000000 &&
           r.return_us <= 1000000 && r.budget_us >= 1000 && r.budget_us <= 5000000 && r.wait_us <= 2000000 &&
           (!(r.flags & 1) || r.wait_us > 0);
}
inline bool same(Request a, Request b) {
    a.operation = b.operation = Operation::Execute;
    return packet(a) == packet(b);
}
struct Ack {
    Status status = Status::Invalid;
    Stage stage = Stage::Idle;
    uint8_t flags = 0;
    uint64_t session = 0, command = 0, epoch = 0, generation = 0, written_ns = 0;
    int32_t out_x = 0, out_y = 0, return_x = 0, return_y = 0, residual_x = 0, residual_y = 0;
};
inline std::array<uint8_t, ack_size> packet(const Ack& a) {
    std::array<uint8_t, ack_size> p{};
    std::memcpy(p.data(), "UPR1", 4);
    p[4] = 1;
    p[5] = uint8_t(a.status);
    p[6] = uint8_t(a.stage);
    p[7] = a.flags;
    put64(p.data() + 8, a.session);
    put64(p.data() + 16, a.command);
    put64(p.data() + 24, a.epoch);
    put64(p.data() + 32, a.generation);
    put32(p.data() + 40, uint32_t(a.out_x));
    put32(p.data() + 44, uint32_t(a.out_y));
    put32(p.data() + 48, uint32_t(a.return_x));
    put32(p.data() + 52, uint32_t(a.return_y));
    put32(p.data() + 56, uint32_t(a.residual_x));
    put32(p.data() + 60, uint32_t(a.residual_y));
    put64(p.data() + 64, a.written_ns);
    return p;
}
inline bool parse_ack(const uint8_t* p, size_t n, Ack& a) {
    if (n != ack_size || std::memcmp(p, "UPR1", 4) || p[4] != 1 || p[5] < 1 || p[5] > 15 || p[6] > 9 ||
        (p[7] & ~7u) || !zero(p + 72, 24))
        return false;
    a.status = Status(p[5]);
    a.stage = Stage(p[6]);
    a.flags = p[7];
    a.session = u64(p + 8);
    a.command = u64(p + 16);
    a.epoch = u64(p + 24);
    a.generation = u64(p + 32);
    a.out_x = i32(p + 40);
    a.out_y = i32(p + 44);
    a.return_x = i32(p + 48);
    a.return_y = i32(p + 52);
    a.residual_x = i32(p + 56);
    a.residual_y = i32(p + 60);
    a.written_ns = u64(p + 64);
    return a.session && a.command && a.epoch;
}
struct Capabilities {
    bool ready = false;
    uint8_t buttons = 0;
    uint64_t session = 0, token = 0, epoch = 0, generation = 0, layout = 0;
    uint32_t poll_us = 0, max_segments = 32;
    int32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
};
inline std::array<uint8_t, query_size> query(uint64_t session, uint64_t token) {
    std::array<uint8_t, query_size> p{};
    std::memcpy(p.data(), "UPQ1", 4);
    p[4] = 1;
    put64(p.data() + 8, session);
    put64(p.data() + 16, token);
    return p;
}
inline bool parse_query(const uint8_t* p, size_t n, uint64_t& session, uint64_t& token) {
    if (n != query_size || std::memcmp(p, "UPQ1", 4) || p[4] != 1 || !zero(p + 5, 3))
        return false;
    session = u64(p + 8);
    token = u64(p + 16);
    return session && token;
}
inline std::array<uint8_t, capability_size> packet(const Capabilities& c) {
    std::array<uint8_t, capability_size> p{};
    std::memcpy(p.data(), "UPI1", 4);
    p[4] = 1;
    p[5] = c.ready;
    p[6] = c.buttons;
    put64(p.data() + 8, c.session);
    put64(p.data() + 16, c.token);
    put64(p.data() + 24, c.epoch);
    put64(p.data() + 32, c.generation);
    put32(p.data() + 40, c.poll_us);
    put32(p.data() + 44, uint32_t(c.xmin));
    put32(p.data() + 48, uint32_t(c.xmax));
    put32(p.data() + 52, uint32_t(c.ymin));
    put32(p.data() + 56, uint32_t(c.ymax));
    put64(p.data() + 64, c.layout);
    put32(p.data() + 72, c.max_segments);
    return p;
}
inline bool parse_capabilities(const uint8_t* p, size_t n, Capabilities& c) {
    if (n != capability_size || std::memcmp(p, "UPI1", 4) || p[4] != 1 || p[5] > 1 || p[7] ||
        !zero(p + 60, 4) || !zero(p + 76, 4))
        return false;
    c.ready = p[5];
    c.buttons = p[6];
    c.session = u64(p + 8);
    c.token = u64(p + 16);
    c.epoch = u64(p + 24);
    c.generation = u64(p + 32);
    c.poll_us = u32(p + 40);
    c.xmin = i32(p + 44);
    c.xmax = i32(p + 48);
    c.ymin = i32(p + 52);
    c.ymax = i32(p + 56);
    c.layout = u64(p + 64);
    c.max_segments = u32(p + 72);
    return c.session && c.token && c.epoch &&
           (!c.ready || (c.generation && c.layout && c.poll_us && c.xmin < 0 && c.xmax > 0 && c.ymin < 0 &&
                         c.ymax > 0 && c.xmin >= -32768 && c.xmax <= 32767 && c.ymin >= -32768 &&
                         c.ymax <= 32767 && c.max_segments == 32));
}
inline const char* status_name(Status s) {
    const char* names[] = {"unknown", "accepted",      "active",           "completed",
                           "busy",    "queue_full",    "invalid",          "not_ready",
                           "stale",   "button_active", "interrupted",      "cancelled",
                           "timeout", "writer_failed", "endpoint_changed", "range"};
    return uint8_t(s) <= 15 ? names[uint8_t(s)] : "unknown";
}
} // namespace move_protocol
