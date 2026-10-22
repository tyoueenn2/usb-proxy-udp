#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// Optional diagnostic sink. No file IO on the control/writer thread. Bounded
// queue: loss is explicit; timing runs with any loss must be discarded.
namespace measurement {
inline long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
class Log {
    std::ofstream file_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    std::thread worker_;
    bool stop_ = false;
    unsigned long long dropped_ = 0;
    std::string component_, run_;
    void write(const std::string& body) {
        file_ << "{\"schema\":1,\"component\":\"" << component_
              << "\",\"run\":\"" << run_ << "\"," << body << "}\n";
    }
    void drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            std::deque<std::string> batch;
            batch.swap(queue_);
            bool done = stop_;
            lock.unlock();
            for (const auto& s : batch) write(s);
            file_.flush();
            if (!file_) std::cerr << "Measurement log write failed; discard this run\n";
            lock.lock();
            if (done) break;
        }
        write("\"event\":\"log_end\",\"dropped\":" + std::to_string(dropped_));
        file_.flush();
    }
  public:
    Log(const char* environment, const char* component) : component_(component) {
        const char* path = std::getenv(environment);
        if (!path || !*path) return;
        file_.open(path, std::ios::app);
        if (!file_) {
            std::cerr << "Cannot open measurement log: " << path << '\n';
            return;
        }
        run_ = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
               "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        write("\"event\":\"log_start\"");
        worker_ = std::thread([this] { drain(); });
    }
    ~Log() {
        if (!worker_.joinable()) return;
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        wake_.notify_one();
        worker_.join();
    }
    bool enabled() const { return worker_.joinable(); }
    void emit(std::string body) {
        if (!enabled()) return;
        { std::lock_guard<std::mutex> lock(mutex_);
          if (queue_.size() >= 8192) { ++dropped_; return; }
          queue_.push_back(std::move(body)); }
        wake_.notify_one();
    }
};
} // namespace measurement
