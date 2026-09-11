#pragma once
#include "click_protocol.h"
#include <array>
#include <algorithm>

namespace proxy_telemetry {
using click_protocol::get32;using click_protocol::get64;using click_protocol::put32;using click_protocol::put64;
struct Snapshot {
    bool ready=false;uint8_t physical=0,persistent=0,scheduled=0;
    int xmin=0,xmax=0,ymin=0,ymax=0,physical_dx=0,physical_dy=0;
    uint32_t accepted=0,completed=0;uint16_t active=0,queued=0;uint64_t generation=0;
    bool operator==(const Snapshot& b)const {
        return ready==b.ready&&physical==b.physical&&persistent==b.persistent&&scheduled==b.scheduled&&
            xmin==b.xmin&&xmax==b.xmax&&ymin==b.ymin&&ymax==b.ymax&&
            physical_dx==b.physical_dx&&physical_dy==b.physical_dy&&accepted==b.accepted&&
            completed==b.completed&&active==b.active&&queued==b.queued&&generation==b.generation;
    }
};
struct Subscription {
    uint64_t client=0,token=0,last_ns=0;uint32_t sequence=0;uint8_t version=0;
    bool accept(const uint8_t* p,size_t n,uint64_t now) {
        if(n!=24||(std::memcmp(p,"UPS1",4)&&std::memcmp(p,"UPS2",4))||get32(p+4))return false;
        auto c=get64(p+8),t=get64(p+16);if(!c||!t)return false;
        if(c==client&&now-last_ns<=100000000&&t<=token)return false;
        if(c!=client)sequence=0;
        client=c;token=t;last_ns=now;version=p[3]-'0';return true;
    }
    bool alive(uint64_t now)const{return client&&now>=last_ns&&now-last_ns<=100000000;}
    std::array<uint8_t,56> packet1(uint64_t server,const Snapshot& s) {
        std::array<uint8_t,56> p{};std::memcpy(p.data(),"UPT1",4);p[4]=s.ready;p[5]=s.physical;
        put64(p.data()+8,client);put64(p.data()+16,server);put64(p.data()+24,token);put32(p.data()+32,sequence++);
        put32(p.data()+36,uint32_t(s.xmin));put32(p.data()+40,uint32_t(s.xmax));
        put32(p.data()+44,uint32_t(s.ymin));put32(p.data()+48,uint32_t(s.ymax));return p;
    }
    std::array<uint8_t,80> packet2(uint64_t server,const Snapshot& s) {
        std::array<uint8_t,80> p{};std::memcpy(p.data(),"UPT2",4);p[4]=s.ready;p[5]=s.physical;
        p[6]=s.persistent;p[7]=s.scheduled;put64(p.data()+8,client);put64(p.data()+16,server);
        put64(p.data()+24,token);put32(p.data()+32,sequence++);put32(p.data()+36,uint32_t(s.xmin));
        put32(p.data()+40,uint32_t(s.xmax));put32(p.data()+44,uint32_t(s.ymin));
        put32(p.data()+48,uint32_t(s.ymax));put32(p.data()+52,uint32_t(s.physical_dx));
        put32(p.data()+56,uint32_t(s.physical_dy));put32(p.data()+60,s.accepted);put32(p.data()+64,s.completed);
        click_protocol::put16(p.data()+68,s.active);click_protocol::put16(p.data()+70,s.queued);
        put64(p.data()+72,s.generation);return p;
    }
};
}
