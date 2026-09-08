#pragma once
#include "backend.hpp"
#include "network.hpp"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
namespace receiver {
struct Samples {
    std::array<double, 2048> values{};
    uint64_t count = 0;
    void add(double v) {
        values[count++ % values.size()] = v;
    }
    double percentile(double p) const;
};
struct Stats {
    ReassemblyStats network;
    uint64_t inferred = 0, sent = 0, replaced = 0, stale = 0, telemetry_rejected = 0;
    bool running = false, armed = false, active = false, synchronized = false, pi_ready = false;
    int width = 0, height = 0;
    uint8_t physical = 0;
    double clock_uncertainty_ms = 0, frame_age_ms = 0;
    double gpu_free_mib = 0, gpu_total_mib = 0;
    std::string status = "Stopped", backend = "Not loaded", error;
    Samples reassembly, upload, inference, postprocess, submit, receiver_total, capture_age;
};
struct Preview {
    std::shared_ptr<const Frame> frame;
    std::vector<Detection> detections;
    Correction correction;
};
class App {
    struct Result {
        std::shared_ptr<const Frame> frame;
        std::vector<Detection> detections;
        uint64_t epoch = 0, id = 0;
        int64_t deadline = 0;
    };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Settings settings_;
    Stats stats_;
    std::atomic<bool> stop_{true};
    std::thread receive_thread_, inference_thread_, pi_thread_;
    std::shared_ptr<const Frame> latest_;
    std::optional<Result> result_;
    Preview preview_;
    ClockSync clock_;
    uint64_t epoch_ = 0, result_id_ = 0;
    bool simulated_ = false;
    void receive_loop();
    void inference_loop();
    void pi_loop();
    void fail(const std::string& error);

  public:
    ~App() {
        stop();
    }
    void start(const Settings& settings, bool simulated = false);
    void stop();
    void arm(bool enabled);
    void configure(const Settings& settings);
    Stats stats() const;
    Preview preview() const;
    void export_metrics(const std::string& path) const;
};
} // namespace receiver
