#include "telemetry_protocol.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

std::vector<uint8_t> from_hex(const std::string& text) {
    auto nibble=[](char c)->uint8_t{return c<='9'?uint8_t(c-'0'):uint8_t((c|32)-'a'+10);};
    std::vector<uint8_t> result;
    for(size_t i=0;i<text.size();) {
        while(i<text.size()&&text[i]==' ')++i;
        if(i==text.size())break;
        assert(i+1<text.size());result.push_back(uint8_t(nibble(text[i])<<4|nibble(text[i+1])));i+=2;
    }
    return result;
}
template<size_t N> void golden(const std::array<uint8_t,N>& actual,const std::string& expected) {
    auto bytes=from_hex(expected);assert(bytes.size()==N);
    assert(std::equal(actual.begin(),actual.end(),bytes.begin()));
}

int main() {
    using namespace proxy_telemetry;Subscription subscription;
    std::array<uint8_t,24> request{};std::memcpy(request.data(),"UPS1",4);
    put64(request.data()+8,9);put64(request.data()+16,123);
    golden(request,"55 50 53 31 00000000 0000000000000009 000000000000007b");
    assert(subscription.accept(request.data(),request.size(),100)&&subscription.version==1);
    Snapshot state;state.ready=true;state.physical=2;state.persistent=1;state.scheduled=4;
    state.xmin=-127;state.xmax=127;state.ymin=-32768;state.ymax=32767;
    state.physical_dx=-3;state.physical_dy=5;state.accepted=50;state.completed=49;
    state.active=1;state.queued=2;state.generation=77;state.poll_us=125;
    state.snapshot_ns=0x0102030405060708ull;state.cumulative_x=-2;
    state.cumulative_y=0x1122334455667788ll;state.physical_age_us=99;
    state.physical_received=1000;state.physical_submitted=999;state.output_queue=3;
    state.synthetic_pending=4;state.superseded=5;state.writer_failures=6;

    auto one=subscription.packet1(7,state);
    golden(one,"55505431 01020000 0000000000000009 0000000000000007 000000000000007b "
               "00000000 ffffff81 0000007f ffff8000 00007fff 00000000");

    std::memcpy(request.data(),"UPS2",4);put64(request.data()+16,124);
    assert(subscription.accept(request.data(),request.size(),200)&&subscription.version==2);
    auto two=subscription.packet2(7,state);
    golden(two,"55505432 01020104 0000000000000009 0000000000000007 000000000000007c "
               "00000001 ffffff81 0000007f ffff8000 00007fff fffffffd 00000005 "
               "00000032 00000031 0001 0002 000000000000004d");

    std::memcpy(request.data(),"UPS3",4);put64(request.data()+16,125);
    assert(subscription.accept(request.data(),request.size(),300)&&subscription.version==3);
    auto three=subscription.packet3(7,state);
    golden(three,"55505433 01020104 0000000000000009 0000000000000007 000000000000007d "
                 "00000002 ffffff81 0000007f ffff8000 00007fff 0000007d "
                 "000000000000004d 0102030405060708 fffffffffffffffe 1122334455667788 "
                 "00000063 00000032 00000031 0001 0002 000003e8 000003e7 "
                 "0003 0004 00000005 00000006 00000000");

    auto malformed=request;malformed[4]=1;assert(!subscription.accept(malformed.data(),malformed.size(),400));
    malformed=request;std::memcpy(malformed.data(),"UPS4",4);assert(!subscription.accept(malformed.data(),malformed.size(),400));
    assert(!subscription.accept(request.data(),request.size()-1,400));
    assert(!subscription.accept(request.data(),request.size(),400)); // duplicate token inside freshness window
    Snapshot only_age=state;only_age.physical_age_us=100;assert(only_age==state);
    std::cout<<"UPS1/2/3 and UPT1/2/3 golden vectors, reserved bytes and physical-only fields passed\n";
}
