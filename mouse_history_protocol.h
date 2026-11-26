#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mouse_history {
constexpr size_t samples = 164;
constexpr size_t query_size = 24;
constexpr size_t reply_size = 48 + samples * 4;
constexpr uint64_t tick_ns = 1000000;
constexpr uint8_t guarded_assist = 1;
using Trace = std::array<std::array<int, 2>, samples>;

struct Reply {
    uint64_t session = 0, token = 0, epoch = 0, generation = 0, end_ms = 0;
    Trace trace{};
};

inline void put64(uint8_t* p, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        p[i] = uint8_t(value);
        value >>= 8;
    }
}
inline uint64_t get64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | p[i];
    return value;
}
inline std::array<uint8_t, query_size> query(uint64_t session, uint64_t token) {
    std::array<uint8_t, query_size> p{};
    std::memcpy(p.data(), "UPS3HIS1", 8);
    put64(p.data() + 8, session);
    put64(p.data() + 16, token);
    return p;
}
inline bool parse_query(const uint8_t* p, size_t n, uint64_t& session, uint64_t& token) {
    if (n != query_size || std::memcmp(p, "UPS3HIS1", 8))
        return false;
    session = get64(p + 8);
    token = get64(p + 16);
    return session && token;
}
inline std::array<uint8_t, reply_size> packet(const Reply& reply) {
    std::array<uint8_t, reply_size> p{};
    std::memcpy(p.data(), "UPH1", 4);
    size_t offset = 8;
    for (auto value : {reply.session, reply.token, reply.epoch, reply.generation, reply.end_ms}) {
        put64(p.data() + offset, value);
        offset += 8;
    }
    for (const auto& delta : reply.trace) {
        for (int counts : delta) {
            const auto value = uint16_t(counts);
            p[offset++] = uint8_t(value >> 8);
            p[offset++] = uint8_t(value);
        }
    }
    return p;
}
inline bool parse_reply(const uint8_t* p, size_t n, Reply& reply) {
    if (n != reply_size || std::memcmp(p, "UPH1", 4) || p[4] || p[5] || p[6] || p[7])
        return false;
    reply.session = get64(p + 8);
    reply.token = get64(p + 16);
    reply.epoch = get64(p + 24);
    reply.generation = get64(p + 32);
    reply.end_ms = get64(p + 40);
    if (!reply.session || !reply.token || !reply.epoch || !reply.generation || reply.end_ms < samples)
        return false;
    size_t offset = 48;
    for (auto& delta : reply.trace) {
        for (int& counts : delta) {
            const int value = int(p[offset]) << 8 | p[offset + 1];
            counts = value < 0x8000 ? value : value - 0x10000;
            offset += 2;
        }
    }
    return true;
}
} // namespace mouse_history
