#pragma once
#include <atomic>
#include <thread>
#include <cstdint>
struct thread_info;
struct usb_raw_transfer_io;
void register_mouse_endpoint(thread_info* info, int interface_number);
void learn_mouse_descriptor(int interface_number, const uint8_t* data, unsigned length);
void set_mouse_protocol(int interface_number, bool boot);
void unregister_mouse_endpoint(thread_info* info);
bool merge_mouse_report(uint8_t endpoint, usb_raw_transfer_io& io);
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
    void server_loop();
};
