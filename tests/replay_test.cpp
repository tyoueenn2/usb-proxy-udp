#include "replay_profile.h"
#include "usb_capture.h"
#include <filesystem>
#include <chrono>
#include <cassert>
#include <iostream>

int main() {
    auto name=std::filesystem::temp_directory_path()/
        ("usb-replay-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".jsonl");
    assert(capture_open(name.string()));assert(!capture_open(name.string()));capture_speed(2);
    std::vector<uint8_t> report={0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0,
        0x05,9,0x19,1,0x29,3,0x15,0,0x25,1,0x95,3,0x75,1,0x81,2,
        0x95,1,0x75,5,0x81,1,0x05,1,0x09,0x30,0x09,0x31,0x15,0x81,
        0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0,0xc0};
    std::vector<uint8_t> device={18,1,0,2,0,0,0,64,0x6d,4,0x39,0xc5,0,1,0,0,0,1};
    std::vector<uint8_t> config={9,2,34,0,1,1,0,0x80,50,
        9,4,0,0,1,3,1,2,0,9,0x21,0x11,1,0,1,0x22,uint8_t(report.size()),0,
        7,5,0x81,3,8,0,1};
    auto descriptor=[&](unsigned type,unsigned value,unsigned index,const std::vector<uint8_t>& data) {
        CaptureSetup setup{type,6,value,index,4096};capture_event(2,&setup);capture_reply("ack",data.data(),data.size());
    };
    descriptor(0x80,0x100,0,{device.begin(),device.begin()+8});descriptor(0x80,0x100,0,device);
    descriptor(0x80,0x200,0,{config.begin(),config.begin()+9});descriptor(0x80,0x200,0,config);
    descriptor(0x81,0x2200,0,report);
    descriptor(0x80,0x301,0x409,{6,3,'A',0,'B',0});
    CaptureSetup vendor{0xc0,1,0,0,4};capture_event(2,&vendor);capture_reply("stall",nullptr,0);
    uint8_t seed[]={1,2,3};capture_report(0x81,0,seed,sizeof(seed));capture_event(7,nullptr);capture_close();
    auto p=ReplayProfile::load(name.string());assert(p.device==device&&p.config==config);
    assert(p.speed==2&&p.interfaces.size()==1&&p.endpoints.size()==1&&p.samples[0x81].size()==3);
    assert(p.lookup(0x80,0x301,0x409)&&!p.lookup(0x80,0x301,0));
    assert(p.lookup(0x81,0x2100,0)->size()==9);
    assert(!p.lookup(0xc0,0,0)); // proprietary control traffic is not blindly replayed
    auto bad=name;bad+=".bad";
    {std::ifstream in(name);std::ofstream out(bad);out<<in.rdbuf()<<"{broken";}
    bool rejected=false;try{ReplayProfile::load(bad.string());}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    {std::ofstream out(bad);out<<"{\"kind\":\"format\",\"version\":1}\n";}
    rejected=false;try{ReplayProfile::load(bad.string());}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    std::filesystem::remove(name);std::filesystem::remove(bad);
    std::cout<<"USB capture/replay: roundtrip, partial descriptors, language IDs, stalls and malformed captures passed\n";
}
