#include "receiver/app.hpp"
#include <atomic>
#include <csignal>
#include <iostream>
static volatile std::sig_atomic_t interrupted = 0;
static void interrupt(int) {
    interrupted = 1;
}
int main(int argc, char** argv) {
    try {
        receiver::Settings cfg;
        bool simulate = false, arm = false;
        int seconds = 10;
        std::string metrics = "metrics.csv";
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw std::runtime_error("Missing value for " + a);
                return std::string(argv[i]);
            };
            if (a == "--profile")
                cfg = receiver::load_settings(value());
            else if (a == "--simulate")
                simulate = true;
            else if (a == "--arm")
                arm = true;
            else if (a == "--seconds")
                seconds = std::stoi(value());
            else if (a == "--metrics")
                metrics = value();
            else if (a == "--help") {
                std::cout << "receiver_headless [--profile FILE] [--simulate] [--arm] [--seconds N] "
                             "[--metrics FILE]\nSimulation is loopback-only. --arm explicitly enables "
                             "hold-to-activate output.\n";
                return 0;
            } else
                throw std::runtime_error("Unknown option: " + a);
        }
        if (seconds < 1 || seconds > 86400)
            throw std::runtime_error("Seconds must be 1..86400");
        std::signal(SIGINT, interrupt);
        receiver::App app;
        app.start(cfg, simulate);
        if (arm)
            app.arm(true);
        auto start = receiver::now_ns();
        int64_t reported = 0;
        while (!interrupted && receiver::now_ns() - start < int64_t(seconds) * 1'000'000'000) {
            auto s = app.stats();
            if (!s.running || !s.error.empty())
                break;
            if (receiver::now_ns() - reported >= 1'000'000'000) {
                std::cout << s.status << " | frames=" << s.inferred << " sent=" << s.sent
                          << " sync=" << s.synchronized << " age_ms=" << s.frame_age_ms << std::endl;
                reported = receiver::now_ns();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        app.stop();
        app.export_metrics(metrics);
        auto s = app.stats();
        if (!s.error.empty()) {
            std::cerr << s.error << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
