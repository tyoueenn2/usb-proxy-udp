#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <algorithm>
// Versioned status-only extension. Existing UPX1 and ASCII commands are unchanged.
namespace proxy_telemetry {
inline uint32_t u32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
inline uint64_t u64(const uint8_t* p){return uint64_t(u32(p))<<32|u32(p+4);}
inline void put32(uint8_t* p,uint32_t v){for(int i=3;i>=0;--i){p[i]=uint8_t(v);v>>=8;}}
inline void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v>>32));put32(p+4,uint32_t(v));}
struct Snapshot {
    bool ready=false;uint8_t physical=0;int xmin=0,xmax=0,ymin=0,ymax=0;
    bool operator==(const Snapshot& b)const{return ready==b.ready&&physical==b.physical&&xmin==b.xmin&&xmax==b.xmax&&ymin==b.ymin&&ymax==b.ymax;}
};
struct Subscription {
    uint64_t client=0,token=0,last_ns=0;uint32_t sequence=0;
    bool accept(const uint8_t* p,size_t n,uint64_t now){
        if(n!=24||std::memcmp(p,"UPS1",4)||u32(p+4))return false;
        auto c=u64(p+8),t=u64(p+16);if(!c||!t)return false;
        if(c==client&&now-last_ns<=100000000&&t<=token)return false;
        if(c!=client)sequence=0;
        client=c;token=t;last_ns=now;return true;
    }
    bool alive(uint64_t now)const{return client&&now>=last_ns&&now-last_ns<=100000000;}
    std::array<uint8_t,56> packet(uint64_t server,const Snapshot& s){
        std::array<uint8_t,56> p{};std::memcpy(p.data(),"UPT1",4);p[4]=s.ready;p[5]=s.physical;
        put64(p.data()+8,client);put64(p.data()+16,server);put64(p.data()+24,token);put32(p.data()+32,sequence++);
        put32(p.data()+36,uint32_t(s.xmin));put32(p.data()+40,uint32_t(s.xmax));put32(p.data()+44,uint32_t(s.ymin));put32(p.data()+48,uint32_t(s.ymax));return p;
    }
};
}
