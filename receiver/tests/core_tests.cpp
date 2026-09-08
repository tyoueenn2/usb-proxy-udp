#include "receiver/app.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace receiver;
static int checks = 0;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string("Line ") + std::to_string(__LINE__) + ": " #x);             \
    } while (false)
std::vector<std::vector<uint8_t>> packets(uint32_t seq, int w = 160, int h = 160, uint8_t fmt = 1,
                                          uint64_t session = 7) {
    FrameHeader header;
    header.session = session;
    header.sequence = seq;
    header.capture_ns = 1000;
    header.width = uint16_t(w);
    header.height = uint16_t(h);
    header.format = fmt;
    header.bytes = uint32_t(w * h * (fmt == 1 ? 3 : 4));
    header.stride = 1352;
    header.count = uint16_t((header.bytes + 1351) / 1352);
    std::vector<std::vector<uint8_t>> result;
    for (uint16_t i = 0; i < header.count; ++i) {
        header.index = i;
        header.offset = uint32_t(i) * 1352;
        auto encoded = encode_frame(header);
        std::vector<uint8_t> p(encoded.begin(), encoded.end());
        for (uint32_t j = 0; j < std::min<uint32_t>(1352, header.bytes - header.offset); ++j)
            p.push_back(uint8_t((j + header.offset) % 251));
        result.push_back(std::move(p));
    }
    return result;
}
std::array<uint8_t, 56> status(uint64_t client, uint64_t token, uint32_t seq, uint64_t server = 9,
                               bool ready = true, uint8_t physical = 2) {
    std::array<uint8_t, 56> p{};
    std::memcpy(p.data(), "UPT1", 4);
    p[4] = ready;
    p[5] = physical;
    put64(p.data() + 8, client);
    put64(p.data() + 16, server);
    put64(p.data() + 24, token);
    put32(p.data() + 32, seq);
    put32(p.data() + 36, uint32_t(-127));
    put32(p.data() + 40, 127);
    put32(p.data() + 44, uint32_t(-127));
    put32(p.data() + 48, 127);
    return p;
}
int main() {
    try {
        CHECK(newer(0, 0xffffffff));
        CHECK(!newer(5, 5));
        CHECK(!newer(4, 5));
        CHECK(!newer(0x80000000, 0));
        auto move = movement(0x12345678, -32768, 32767);
        CHECK(move[0] == 'U' && move[3] == '1' && move[4] == 0x12 && move[7] == 0x78 && move[8] == 0x80 &&
              move[9] == 0 && move[10] == 0x7f && move[11] == 0xff && move[14] == 0 && move[15] == 0);
        Reassembler r;
        r.reset(7);
        auto p = packets(1);
        CHECK(p.front().size() == 1400);
        std::shared_ptr<const Frame> completed;
        for (auto it = p.rbegin(); it != p.rend(); ++it) {
            auto f = r.accept(*it, 1'000'000);
            if (f)
                completed = f;
        }
        CHECK(completed && completed->header.width == 160);
        for (size_t i = 0; i < completed->header.bytes; ++i)
            CHECK(completed->pixels[i] == i % 251);
        CHECK(!r.accept(p[0], 2'000'000));
        CHECK(r.stats.old == 1);
        auto p2 = packets(2);
        CHECK(!r.accept(p2[0], 3'000'000));
        CHECK(!r.accept(p2[0], 4'000'000));
        CHECK(r.stats.duplicates == 1);
        p2[0][48] ^= 1;
        CHECK(!r.accept(p2[0], 5'000'000));
        CHECK(r.stats.invalid == 1);
        auto malformed = packets(3);
        put32(malformed[0].data() + 40, 1);
        CHECK(!parse_frame(malformed[0]));
        malformed = packets(3);
        malformed[0].push_back(0);
        CHECK(!parse_frame(malformed[0]));
        malformed = packets(3);
        put16(malformed[0].data() + 28, 1025);
        CHECK(!parse_frame(malformed[0]));
        malformed = packets(3);
        put16(malformed[0].data() + 38, 1);
        CHECK(!parse_frame(malformed[0]));
        auto p3 = packets(3);
        r.accept(p3[0], 6'000'000);
        r.expire(27'000'001);
        CHECK(r.stats.expired == 1);
        for (uint32_t s = 4; s < 8; ++s)
            r.accept(packets(s)[0], 30'000'000 + s);
        CHECK(r.stats.evicted >= 1);
        auto last = packets(8, 320, 160, 2);
        for (auto& packet : last) {
            auto f = r.accept(packet, 31'000'000);
            if (f)
                completed = f;
        }
        CHECK(completed->header.sequence == 8 && completed->header.format == 2);
        CHECK(completed->pixels[42] == 42);
        r.reset(8);
        CHECK(!r.accept(last[0], 32'000'000));
        auto restart = packets(0, 160, 160, 1, 8);
        for (auto& packet : restart) {
            auto f = r.accept(packet, 33'000'000);
            if (f)
                completed = f;
        }
        CHECK(completed->header.session == 8 && completed->header.sequence == 0);
        // Shape metadata disagreement invalidates the in-flight frame.
        r.reset(7);
        auto conflict = packets(1);
        r.accept(conflict[0], 100);
        put64(conflict[1].data() + 20, 2000);
        auto before = r.stats.invalid;
        r.accept(conflict[1], 101);
        CHECK(r.stats.invalid == before + 1);
        // Inference/preview-held buffers cannot be recycled underneath their readers.
        completed.reset();
        std::vector<std::shared_ptr<const Frame>> held;
        r.reset(7);
        for (int s = 1; s <= 7; ++s) {
            for (auto& packet : packets(uint32_t(s))) {
                auto f = r.accept(packet, 1000 + s);
                if (f)
                    held.push_back(f);
            }
        }
        CHECK(held.size() == 7);
        r.accept(packets(8)[0], 2000);
        CHECK(r.stats.pool_drops > 0);
        CHECK(held.front()->pixels[100] == 100);
        // Fuzz malformed datagrams; memory use is fixed independently of input sizes.
        std::mt19937 rng(42);
        for (int i = 0; i < 10000; ++i) {
            std::vector<uint8_t> fuzz(rng() % 1600);
            for (auto& v : fuzz)
                v = uint8_t(rng());
            r.accept(fuzz, 3000 + i);
        }
        ClockSync clock;
        CHECK(!clock.age_upper(100, 200));
        CHECK(clock.observe(1'000'000'000, 1'101'000'000, 1'101'100'000, 1'002'100'000));
        auto age = clock.age_upper(1'105'000'000, 1'010'000'000);
        CHECK(age && *age >= 6'000'000 && *age < 7'000'000);
        CHECK(!clock.observe(100, 50, 49, 200));
        CHECK(!clock.age_upper(100, 5'000'000'000));
        clock.reset();
        CHECK(!clock.age_upper(100, 200));
        TelemetryGate gate;
        gate.reset(7);
        gate.issue(100, 1'000'000);
        CHECK(gate.accept(status(7, 100, 0xffffffff), 2'000'000));
        CHECK(gate.fresh(2'000'000));
        CHECK(gate.accept(status(7, 100, 0), 3'000'000));
        CHECK(!gate.accept(status(7, 100, 0), 4'000'000));
        CHECK(!gate.accept(status(7, 99, 1), 4'000'000));
        CHECK(!gate.accept(status(8, 100, 1), 4'000'000));
        CHECK(!gate.fresh(52'000'000));
        gate.issue(101, 53'000'000);
        CHECK(gate.accept(status(7, 101, 1), 54'000'000));
        CHECK(!gate.accept(status(7, 100, 2), 54'000'000));
        CHECK(!gate.accept(status(7, 101, 2, 10), 55'000'000));
        gate.issue(102, 56'000'000);
        CHECK(gate.accept(status(7, 102, 0, 10), 57'000'000));
        CHECK(gate.accept(status(7, 102, 1, 10, false), 58'000'000));
        CHECK(!gate.fresh(58'000'000));
        Settings s;
        auto box = letterbox(320, 160, 320);
        CHECK(box.left == 0 && box.top == 80 && box.scale == 1);
        box = letterbox(160, 320, 320);
        CHECK(box.left == 80 && box.top == 0);
        // Three candidates: two overlapping same-class boxes, one distinct class.
        std::vector<float> output = {160, 160, 160, 160, 160, 160, 40,  40,  40,
                                     40,  40,  40,  .9f, .8f, .1f, .1f, .2f, .85f};
        auto ds = decode_yolo(output, 3, 2, 320, 160, 320, s);
        CHECK(ds.size() == 2);
        CHECK(ds[0].x == 140 && ds[0].y == 60 && ds[0].cls == 0);
        s.classes = {1};
        ds = decode_yolo(output, 3, 2, 320, 160, 320, s);
        CHECK(ds.size() == 1 && ds[0].cls == 1);
        s.classes.clear();
        Telemetry limits;
        limits.xmin = limits.ymin = -127;
        limits.xmax = limits.ymax = 127;
        Controller control;
        std::vector<Detection> targets = {{180, 150, 20, 20, .9f, 0}, {210, 150, 20, 20, .99f, 1}};
        auto correction = control.update(targets, 320, 320, s, limits, 1'000'000);
        CHECK(correction.target && correction.target->cls == 0 && correction.dx == 6 && correction.dy == 0);
        s.highest_confidence = true;
        correction = control.update(targets, 320, 320, s, limits, 2'000'000);
        CHECK(correction.target->cls == 1 && correction.dx == 12);
        s.persistence = true;
        targets[0].score = 1;
        correction = control.update(targets, 320, 320, s, limits, 3'000'000);
        CHECK(correction.target->cls == 1);
        CHECK(!control.update({}, 320, 320, s, limits, 4'000'000).target);
        s = {};
        s.gain_x = .1f;
        s.deadzone = 0;
        targets = {{155, 150, 20, 20, .9f, 0}};
        control.reset();
        auto a = control.update(targets, 320, 320, s, limits, 1'000'000),
             b = control.update(targets, 320, 320, s, limits, 2'000'000);
        CHECK(a.dx == 0 && b.dx == 1);
        control.reset();
        CHECK(control.update(targets, 320, 320, s, limits, 3'000'000).dx == 0);
        targets = {{150, 150, 20, 20, .9f, 0}};
        CHECK(control.update(targets, 320, 320, s, limits, 4'000'000).dx == 0);
        s.gain_x = 10;
        s.max_step = 32;
        targets = {{180, 150, 20, 20, .9f, 0}};
        limits.xmax = 10;
        CHECK(control.update(targets, 320, 320, s, limits, 5'000'000).dx == 10);
        s.smoothing_ms = 20;
        control.reset();
        auto smooth = control.update(targets, 320, 320, s, limits, 6'000'000);
        CHECK(smooth.dx >= 0 && smooth.dx <= 10);
        bool rejected = false;
        s.confidence = std::nanf("");
        try {
            validate(s);
        } catch (...) {
            rejected = true;
        }
        CHECK(rejected);
        auto file = std::filesystem::temp_directory_path() / "receiver-profile-roundtrip.json";
        s = {};
        s.gain_x = .3f;
        s.classes = {1, 2};
        save_settings(s, file.string());
        auto loaded = load_settings(file.string());
        CHECK(loaded.classes == s.classes && loaded.gain_x == s.gain_x);
        std::filesystem::remove(file);
        Samples samples;
        samples.add(1);
        samples.add(2);
        samples.add(3);
        CHECK(samples.percentile(.5) == 2 && samples.percentile(.99) == 3);
        std::cout << checks << " core checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
