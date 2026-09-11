#include "click_protocol.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace click_protocol;
    auto bytes=request(Operation::Schedule,0x1122334455667788ull,99,5,2500,8000,20000);
    Request decoded;assert(parse(bytes.data(),bytes.size(),decoded));
    assert(decoded.operation==Operation::Schedule&&decoded.session==0x1122334455667788ull&&decoded.command==99);
    assert(decoded.button==5&&decoded.count==2500&&decoded.press_us==8000&&decoded.interval_us==20000);
    {auto invalid=bytes;invalid[4]=2;assert(!parse(invalid.data(),invalid.size(),decoded));}
    for(size_t i:{size_t(7),size_t(36)}){auto invalid=bytes;invalid[i]=1;assert(!parse(invalid.data(),invalid.size(),decoded));}
    assert(!parse(bytes.data(),bytes.size()-1,decoded));
    auto release=request(Operation::ReleaseAll,7,100);assert(parse(release.data(),release.size(),decoded));
    release[6]=1;assert(!parse(release.data(),release.size(),decoded));

    Ack source{Status::Duplicate,3,7,100,0x8877665544332211ull,25,14,9};
    auto wire=packet(source);Ack target;assert(parse_ack(wire.data(),wire.size(),target));
    assert(target.status==Status::Duplicate&&target.button==3&&target.session==7&&target.command==100);
    assert(target.server_epoch==0x8877665544332211ull&&target.accepted==25&&target.completed==14&&target.queue_depth==9);
    wire[44]=1;assert(!parse_ack(wire.data(),wire.size(),target));
    std::cout<<"UPC1/UPA1 version, fields, reserved bytes, release-all and statuses passed\n";
}
