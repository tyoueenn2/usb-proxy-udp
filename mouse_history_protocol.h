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
constexpr size_t steering_size = 64;
constexpr uint64_t steering_lifetime_ms = 20;
constexpr uint16_t steering_max_angle_cdeg = 9000;
using Trace = std::array<std::array<int, 2>, samples>;

struct Reply {
    uint64_t session = 0, token = 0, epoch = 0, generation = 0, end_ms = 0;
    Trace trace{};
};

enum class SteeringMode : uint8_t { directional, inner, hold };

struct Steering {
    uint64_t session = 0, token = 0, epoch = 0, generation = 0;
    uint64_t sample_ms = 0, expires_ms = 0;
    uint16_t angle_cdeg = 0;
    SteeringMode mode = SteeringMode::directional;
    std::array<int, 2> aim{}, tangent{};
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
inline std::array<uint8_t, steering_size> packet(const Steering& steering) {
    std::array<uint8_t, steering_size> p{};
    std::memcpy(p.data(), "UPJ1", 4);
    p[4] = uint8_t(steering.angle_cdeg >> 8);
    p[5] = uint8_t(steering.angle_cdeg);
    p[6] = uint8_t(steering.mode);
    size_t offset = 8;
    for (auto value : {steering.session, steering.token, steering.epoch, steering.generation,
                       steering.sample_ms, steering.expires_ms}) {
        put64(p.data() + offset, value);
        offset += 8;
    }
    for (int counts : {steering.aim[0], steering.aim[1], steering.tangent[0], steering.tangent[1]}) {
        const auto value = uint16_t(counts);
        p[offset++] = uint8_t(value >> 8);
        p[offset++] = uint8_t(value);
    }
    return p;
}
inline bool parse_steering(const uint8_t* p, size_t n, Steering& result) {
    if (n != steering_size || std::memcmp(p, "UPJ1", 4) || p[6] > uint8_t(SteeringMode::hold) || p[7])
        return false;
    result = {};
    result.angle_cdeg = uint16_t(unsigned(p[4]) << 8 | p[5]);
    result.mode = SteeringMode(p[6]);
    result.session = get64(p + 8);
    result.token = get64(p + 16);
    result.epoch = get64(p + 24);
    result.generation = get64(p + 32);
    if (!result.session || !result.token || !result.epoch || !result.generation)
        return false;
    result.sample_ms = get64(p + 40);
    result.expires_ms = get64(p + 48);
    size_t offset = 56;
    for (auto* vector : {&result.aim, &result.tangent}) {
        for (int& counts : *vector) {
            const int value = int(p[offset]) << 8 | p[offset + 1];
            counts = value < 0x8000 ? value : value - 0x10000;
            if (counts < -10000 || counts > 10000)
                return false;
            offset += 2;
        }
    }
    const bool aim = result.aim != std::array<int, 2>{};
    const bool tangent = result.tangent != std::array<int, 2>{};
    if (!result.angle_cdeg)
        return result.mode == SteeringMode::directional && !result.sample_ms && !result.expires_ms &&
               !aim && !tangent;
    if (result.angle_cdeg < 50 || result.angle_cdeg > steering_max_angle_cdeg || !result.sample_ms ||
        result.expires_ms <= result.sample_ms || result.expires_ms - result.sample_ms > steering_lifetime_ms)
        return false;
    if (result.mode == SteeringMode::hold)
        return !aim && !tangent;
    if (result.mode == SteeringMode::inner)
        return int64_t(result.aim[0]) * result.tangent[0] + int64_t(result.aim[1]) * result.tangent[1] > 0;
    return aim && tangent;
}
} // namespace mouse_history
