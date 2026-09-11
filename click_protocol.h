#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace click_protocol {
constexpr uint8_t VERSION=1;
constexpr size_t REQUEST_SIZE=40;
constexpr size_t ACK_SIZE=48;
enum class Operation:uint8_t {Schedule=1,ReleaseAll=2};
enum class Status:uint8_t {
    Accepted=1,Duplicate=2,Completed=3,Busy=4,QueueFull=5,
    UnsupportedButton=6,Invalid=7,Cancelled=8,ButtonActive=9,
    NotReady=10,StaleCommand=11
};
inline uint16_t get16(const uint8_t* p){return uint16_t(p[0])<<8|p[1];}
inline uint32_t get32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
inline uint64_t get64(const uint8_t* p){return uint64_t(get32(p))<<32|get32(p+4);}
inline void put16(uint8_t* p,uint16_t v){p[0]=uint8_t(v>>8);p[1]=uint8_t(v);}
inline void put32(uint8_t* p,uint32_t v){for(int i=3;i>=0;--i){p[i]=uint8_t(v);v>>=8;}}
inline void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v>>32));put32(p+4,uint32_t(v));}
struct Request {
    Operation operation=Operation::Schedule;
    uint8_t button=0;
    uint64_t session=0,command=0;
    uint32_t count=0,press_us=0,interval_us=0;
};
inline bool parse(const uint8_t* p,size_t n,Request& r) {
    if(n!=REQUEST_SIZE||std::memcmp(p,"UPC1",4)||p[4]!=VERSION||p[7]||get32(p+36))return false;
    if(p[5]!=uint8_t(Operation::Schedule)&&p[5]!=uint8_t(Operation::ReleaseAll))return false;
    r.operation=Operation(p[5]);r.button=p[6];r.session=get64(p+8);r.command=get64(p+16);
    r.count=get32(p+24);r.press_us=get32(p+28);r.interval_us=get32(p+32);
    if(!r.session||!r.command)return false;
    if(r.operation==Operation::ReleaseAll&&(r.button||r.count||r.press_us||r.interval_us))return false;
    return true;
}
inline std::array<uint8_t,REQUEST_SIZE> request(Operation op,uint64_t session,uint64_t command,
                                                uint8_t button=0,uint32_t count=0,
                                                uint32_t press_us=0,uint32_t interval_us=0) {
    std::array<uint8_t,REQUEST_SIZE> p{};std::memcpy(p.data(),"UPC1",4);p[4]=VERSION;
    p[5]=uint8_t(op);p[6]=button;put64(p.data()+8,session);put64(p.data()+16,command);
    put32(p.data()+24,count);put32(p.data()+28,press_us);put32(p.data()+32,interval_us);return p;
}
struct Ack {
    Status status=Status::Invalid;uint8_t button=0;uint64_t session=0,command=0,server_epoch=0;
    uint32_t accepted=0,completed=0;uint16_t queue_depth=0;
};
inline std::array<uint8_t,ACK_SIZE> packet(const Ack& a) {
    std::array<uint8_t,ACK_SIZE> p{};std::memcpy(p.data(),"UPA1",4);p[4]=VERSION;p[5]=uint8_t(a.status);p[6]=a.button;
    put64(p.data()+8,a.session);put64(p.data()+16,a.command);put32(p.data()+24,a.accepted);
    put32(p.data()+28,a.completed);put64(p.data()+32,a.server_epoch);put16(p.data()+40,a.queue_depth);return p;
}
inline bool parse_ack(const uint8_t* p,size_t n,Ack& a) {
    if(n!=ACK_SIZE||std::memcmp(p,"UPA1",4)||p[4]!=VERSION||p[7]||get16(p+42)||get32(p+44))return false;
    if(p[5]<uint8_t(Status::Accepted)||p[5]>uint8_t(Status::StaleCommand))return false;
    a.status=Status(p[5]);a.button=p[6];a.session=get64(p+8);a.command=get64(p+16);
    a.accepted=get32(p+24);a.completed=get32(p+28);a.server_epoch=get64(p+32);a.queue_depth=get16(p+40);return true;
}
}
