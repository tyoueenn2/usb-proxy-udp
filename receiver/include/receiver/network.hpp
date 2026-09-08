#pragma once
#include "protocol.hpp"
#include <string>
namespace receiver {
struct Address {
    uint32_t ip = 0;
    uint16_t port = 0;
    bool operator==(const Address&) const = default;
};
Address address(const std::string& ip, int port);
class UdpSocket {
    uintptr_t socket_ = ~uintptr_t(0);

  public:
    UdpSocket(const std::string& ip, int port);
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    int receive(std::span<uint8_t> buffer, Address& from, int timeout_ms);
    bool send(Bytes data, const Address& to);
};
uint64_t random_id();
} // namespace receiver
