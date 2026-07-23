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
#include <mutex>

static std::vector<HidMouseReport> g_mouse_reports;
static std::mutex g_mouse_reports_mutex;
static std::atomic<uint32_t> g_real_mouse_buttons(0);

// Global variable to track real mouse button state from physical mouse
std::atomic<uint8_t> g_real_mouse_button_state(0x00);

// Function to update real mouse state (called from proxy.cpp)
void update_real_mouse_state(uint8_t button_state) {
    g_real_mouse_button_state.store(button_state);
}

void configure_hid_mouse_reports(const std::vector<std::pair<int, std::vector<uint8_t> > >& descriptors) {
    std::vector<HidMouseReport> parsed;
    for (size_t i = 0; i < descriptors.size(); ++i) {
        std::vector<HidMouseReport> reports = parse_hid_mouse_reports(descriptors[i].first, descriptors[i].second);
        parsed.insert(parsed.end(), reports.begin(), reports.end());
    }
    std::lock_guard<std::mutex> lock(g_mouse_reports_mutex);
    g_mouse_reports.swap(parsed);
    printf("[HID] Parsed %lu relative mouse report format(s) from device descriptors\n", g_mouse_reports.size());
}

void update_real_mouse_report(uint8_t endpoint, const uint8_t *data, size_t length) {
	int interface_number = -1;
	struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
	for (int i = 0; i < config->config.bNumInterfaces; ++i) {
		struct raw_gadget_altsetting *alt = &config->interfaces[i].altsettings[config->interfaces[i].current_altsetting];
		for (int j = 0; j < alt->interface.bNumEndpoints; ++j)
			if (alt->endpoints[j].endpoint.bEndpointAddress == endpoint) interface_number = alt->interface.bInterfaceNumber;
	}
    std::lock_guard<std::mutex> lock(g_mouse_reports_mutex);
    for (size_t i = 0; i < g_mouse_reports.size(); ++i) {
        const HidMouseReport& r = g_mouse_reports[i];
		if (r.interface_number != interface_number) continue;
        size_t prefix = r.has_report_id ? 1 : 0;
        if (length < prefix + (r.report_bits + 7) / 8 || (r.has_report_id && data[0] != r.report_id)) continue;
        uint32_t buttons = 0;
        for (size_t b = 0; b < r.buttons.size() && b < 32; ++b)
            if (hid_get_bits(data + prefix, r.buttons[b].bit_offset, r.buttons[b].bit_size)) buttons |= 1u << b;
        g_real_mouse_buttons.store(buttons);
        g_real_mouse_button_state.store((uint8_t)buttons);
        return;
    }
}

UdpServer::UdpServer(int port) : port(port), sockfd(-1), running(false), current_button_state(0x00) {}

UdpServer::~UdpServer() {
    stop();
}

void UdpServer::start() {
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return;
    }

    // Configure socket for low latency
    // Minimize receive buffer to reduce buffering delay
    int rcvbuf = 4096;  // Small buffer for low latency
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("setsockopt SO_RCVBUF failed (non-fatal)");
    }
    
    // Set socket priority for faster processing
    int priority = 6;  // High priority
    if (setsockopt(sockfd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) < 0) {
        perror("setsockopt SO_PRIORITY failed (non-fatal)");
    }
    
    // Set receive timeout to prevent indefinite blocking
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO failed (non-fatal)");
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
            
            if (debug_level >= 1) {
                printf("[UDP] Received: %s\n", packet.c_str());
            }
            
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

    HidMouseReport report;
    if (!find_mouse_report(mouse_ep, report)) {
        printf("Error: HID mouse report descriptor has no relative X/Y report for endpoint 0x%02x\n", mouse_ep);
        return;
    }

    if (debug_level >= 1) {
        printf("[CMD] Processing command: %s (using EP 0x%02x)\n", cmd.c_str(), mouse_ep);
    }

    if (cmd == "+move") {
        int x, y;
        if (ss >> x >> y) {
            uint32_t real_button_state = g_real_mouse_buttons.load();
            std::vector<uint8_t> data = build_mouse_report(report, x, y, real_button_state);
            
            if (debug_level >= 2) {
                printf("[CMD] Mouse move: X=%d, Y=%d (real button state: 0x%02x)\n", x, y, real_button_state);
            }
            
            inject_packet(mouse_ep, data);
        } else {
            printf("Error: +move requires X and Y coordinates\n");
        }
    } else if (cmd == "+click") {
        // Click: Left button down then up
        std::vector<uint8_t> down = build_mouse_report(report, 0, 0, 1);
        
        if (debug_level >= 2) {
            printf("[CMD] Mouse left click\n");
        }
        
        inject_packet(mouse_ep, down);

        // Small delay between down and up
        usleep(10000); // 10ms
        
        // Release: All buttons up
        std::vector<uint8_t> up = build_mouse_report(report, 0, 0, 0);
        inject_packet(mouse_ep, up);
    } else if (cmd == "+mousedown") {
        // Press and hold mouse button
        int button = 1; // Default to left button
        ss >> button; // Optional: read button number
        
        if (button < 1 || button > 32) { printf("Error: button must be 1-32\n"); return; }
        current_button_state |= (1u << (button - 1)); // Set button bit
        
        std::vector<uint8_t> data = build_mouse_report(report, 0, 0, current_button_state);
        
        if (debug_level >= 2) {
            printf("[CMD] Mouse button %d down (state: 0x%02x)\n", button, current_button_state);
        }
        
        inject_packet(mouse_ep, data);
    } else if (cmd == "+mouseup") {
        // Release mouse button
        int button = 1; // Default to left button
        ss >> button; // Optional: read button number
        
        if (button < 1 || button > 32) { printf("Error: button must be 1-32\n"); return; }
        current_button_state &= ~(1u << (button - 1)); // Clear button bit
        
        std::vector<uint8_t> data = build_mouse_report(report, 0, 0, current_button_state);
        
        if (debug_level >= 2) {
            printf("[CMD] Mouse button %d up (state: 0x%02x)\n", button, current_button_state);
        }
        
        inject_packet(mouse_ep, data);
    } else {
        printf("Error: Unknown command: %s\n", cmd.c_str());
    }
}

void UdpServer::handle_raw_injection(const std::string& data_str) {
    std::stringstream ss(data_str);
    std::string ep_str, payload_str;
    
    // Get the first word (endpoint)
    if (!(ss >> ep_str)) {
        printf("Error: No endpoint specified\n");
        return;
    }

    int ep_addr = 0;
    try {
        ep_addr = std::stoi(ep_str, nullptr, 16);
    } catch (...) {
        printf("Invalid endpoint address: %s\n", ep_str.c_str());
        return;
    }

    // Get the remaining part as payload (can be space-separated hex)
    std::string remaining;
    std::getline(ss, remaining);
    
    // Remove leading whitespace
    size_t start = remaining.find_first_not_of(" \t");
    if (start != std::string::npos) {
        remaining = remaining.substr(start);
    }
    
    if (remaining.empty()) {
        printf("Error: No payload specified\n");
        return;
    }
    
    if (debug_level >= 2) {
        printf("[RAW] EP: 0x%02x, Payload: %s\n", ep_addr, remaining.c_str());
    }
    
    // Parse hex string (handles "010203" or "01 02 03" formats)
    std::vector<uint8_t> data = parseHexString(remaining);
    
    if (data.empty()) {
        printf("Error: Could not parse payload\n");
        return;
    }
    
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
                
                // Wake the endpoint thread immediately for low latency
                ep->thread_info.data_cond->notify_one();
                
                if (debug_level >= 1) {
                    printf("[INJ] EP 0x%02x: Injected %lu bytes\n", ep_addr, data.size());
                }
                
                if (debug_level >= 3) {
                    printHexDump("[INJ] Data: ", data.data(), data.size());
                }
                
                return;
            }
        }
    }
    printf("Endpoint 0x%02x not found for injection\n", ep_addr);
}

int UdpServer::find_mouse_endpoint() {
    struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];

    // Prefer an interface whose descriptor actually contains a relative X/Y
    // report. Many composite mice use protocol 0 instead of boot protocol 2.
    std::lock_guard<std::mutex> lock(g_mouse_reports_mutex);
    for (size_t r = 0; r < g_mouse_reports.size(); ++r) {
        for (int i = 0; i < config->config.bNumInterfaces; ++i) {
            struct raw_gadget_interface *iface = &config->interfaces[i];
            struct raw_gadget_altsetting *alt = &iface->altsettings[iface->current_altsetting];
            if (alt->interface.bInterfaceNumber != g_mouse_reports[r].interface_number) continue;
            for (int j = 0; j < alt->interface.bNumEndpoints; ++j) {
                struct raw_gadget_endpoint *ep = &alt->endpoints[j];
                if ((ep->endpoint.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT &&
                    (ep->endpoint.bEndpointAddress & USB_DIR_IN)) return ep->endpoint.bEndpointAddress;
            }
        }
    }

    // Compatibility fallback for devices that do not expose a usable descriptor.
    
    for (int i = 0; i < config->config.bNumInterfaces; i++) {
        struct raw_gadget_interface *iface = &config->interfaces[i];
        struct raw_gadget_altsetting *alt = &iface->altsettings[iface->current_altsetting];
        
        // Check if this is a boot-protocol HID Mouse interface.
        if (alt->interface.bInterfaceClass == 3 && alt->interface.bInterfaceProtocol == 2) {
            // Found HID Mouse interface, return its interrupt IN endpoint
            for (int j = 0; j < alt->interface.bNumEndpoints; j++) {
                struct raw_gadget_endpoint *ep = &alt->endpoints[j];
                if ((ep->endpoint.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT &&
                    (ep->endpoint.bEndpointAddress & USB_DIR_IN)) {
                    if (debug_level >= 2) {
                        printf("[INIT] Found mouse endpoint: 0x%02x (max packet: %d bytes)\n", 
                               ep->endpoint.bEndpointAddress, ep->endpoint.wMaxPacketSize);
                    }
                    return ep->endpoint.bEndpointAddress;
                }
            }
        }
    }
    
    if (debug_level >= 1) {
        printf("[WARN] No HID Mouse interface found, falling back to first Interrupt IN\n");
    }
    
    // Fallback: Find first Interrupt IN endpoint
    for (int i = 0; i < config->config.bNumInterfaces; i++) {
        struct raw_gadget_interface *iface = &config->interfaces[i];
        struct raw_gadget_altsetting *alt = &iface->altsettings[iface->current_altsetting];
        
        for (int j = 0; j < alt->interface.bNumEndpoints; j++) {
            struct raw_gadget_endpoint *ep = &alt->endpoints[j];
            if ((ep->endpoint.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT &&
                (ep->endpoint.bEndpointAddress & USB_DIR_IN)) {
                return ep->endpoint.bEndpointAddress;
            }
        }
    }
    return -1;
}

bool UdpServer::find_mouse_report(int endpoint, HidMouseReport& report) {
    int interface_number = -1;
    struct raw_gadget_config *config = &host_device_desc.configs[host_device_desc.current_config];
    for (int i = 0; i < config->config.bNumInterfaces; ++i) {
        struct raw_gadget_altsetting *alt = &config->interfaces[i].altsettings[config->interfaces[i].current_altsetting];
        for (int j = 0; j < alt->interface.bNumEndpoints; ++j)
            if (alt->endpoints[j].endpoint.bEndpointAddress == endpoint) interface_number = alt->interface.bInterfaceNumber;
    }
    std::lock_guard<std::mutex> lock(g_mouse_reports_mutex);
    for (size_t i = 0; i < g_mouse_reports.size(); ++i)
        if (g_mouse_reports[i].interface_number == interface_number) { report = g_mouse_reports[i]; return true; }
    return false;
}

std::vector<uint8_t> UdpServer::build_mouse_report(const HidMouseReport& report, int x, int y, uint32_t buttons) {
    size_t prefix = report.has_report_id ? 1 : 0;
    std::vector<uint8_t> data(prefix + (report.report_bits + 7) / 8, 0);
    if (report.has_report_id) data[0] = report.report_id;
    x = std::max(report.x.logical_min, std::min(report.x.logical_max, x));
    y = std::max(report.y.logical_min, std::min(report.y.logical_max, y));
    hid_set_bits(data, prefix * 8 + report.x.bit_offset, report.x.bit_size, x);
    hid_set_bits(data, prefix * 8 + report.y.bit_offset, report.y.bit_size, y);
    for (size_t i = 0; i < report.buttons.size() && i < 32; ++i)
        hid_set_bits(data, prefix * 8 + report.buttons[i].bit_offset, report.buttons[i].bit_size, (buttons >> i) & 1);
    return data;
}
