#include "receiver/protocol.hpp"
#include <algorithm>
#include <limits>
namespace receiver {
std::optional<FrameHeader> parse_frame(Bytes p) {
    if (p.size() < 48 || p.size() > 1400 || std::memcmp(p.data(), "UVF1", 4) || p[4] != 1 ||
        be16(p.data() + 6) != 48 || be16(p.data() + 46) != 0)
        return {};
    const auto* b = p.data();
    FrameHeader h;
    h.format = b[5];
    h.session = be64(b + 8);
    h.sequence = be32(b + 16);
    auto stamp = be64(b + 20);
    if (stamp > uint64_t(std::numeric_limits<int64_t>::max()))
        return {};
    h.capture_ns = int64_t(stamp);
    h.width = be16(b + 28);
    h.height = be16(b + 30);
    h.bytes = be32(b + 32);
    h.index = be16(b + 36);
    h.count = be16(b + 38);
    h.offset = be32(b + 40);
    h.stride = be16(b + 44);
    if (!h.session || !h.width || !h.height || h.width > 1024 || h.height > 1024 ||
        (h.format != 1 && h.format != 2))
        return {};
    const uint32_t expected = uint32_t(h.width) * h.height * (h.format == 1 ? 3 : 4);
    if (h.bytes != expected || h.stride < 1024 || h.stride > 1352 ||
        h.count != (h.bytes + h.stride - 1) / h.stride || h.count > max_fragments || h.index >= h.count ||
        h.offset != uint32_t(h.index) * h.stride)
        return {};
    if (p.size() != 48 + std::min<uint32_t>(h.stride, h.bytes - h.offset))
        return {};
    return h;
}
std::array<uint8_t, 48> encode_frame(const FrameHeader& h) {
    std::array<uint8_t, 48> p{};
    std::memcpy(p.data(), "UVF1", 4);
    p[4] = 1;
    p[5] = h.format;
    put16(p.data() + 6, 48);
    put64(p.data() + 8, h.session);
    put32(p.data() + 16, h.sequence);
    put64(p.data() + 20, uint64_t(h.capture_ns));
    put16(p.data() + 28, h.width);
    put16(p.data() + 30, h.height);
    put32(p.data() + 32, h.bytes);
    put16(p.data() + 36, h.index);
    put16(p.data() + 38, h.count);
    put32(p.data() + 40, h.offset);
    put16(p.data() + 44, h.stride);
    return p;
}
Reassembler::Reassembler() {
    for (auto& p : pool_)
        p = std::make_shared<Frame>();
}
void Reassembler::reset(uint64_t session) {
    for (auto& s : slots_)
        s = {};
    session_ = session;
    have_completed_ = false;
}
void Reassembler::expire(int64_t now) {
    for (auto& s : slots_)
        if (s.frame && now - s.frame->first_ns > 20'000'000) {
            s = {};
            ++stats.expired;
        }
}
std::shared_ptr<const Frame> Reassembler::accept(Bytes p, int64_t now) {
    ++stats.packets;
    expire(now);
    auto parsed = parse_frame(p);
    if (!parsed) {
        ++stats.invalid;
        return {};
    }
    auto h = *parsed;
    if (h.session != session_ || (have_completed_ && !newer(h.sequence, completed_))) {
        ++stats.old;
        return {};
    }
    Slot* slot = nullptr;
    for (auto& s : slots_)
        if (s.frame && s.frame->header.sequence == h.sequence) {
            slot = &s;
            break;
        }
    if (!slot) {
        for (auto& s : slots_)
            if (!s.frame) {
                slot = &s;
                break;
            }
        if (!slot) {
            slot = &*std::min_element(slots_.begin(), slots_.end(),
                                      [](auto& a, auto& b) { return a.frame->first_ns < b.frame->first_ns; });
            *slot = {};
            ++stats.evicted;
        }
        for (auto& f : pool_)
            if (f.use_count() == 1) {
                slot->frame = f;
                break;
            }
        if (!slot->frame) {
            ++stats.pool_drops;
            return {};
        }
        slot->frame->header = h;
        slot->frame->first_ns = now;
    }
    auto canonical = h;
    canonical.index = slot->frame->header.index;
    canonical.offset = slot->frame->header.offset;
    if (canonical != slot->frame->header) {
        *slot = {};
        ++stats.invalid;
        return {};
    }
    auto* dst = slot->frame->pixels.data() + h.offset;
    if (slot->seen[h.index]) {
        if (std::memcmp(dst, p.data() + 48, p.size() - 48)) {
            *slot = {};
            ++stats.invalid;
        } else
            ++stats.duplicates;
        return {};
    }
    std::memcpy(dst, p.data() + 48, p.size() - 48);
    slot->seen[h.index] = 1;
    ++slot->received;
    if (slot->received != h.count)
        return {};
    auto result = slot->frame;
    result->complete_ns = now;
    completed_ = h.sequence;
    have_completed_ = true;
    ++stats.completed;
    for (auto& s : slots_)
        if (s.frame && !newer(s.frame->header.sequence, completed_))
            s = {};
    return result;
}
bool ClockSync::observe(int64_t t0, int64_t t1, int64_t t2, int64_t t3) {
    if (t0 < 0 || t1 < 0 || t2 < t1 || t3 < t0 || t3 - t0 > 50'000'000 || t2 - t1 > t3 - t0)
        return false;
    const auto lo = t2 - t3, hi = t1 - t0;
    if (lo > hi)
        return false;
    lower_ = lo;
    upper_ = hi;
    updated_ = t3;
    return true;
}
std::optional<int64_t> ClockSync::age_upper(int64_t capture, int64_t now) const {
    if (!synchronized(now))
        return {};
    // 200 ppm relative drift allowance + 0.1 ms timestamp margin.
    const auto margin = (now - updated_) / 5000 + 100'000;
    long double age = static_cast<long double>(now) - capture + upper_ + margin;
    if (age < 0 || age > std::numeric_limits<int64_t>::max())
        return {};
    return int64_t(age);
}
std::array<uint8_t, 24> subscribe(uint64_t client, uint64_t token) {
    std::array<uint8_t, 24> p{};
    std::memcpy(p.data(), "UPS1", 4);
    put64(p.data() + 8, client);
    put64(p.data() + 16, token);
    return p;
}
std::optional<Telemetry> parse_telemetry(Bytes p) {
    if (p.size() != 56 || std::memcmp(p.data(), "UPT1", 4) || p[4] > 1 || p[6] || p[7] || be32(p.data() + 52))
        return {};
    Telemetry t;
    t.ready = p[4] != 0;
    t.physical = p[5];
    t.client = be64(p.data() + 8);
    t.server = be64(p.data() + 16);
    t.token = be64(p.data() + 24);
    t.sequence = be32(p.data() + 32);
    auto signed32 = [](const uint8_t* b) {
        uint32_t u = be32(b);
        return u <= 0x7fffffffu ? int(u) : int(int64_t(u) - 0x100000000ll);
    };
    t.xmin = signed32(p.data() + 36);
    t.xmax = signed32(p.data() + 40);
    t.ymin = signed32(p.data() + 44);
    t.ymax = signed32(p.data() + 48);
    if (!t.client || !t.server || t.xmin > 0 || t.ymin > 0 || t.xmax < 0 || t.ymax < 0 || t.xmin < -32768 ||
        t.ymin < -32768 || t.xmax > 32767 || t.ymax > 32767)
        return {};
    if (t.ready && (t.xmin == t.xmax || t.ymin == t.ymax))
        return {};
    return t;
}
std::array<uint8_t, 16> movement(uint32_t sequence, int dx, int dy) {
    std::array<uint8_t, 16> p{};
    std::memcpy(p.data(), "UPX1", 4);
    put32(p.data() + 4, sequence);
    put16(p.data() + 8, uint16_t(std::clamp(dx, -32768, 32767)));
    put16(p.data() + 10, uint16_t(std::clamp(dy, -32768, 32767)));
    return p;
}
void TelemetryGate::reset(uint64_t c) {
    client_ = c;
    server_ = 0;
    have_ = false;
    received_ = issued_ = 0;
    tokens_ = {};
    next_ = 0;
    state = {};
}
void TelemetryGate::issue(uint64_t token, int64_t time) {
    tokens_[next_++ % tokens_.size()] = {token, time};
}
bool TelemetryGate::accept(Bytes p, int64_t time) {
    auto t = parse_telemetry(p);
    if (!t || t->client != client_)
        return false;
    int64_t issued = 0;
    for (auto [token, at] : tokens_)
        if (token && token == t->token && time >= at && time - at <= 50'000'000)
            issued = at;
    if (!issued || issued < issued_)
        return false;
    if (have_ && t->server == server_ && !newer(t->sequence, sequence_))
        return false;
    // A fresh token is required to change server epochs, preventing an older epoch replay.
    if (have_ && t->server != server_ && issued <= issued_)
        return false;
    state = *t;
    server_ = t->server;
    sequence_ = t->sequence;
    issued_ = issued;
    received_ = time;
    have_ = true;
    return true;
}
bool TelemetryGate::fresh(int64_t time) const {
    return have_ && state.ready && time >= received_ && time - received_ <= 50'000'000 && time >= issued_ &&
           time - issued_ <= 50'000'000;
}
} // namespace receiver
