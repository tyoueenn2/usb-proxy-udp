#include "telemetry_protocol.h"
#include <cassert>
#include <iostream>
int main(){using namespace proxy_telemetry;std::array<uint8_t,24> request{};std::memcpy(request.data(),"UPS1",4);put64(request.data()+8,9);put64(request.data()+16,123);
    Subscription sub;assert(sub.accept(request.data(),24,1000));assert(sub.alive(1000));assert(!sub.accept(request.data(),24,2000));assert(!sub.alive(100001001));
    Snapshot physical{true,2,-127,127,-32768,32767};auto p=sub.packet(7,physical);assert(!std::memcmp(p.data(),"UPT1",4));assert(p[4]==1&&p[5]==2);assert(u64(p.data()+8)==9&&u64(p.data()+16)==7&&u64(p.data()+24)==123);assert(u32(p.data()+36)==uint32_t(-127));assert(u32(p.data()+32)==0);assert(u32(sub.packet(7,physical).data()+32)==1);
    request[4]=1;assert(!sub.accept(request.data(),24,2000));request[4]=0;put64(request.data()+16,124);assert(sub.accept(request.data(),24,2000));auto not_ready=sub.packet(7,{});assert(not_ready[4]==0&&not_ready[5]==0);std::cout<<"telemetry protocol tests passed\n";
}
