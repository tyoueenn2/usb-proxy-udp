#pragma once
#include "mouse_mixer.h"
#include <atomic>
#include <thread>
#include <cstdint>
class SchedulerEvent;
class SchedulerTimer;
class UdpServer {
public:
    explicit UdpServer(int port) : port(port) {}
    ~UdpServer() { stop(); }
    bool start();
    void stop();
    void join();
private:
    int port, sockfd = -1;
    std::atomic<bool> running{false};
    std::thread server_thread;
    std::shared_ptr<SchedulerEvent> wake_;
    void server_loop(SchedulerTimer& timer);
};
