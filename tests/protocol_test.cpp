#include "mouse_protocol.h"
#include <cassert>
#include <iostream>
#include <random>

std::vector<uint8_t> descriptor(int bits, bool report_id) {
    std::vector<uint8_t> d={0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0};
    if(report_id)d.insert(d.end(),{0x85,2});
    const uint8_t buttons[]={0x05,9,0x19,1,0x29,5,0x15,0,0x25,1,0x75,1,0x95,5,0x81,2,
        0x75,3,0x95,1,0x81,1,0x05,1,0x09,0x30,0x09,0x31};
    d.insert(d.end(),std::begin(buttons),std::end(buttons));
    if(bits==8)d.insert(d.end(),{0x15,0x81,0x25,0x7f});
    else if(bits==12)d.insert(d.end(),{0x16,0x00,0xf8,0x26,0xff,0x07});
    else d.insert(d.end(),{0x16,0x00,0x80,0x26,0xff,0x7f});
    d.insert(d.end(),{0x75,uint8_t(bits),0x95,2,0x81,6,
        0x09,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,6,0xc0,0xc0});
    return d;
}
int main() {
    for(int bits:{8,12,16})for(bool id:{false,true}) {
        auto d=descriptor(bits,id);auto profiles=parse_mouse_descriptor(d.data(),d.size());
        assert(profiles.size()==1);const auto& p=profiles[0];
        std::vector<uint8_t> report(p.size,0);if(id)report[0]=2;
        assert(p.matches(report.data(),report.size()));
        MouseCommand c;c.x=-100;c.y=100;c.wheel=-1;c.buttons=0x15;
        assert(p.encode(c,report.data()));
        assert(p.x.get(report.data())==-100 && p.y.get(report.data())==100);
        assert(p.wheel.get(report.data())==-1 && p.button_mask(report.data())==0x15);
        c={};assert(p.encode(c,report.data()));assert(p.wheel.get(report.data())==0);
        c.x=320;c.y=-320;auto parts=split_mouse_command(p,c);
        assert(parts.size()==(bits==8?3u:1u));int x=0,y=0;
        for(auto& part:parts){assert(p.encode(part,report.data()));x+=p.x.get(report.data());y+=p.y.get(report.data());}
        assert(x==320&&y==-320);
        c.pan=1;assert(split_mouse_command(p,c).empty());
        c={};c.buttons=128;assert(split_mouse_command(p,c).empty());
        for(size_t n=0;n<d.size();++n)assert(parse_mouse_descriptor(d.data(),n).empty());
        if(id){report[0]=3;assert(!p.matches(report.data(),report.size()));}
    }
    assert(sequence_newer(0,0xffffffff));assert(!sequence_newer(9,10));
    assert(!sequence_newer(10,10));assert(!sequence_newer(0x80000000,0));
    const uint8_t negative[]={0x80,0};assert(read_i16(negative)==-32768);
    std::mt19937 rng(1234);
    for(int iteration=0;iteration<20000;++iteration){
        std::vector<uint8_t> bytes(rng()%128);
        for(auto& b:bytes)b=uint8_t(rng());
        auto profiles=parse_mouse_descriptor(bytes.data(),bytes.size());
        for(const auto& p:profiles){
            assert(p.size>0&&p.size<=4096);
            std::vector<uint8_t> r(p.size,0);MouseCommand c;p.encode(c,r.data());
        }
    }
    std::cout<<"HID layouts, splitting, malformed descriptors and sequence tests passed\n";
}
