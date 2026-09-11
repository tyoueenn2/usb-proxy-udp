#include "telemetry_protocol.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace proxy_telemetry;Subscription subscription;
    std::array<uint8_t,24> request{};std::memcpy(request.data(),"UPS1",4);
    put64(request.data()+8,9);put64(request.data()+16,123);
    assert(subscription.accept(request.data(),request.size(),100)&&subscription.version==1);
    Snapshot state;state.ready=true;state.physical=2;state.persistent=1;state.scheduled=4;
    state.xmin=-127;state.xmax=127;state.ymin=-32768;state.ymax=32767;
    state.physical_dx=-3;state.physical_dy=5;state.accepted=50;state.completed=49;state.active=1;state.queued=2;state.generation=77;
    auto one=subscription.packet1(7,state);assert(!std::memcmp(one.data(),"UPT1",4));
    assert(one[4]==1&&one[5]==2&&one[6]==0&&one[7]==0); // UPT1 remains physical-only and unchanged.
    assert(get64(one.data()+8)==9&&get64(one.data()+16)==7&&get64(one.data()+24)==123);
    std::memcpy(request.data(),"UPS2",4);put64(request.data()+16,124);
    assert(subscription.accept(request.data(),request.size(),200)&&subscription.version==2);
    auto two=subscription.packet2(7,state);assert(!std::memcmp(two.data(),"UPT2",4));
    assert(two[5]==2&&two[6]==1&&two[7]==4); // Distinct physical, persistent and scheduled masks.
    assert(int32_t(get32(two.data()+52))==-3&&int32_t(get32(two.data()+56))==5);
    assert(get32(two.data()+60)==50&&get32(two.data()+64)==49);
    assert(click_protocol::get16(two.data()+68)==1&&click_protocol::get16(two.data()+70)==2&&get64(two.data()+72)==77);
    // An injected movement has no input to the telemetry codec; physical direction stays explicitly supplied.
    std::cout<<"UPS1/UPT1 compatibility and UPT2 physical/synthetic separation passed\n";
}
