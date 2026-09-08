#include "receiver/app.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
namespace receiver {
double Samples::percentile(double p) const {
    auto n = size_t(std::min<uint64_t>(count, values.size()));
    if (!n)
        return 0;
    auto sorted = values;
    std::sort(sorted.begin(), sorted.begin() + n);
    return sorted[size_t(std::ceil(p * (n - 1)))];
}
void App::fail(const std::string& error) {
    std::lock_guard lock(mutex_);
    stats_.error = error;
    stats_.status = "Failed";
    stats_.armed = stats_.active = false;
    ++epoch_;
    result_.reset();
    stop_ = true;
    cv_.notify_all();
}
void App::start(const Settings& settings, bool simulated) {
    stop();
    validate(settings);
    address(settings.bind_ip, settings.frame_port);
    auto sender = address(settings.sender_ip, 0);
    auto pi = address(settings.pi_ip, settings.pi_port);
    auto local = address("127.0.0.1", 0);
    if (simulated && (sender.ip != local.ip || pi.ip != local.ip))
        throw std::runtime_error("Simulation requires both sender and Pi to be 127.0.0.1");
    {
        std::lock_guard lock(mutex_);
        settings_ = settings;
        stats_ = {};
        stats_.running = true;
        stats_.status = "Starting";
        latest_.reset();
        result_.reset();
        preview_ = {};
        clock_.reset();
        ++epoch_;
        simulated_ = simulated;
        stop_ = false;
    }
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    receive_thread_ = std::thread([this] {
        try {
            receive_loop();
        } catch (const std::exception& e) {
            fail(e.what());
        }
    });
    inference_thread_ = std::thread([this] {
        try {
            inference_loop();
        } catch (const std::exception& e) {
            fail(e.what());
        }
    });
    pi_thread_ = std::thread([this] {
        try {
            pi_loop();
        } catch (const std::exception& e) {
            fail(e.what());
        }
    });
}
void App::stop() {
    bool had_threads = receive_thread_.joinable();
    stop_ = true;
    {
        std::lock_guard lock(mutex_);
        stats_.armed = stats_.active = false;
        ++epoch_;
        result_.reset();
    }
    cv_.notify_all();
    for (auto* t : {&receive_thread_, &inference_thread_, &pi_thread_})
        if (t->joinable())
            t->join();
    {
        std::lock_guard lock(mutex_);
        stats_.running = false;
        latest_.reset();
        preview_ = {};
        if (stats_.error.empty())
            stats_.status = "Stopped";
    }
#ifdef _WIN32
    if (had_threads)
        timeEndPeriod(1);
#else
    (void)had_threads;
#endif
}
void App::arm(bool enabled) {
    std::lock_guard lock(mutex_);
    stats_.armed = enabled && !stop_ && stats_.error.empty();
    ++epoch_;
    result_.reset();
    stats_.active = false;
}
void App::configure(const Settings& s) {
    validate(s);
    std::lock_guard lock(mutex_);
    if (s.bind_ip != settings_.bind_ip || s.sender_ip != settings_.sender_ip || s.pi_ip != settings_.pi_ip ||
        s.frame_port != settings_.frame_port || s.pi_port != settings_.pi_port ||
        s.model != settings_.model || s.metadata != settings_.metadata ||
        s.input_size != settings_.input_size)
        throw std::runtime_error("Stop and restart to change networking or model");
    settings_ = s;
    ++epoch_;
    result_.reset();
    if (!s.preview)
        preview_ = {};
}
Stats App::stats() const {
    std::lock_guard lock(mutex_);
    auto s = stats_;
    s.running = !stop_;
    return s;
}
Preview App::preview() const {
    std::lock_guard lock(mutex_);
    return preview_;
}
void App::receive_loop() {
    Settings cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = settings_;
    }
    UdpSocket socket(cfg.bind_ip, cfg.frame_port);
    auto allowed = address(cfg.sender_ip, 0);
    Address peer{};
    uint64_t session = 0;
    int64_t last_hello = 0, last_ping = 0, pending_ping = 0;
    std::array<uint64_t, 16> retired{};
    size_t retire_at = 0;
    Reassembler reassembly;
    std::array<uint8_t, 2048> data{};
    while (!stop_) {
        Address from;
        int n = socket.receive(data, from, 2);
        auto now = now_ns();
        if (n > 0 && from.ip == allowed.ip) {
            Bytes p(data.data(), size_t(n));
            if (n == 16 && !std::memcmp(data.data(), "UVH1", 4) && be32(data.data() + 4) == 0) {
                uint64_t next = be64(data.data() + 8);
                if (next && std::find(retired.begin(), retired.end(), next) == retired.end() &&
                    (!session || from == peer || now - last_hello > 100'000'000)) {
                    if (next != session || from != peer) {
                        if (session)
                            retired[retire_at++ % retired.size()] = session;
                        session = next;
                        peer = from;
                        reassembly.reset(session);
                        pending_ping = last_ping = 0;
                        std::lock_guard lock(mutex_);
                        clock_.reset();
                        latest_.reset();
                        result_.reset();
                        preview_ = {};
                        ++epoch_;
                    }
                    last_hello = now;
                }
            } else if (session && from == peer) {
                if (n == 40 && !std::memcmp(data.data(), "UVS1", 4) && be32(data.data() + 4) == 0 &&
                    be64(data.data() + 8) == session && be64(data.data() + 16) == uint64_t(pending_ping) &&
                    pending_ping) {
                    auto t1 = be64(data.data() + 24), t2 = be64(data.data() + 32);
                    if (t1 <= INT64_MAX && t2 <= INT64_MAX) {
                        std::lock_guard lock(mutex_);
                        clock_.observe(pending_ping, int64_t(t1), int64_t(t2), now);
                    }
                    pending_ping = 0;
                } else if (n >= 48 && !std::memcmp(data.data(), "UVF1", 4)) {
                    auto frame = reassembly.accept(p, now);
                    if (frame) {
                        std::lock_guard lock(mutex_);
                        if (latest_)
                            ++stats_.replaced;
                        latest_ = std::move(frame);
                        stats_.width = latest_->header.width;
                        stats_.height = latest_->header.height;
                        stats_.reassembly.add(double(latest_->complete_ns - latest_->first_ns) / 1e6);
                        cv_.notify_one();
                    }
                }
            }
        }
        reassembly.expire(now);
        if (session && now - last_ping >= 250'000'000) {
            std::array<uint8_t, 24> ping{};
            std::memcpy(ping.data(), "UVC1", 4);
            put64(ping.data() + 8, session);
            put64(ping.data() + 16, uint64_t(now));
            if (socket.send(ping, peer)) {
                pending_ping = last_ping = now;
            }
        }
        {
            std::lock_guard lock(mutex_);
            stats_.network = reassembly.stats;
            stats_.synchronized = clock_.synchronized(now);
            stats_.clock_uncertainty_ms = clock_.uncertainty_ms();
        }
    }
}
void App::inference_loop() {
    Settings initial;
    {
        std::lock_guard lock(mutex_);
        initial = settings_;
    }
    std::unique_ptr<Backend> backend;
    if (!simulated_)
        backend = make_backend(initial);
    {
        std::lock_guard lock(mutex_);
        stats_.backend =
            simulated_ ? "SIMULATION (fixed test detection, loopback only)" : backend->description();
        if (backend) {
            auto [free, total] = backend->device_memory();
            stats_.gpu_free_mib = double(free) / (1024 * 1024);
            stats_.gpu_total_mib = double(total) / (1024 * 1024);
        }
    }
    int64_t last_preview = 0;
    while (!stop_) {
        std::shared_ptr<const Frame> frame;
        Settings cfg;
        uint64_t epoch;
        int64_t deadline = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(10), [&] { return stop_ || bool(latest_); });
            if (stop_)
                break;
            if (!latest_)
                continue;
            frame = std::move(latest_);
            cfg = settings_;
            epoch = epoch_;
            auto at = now_ns();
            auto age = clock_.age_upper(frame->header.capture_ns, at);
            if (age) {
                stats_.frame_age_ms = double(*age) / 1e6;
                const int64_t horizon = int64_t(cfg.max_age_ms) * 1'000'000;
                deadline = at + horizon - *age - horizon / 5000;
            }
            if ((deadline && at > deadline) || at - frame->first_ns > int64_t(cfg.max_age_ms) * 1'000'000) {
                ++stats_.stale;
                continue;
            }
        }
        std::vector<Detection> detections;
        double upload = 0, infer = 0;
        auto post_start = now_ns();
        if (simulated_) {
            detections.push_back({frame->header.width * .60f, frame->header.height * .4f,
                                  frame->header.width * .2f, frame->header.height * .2f, .95f, 0});
        } else {
            auto result = backend->run(*frame);
            upload = result.upload_ms;
            infer = result.inference_ms;
            post_start = now_ns();
            detections = decode_yolo(result.output, result.candidates, result.classes, frame->header.width,
                                     frame->header.height, cfg.input_size, cfg);
        }
        auto done = now_ns();
        {
            std::lock_guard lock(mutex_);
            ++stats_.inferred;
            stats_.upload.add(upload);
            stats_.inference.add(infer);
            stats_.postprocess.add(double(done - post_start) / 1e6);
            if (epoch == epoch_ && !stop_) {
                result_ = Result{frame, std::move(detections), epoch, ++result_id_, deadline};
                if (cfg.preview && done - last_preview >= 33'333'333) {
                    preview_ = {frame, result_->detections, {}};
                    last_preview = done;
                }
            }
        }
    }
}
void App::pi_loop() {
    Settings initial;
    {
        std::lock_guard lock(mutex_);
        initial = settings_;
    }
    UdpSocket socket("0.0.0.0", 0);
    auto pi = address(initial.pi_ip, initial.pi_port);
    const uint64_t client = random_id();
    TelemetryGate gate;
    gate.reset(client);
    uint64_t token = random_id(), last_result = 0, last_epoch = 0;
    uint32_t sequence = uint32_t(random_id());
    int64_t renewal = 0;
    Controller controller;
    bool was_active = false;
    std::array<uint8_t, 2048> bytes{};
    while (!stop_) {
        auto now = now_ns();
#ifdef _WIN32
        if (GetAsyncKeyState(VK_DELETE) & 0x8000)
            arm(false);
#endif
        if (now - renewal >= 20'000'000) {
            auto p = subscribe(client, ++token);
            gate.issue(token, now);
            if (!socket.send(p, pi))
                throw std::runtime_error("Pi subscription send failed");
            renewal = now;
        }
        Address from;
        int n = socket.receive(bytes, from, 1);
        now = now_ns();
        bool proxy_error = false;
        if (n > 0 && from == pi) {
            if (n >= 4 && !std::memcmp(bytes.data(), "UPT1", 4)) {
                if (!gate.accept(Bytes(bytes.data(), size_t(n)), now)) {
                    std::lock_guard lock(mutex_);
                    ++stats_.telemetry_rejected;
                }
            } else {
                std::string reply(reinterpret_cast<char*>(bytes.data()), size_t(n));
                if (reply == "busy" || reply.rfind("error", 0) == 0) {
                    std::lock_guard lock(mutex_);
                    stats_.error = "Pi: " + reply;
                    stats_.armed = false;
                    ++epoch_;
                    result_.reset();
                    proxy_error = true;
                }
            }
        }
        std::lock_guard lock(mutex_);
        bool active = stats_.armed && !proxy_error && gate.fresh(now) &&
                      (gate.state.physical & (1u << (settings_.activation_button - 1)));
        stats_.pi_ready = gate.fresh(now);
        stats_.physical = gate.state.physical;
        if (active != was_active) {
            ++epoch_;
            result_.reset();
            controller.reset();
            was_active = active;
        }
        stats_.active = active;
        stats_.status = !stats_.error.empty() ? "Error - stop and restart"
                        : !gate.fresh(now)    ? "Waiting for fresh Pi telemetry (extension required)"
                        : !stats_.armed       ? "Disarmed"
                        : !active             ? "Armed - hold activation button"
                                              : "Active - waiting for fresh detection";
        if (!active || epoch_ != last_epoch) {
            controller.reset();
            last_epoch = epoch_;
        }
        if (active && result_ && (!result_->deadline || now > result_->deadline)) {
            controller.reset();
            stats_.status = "Active - waiting for fresh capture";
        }
        if (!active || !result_ || result_->id == last_result)
            continue;
        auto& r = *result_;
        last_result = r.id;
        if (r.epoch != epoch_ || !r.deadline || now > r.deadline ||
            now - r.frame->first_ns > int64_t(settings_.max_age_ms) * 1'000'000) {
            controller.reset();
            ++stats_.stale;
            continue;
        }
        auto correction = controller.update(r.detections, r.frame->header.width, r.frame->header.height,
                                            settings_, gate.state, now);
        if (preview_.frame && preview_.frame->header.sequence == r.frame->header.sequence)
            preview_.correction = correction;
        if (!correction.target) {
            stats_.status = "Active - no target";
            continue;
        }
        if (correction.dx || correction.dy) {
            // Serialize final gating and send against disarm/configuration changes.
            auto send_at = now_ns();
            if (send_at > r.deadline || !gate.fresh(send_at)) {
                controller.reset();
                ++stats_.stale;
                continue;
            }
            auto packet = movement(sequence++, correction.dx, correction.dy);
            if (!socket.send(packet, pi)) {
                stats_.armed = false;
                stats_.error = "Pi command send failed";
                ++epoch_;
                controller.reset();
                continue;
            }
            auto sent = now_ns();
            ++stats_.sent;
            stats_.submit.add(double(sent - send_at) / 1e6);
            stats_.receiver_total.add(double(sent - r.frame->first_ns) / 1e6);
            stats_.capture_age.add(double(int64_t(settings_.max_age_ms) * 1'000'000 - (r.deadline - sent)) /
                                   1e6);
            stats_.status = "Active - correction submitted";
        }
    }
    // V1 never injects button holds; no release command is needed or sent on shutdown.
}
void App::export_metrics(const std::string& path) const {
    auto s = stats();
    std::ofstream f(path);
    if (!f)
        throw std::runtime_error("Cannot write metrics");
    f << "stage,samples,p50_ms,p95_ms,p99_ms\n";
    for (auto pair : {std::pair{"reassembly", &s.reassembly},
                      {"copies_preprocess", &s.upload},
                      {"inference", &s.inference},
                      {"postprocess", &s.postprocess},
                      {"command_submission", &s.submit},
                      {"receiver_to_submission", &s.receiver_total},
                      {"capture_age_upper_at_submission", &s.capture_age}})
        f << pair.first << ',' << pair.second->count << ',' << pair.second->percentile(.5) << ','
          << pair.second->percentile(.95) << ',' << pair.second->percentile(.99) << '\n';
    f << "# backend," << s.backend << "\n# frames," << s.inferred << "\n# sent," << s.sent
      << "\n# superseded," << s.replaced << "\n# stale," << s.stale << "\n# incomplete_expired,"
      << s.network.expired << "\n# incomplete_evicted," << s.network.evicted << "\n# invalid_packets,"
      << s.network.invalid << "\n# duplicate_packets," << s.network.duplicates << "\n# pool_drops,"
      << s.network.pool_drops << "\n# host_frame_pool_mib,28\n# gpu_free_at_start_mib," << s.gpu_free_mib
      << "\n# gpu_total_mib," << s.gpu_total_mib << '\n';
}
} // namespace receiver
