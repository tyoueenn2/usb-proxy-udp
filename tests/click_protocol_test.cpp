#include "click_protocol.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace click_protocol;
    const std::array<uint8_t,REQUEST_SIZE> v1_golden={
        'U','P','C','1',1,1,5,0,
        0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
        0,0,0,0,0,0,0,0x63,
        0,0,0x09,0xc4,0,0,0x1f,0x40,0,0,0x4e,0x20,0,0,0,0
    };
    auto bytes=request(Operation::Schedule,0x1122334455667788ull,99,5,2500,8000,20000,VERSION1);
    assert(bytes==v1_golden);
    Request decoded;assert(parse(bytes.data(),bytes.size(),decoded));
    assert(decoded.version==VERSION1&&decoded.operation==Operation::Schedule&&
           decoded.session==0x1122334455667788ull&&decoded.command==99);
    assert(decoded.button==5&&decoded.count==2500&&decoded.press_us==8000&&decoded.interval_us==20000);

    auto v2=request(Operation::Schedule,0x1122334455667788ull,99,5,2500,8000,20000,VERSION2);
    auto v2_golden=v1_golden;v2_golden[4]=2;assert(v2==v2_golden);
    assert(parse(v2.data(),v2.size(),decoded)&&decoded.version==VERSION2);
    {auto invalid=bytes;invalid[4]=3;assert(!parse(invalid.data(),invalid.size(),decoded));}
    for(size_t i:{size_t(7),size_t(36)}){auto invalid=bytes;invalid[i]=1;assert(!parse(invalid.data(),invalid.size(),decoded));}
    assert(!parse(bytes.data(),bytes.size()-1,decoded));
    auto release=request(Operation::ReleaseAll,7,100,0,0,0,0,VERSION2);
    assert(parse(release.data(),release.size(),decoded)&&decoded.version==VERSION2);
    release[6]=1;assert(!parse(release.data(),release.size(),decoded));

    const std::array<uint8_t,ACK_SIZE> ack_golden={
        'U','P','A','1',2,2,3,0,
        0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0x64,
        0,0,0,0x19,0,0,0,0x0e,
        0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
        0,9,0,0,0,0,0,0
    };
    Ack source{Status::Duplicate,3,7,100,0x8877665544332211ull,25,14,9};source.version=VERSION2;
    auto wire=packet(source);assert(wire==ack_golden);Ack target;assert(parse_ack(wire.data(),wire.size(),target));
    assert(target.version==VERSION2&&target.status==Status::Duplicate&&target.button==3&&
           target.session==7&&target.command==100);
    assert(target.server_epoch==0x8877665544332211ull&&target.accepted==25&&
           target.completed==14&&target.queue_depth==9);
    {auto invalid=wire;invalid[4]=0;assert(!parse_ack(invalid.data(),invalid.size(),target));}
    {auto invalid=wire;invalid[42]=1;assert(!parse_ack(invalid.data(),invalid.size(),target));}
    {auto invalid=wire;invalid[44]=1;assert(!parse_ack(invalid.data(),invalid.size(),target));}
    source.version=VERSION1;wire=packet(source);assert(wire[4]==1&&parse_ack(wire.data(),wire.size(),target));
    std::cout<<"UPC1/UPA1 v1/v2 golden vectors, fields, reserved bytes and release-all passed\n";
}
