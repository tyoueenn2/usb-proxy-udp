#ifndef UDP_SERVER_H
#define UDP_SERVER_H

#include <thread>
#include <atomic>
#include <string>
#include <vector>

class UdpServer {
public:
    UdpServer(int port);
    ~UdpServer();

    void start();
    void stop();
    void join();

private:
    int port;
    int sockfd;
    std::thread server_thread;
    std::atomic<bool> running;
    
    // Track current mouse button state
    uint8_t current_button_state;

    void server_loop();
    void process_packet(const std::string& packet);
    void handle_command(const std::string& command);
    void handle_raw_injection(const std::string& data);
    void inject_packet(int ep_addr, const std::vector<uint8_t>& data);
    
    // Helper to find mouse endpoint
    int find_mouse_endpoint();
};

#endif // UDP_SERVER_H
