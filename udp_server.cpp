#include "udp_server.h"
#include "host-raw-gadget.h"
#include "proxy.h"
#include "misc.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <iomanip>

UdpServer::UdpServer(int port) : port(port), sockfd(-1), running(false) {}

UdpServer::~UdpServer() {
    stop();
}

void UdpServer::start() {
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        close(sockfd);
        sockfd = -1;
        return;
    }

    running = true;
    server_thread = std::thread(&UdpServer::server_loop, this);
    printf("UDP Server started on port %d\n", port);
}

void UdpServer::stop() {
    running = false;
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
}

void UdpServer::join() {
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

void UdpServer::server_loop() {
    char buffer[1024];
    struct sockaddr_in cliaddr;
    socklen_t len;

    while (running) {
        len = sizeof(cliaddr);
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&cliaddr, &len);
        if (n > 0) {
            buffer[n] = '\0';
            std::string packet(buffer);
            // Remove newline if present
            packet.erase(std::remove(packet.begin(), packet.end(), '\n'), packet.end());
            packet.erase(std::remove(packet.begin(), packet.end(), '\r'), packet.end());
            
            process_packet(packet);
        }
    }
}

void UdpServer::process_packet(const std::string& packet) {
    if (packet.empty()) return;

    if (packet[0] == '+') {
        handle_command(packet);
    } else {
        handle_raw_injection(packet);
    }
}

void UdpServer::handle_command(const std::string& command) {
    std::stringstream ss(command);
    std::string cmd;
    ss >> cmd;

    int mouse_ep = find_mouse_endpoint();
    if (mouse_ep == -1) {
        printf("Error: Could not find mouse endpoint for injection\n");
        return;
    }

    if (cmd == "+move") {
        int x, y;
        if (ss >> x >> y) {
            // Construct standard mouse report: Buttons, X, Y, Wheel
            // Assuming 4 byte report. X and Y are signed 8-bit usually.
            std::vector<uint8_t> data(4, 0);
            data[1] = (uint8_t)x;
            data[2] = (uint8_t)y;
            inject_packet(mouse_ep, data);
        }
    } else if (cmd == "+click") {
        // Click: Button 1 Down, then Up
        std::vector<uint8_t> down(4, 0);
        down[0] = 1; // Button 1
        inject_packet(mouse_ep, down);

        // Small delay? Or just send immediately?
        // In a real scenario, we might need a delay, but here we just inject.
        // Let's inject UP immediately.
        std::vector<uint8_t> up(4, 0);
        up[0] = 0; // All buttons up
        inject_packet(mouse_ep, up);
    }
}

void UdpServer::handle_raw_injection(const std::string& data_str) {
    std::stringstream ss(data_str);
    std::string ep_str, payload_str;
    ss >> ep_str >> payload_str;

    if (ep_str.empty()) return;

    int ep_addr = 0;
    try {
        ep_addr = std::stoi(ep_str, nullptr, 16);
    } catch (...) {
        printf("Invalid endpoint address: %s\n", ep_str.c_str());
        return;
    }

    // If payload is separated by space, or just one long hex string?
    // The example was "81 00010203" -> EP 81, Data 00010203
    
    // If payload_str is empty, maybe the rest of the string is the payload?
    // But ss >> payload_str only takes one word.
    // Let's assume the format is "EP HEXDATA"
    
    if (payload_str.empty()) {
         // Maybe the user sent "81 00 01 02 03"?
         // Let's try to parse the rest of the stream
         while (ss >> payload_str) {
             // Append? No, let's assume standard format "EP DATA"
             // If data is space separated, we need to handle it.
             // But hexToAscii expects a single string.
             // Let's concatenate if there are multiple parts?
             // Or just assume "EP DATA_NO_SPACES"
         }
    }

    // Re-read payload from the original string to be safe?
    // Let's just use the second word as the payload for now, assuming "EP DATA"
    // If the user provided "81 00010203", payload_str is "00010203".
    
    std::string payload_raw = hexToAscii(payload_str);
    std::vector<uint8_t> data(payload_raw.begin(), payload_raw.end());
    inject_packet(ep_addr, data);
}

void UdpServer::inject_packet(int ep_addr, const std::vector<uint8_t>& data) {
    // Find the endpoint queue
    struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
    
    for (int i = 0; i < config->config.bNumInterfaces; i++) {
        struct raw_gadget_interface *iface = &config->interfaces[i];
        struct raw_gadget_altsetting *alt = &iface->altsettings[iface->current_altsetting];
        
        for (int j = 0; j < alt->interface.bNumEndpoints; j++) {
            struct raw_gadget_endpoint *ep = &alt->endpoints[j];
            if (ep->endpoint.bEndpointAddress == ep_addr) {
                // Found it
                struct usb_raw_transfer_io io;
                io.inner.ep = ep->thread_info.ep_num;
                io.inner.flags = 0;
                io.inner.length = data.size();
                if (data.size() > sizeof(io.data)) {
                    printf("Packet too large for injection: %lu\n", data.size());
                    return;
                }
                memcpy(io.data, data.data(), data.size());

                ep->thread_info.data_mutex->lock();
                ep->thread_info.data_queue->push_back(io);
                ep->thread_info.data_mutex->unlock();
                
                if (debug_udp) {
                    printf("EP%02x: Injected %lu bytes\n", ep_addr, data.size());
                }
                return;
            }
        }
    }
    printf("Endpoint %02x not found for injection\n", ep_addr);
}

int UdpServer::find_mouse_endpoint() {
    // Heuristic: Find first Interrupt IN endpoint.
    // Mouse is usually HID class (Interface Class 3), but here we just look for endpoints.
    
    struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
    
    for (int i = 0; i < config->config.bNumInterfaces; i++) {
        struct raw_gadget_interface *iface = &config->interfaces[i];
        struct raw_gadget_altsetting *alt = &iface->altsettings[iface->current_altsetting];
        
        // Check interface class?
        // if (alt->interface.bInterfaceClass == 3) { ... } 
        
        for (int j = 0; j < alt->interface.bNumEndpoints; j++) {
            struct raw_gadget_endpoint *ep = &alt->endpoints[j];
            if ((ep->endpoint.bmAttributes & USB_TRANSFER_TYPE_MASK) == USB_ENDPOINT_XFER_INT &&
                (ep->endpoint.bEndpointAddress & USB_DIR_IN)) {
                return ep->endpoint.bEndpointAddress;
            }
        }
    }
    return -1;
}
