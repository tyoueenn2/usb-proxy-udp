#pragma once
#include "mouse_protocol.h"
#include <jsoncpp/json/json.h>
#include <fstream>
#include <tuple>
#include <stdexcept>

struct ReplayEndpoint {int interface_number=0;std::vector<uint8_t> descriptor;};
struct ReplayInterface {int number=0,subclass=0,protocol=0;std::vector<uint8_t> hid,report;};
struct ReplayProfile {
    using Key=std::tuple<int,int,int>;
    std::map<Key,std::vector<uint8_t>> descriptors;
    std::map<int,std::vector<uint8_t>> samples;
    std::vector<ReplayInterface> interfaces;
    std::vector<ReplayEndpoint> endpoints;
    std::vector<uint8_t> device,config;
    int speed=0;
    static std::vector<uint8_t> unhex(const std::string& s) {
        if(s.size()%2||s.size()>8192)throw std::runtime_error("Invalid capture hex length");
        std::vector<uint8_t> out;
        auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;
            if(c>='A'&&c<='F')return c-'A'+10;throw std::runtime_error("Invalid capture hex");};
        for(size_t i=0;i<s.size();i+=2)out.push_back(uint8_t(digit(s[i])*16+digit(s[i+1])));
        return out;
    }
    const std::vector<uint8_t>* lookup(int type,int value,int index) const {
        auto it=descriptors.find({type,value,index});return it==descriptors.end()?nullptr:&it->second;
    }
    static ReplayProfile load(const std::string& path) {
        ReplayProfile p;std::ifstream file(path);if(!file)throw std::runtime_error("Cannot open USB capture");
        std::string line;bool format=false;size_t total=0;
        while(std::getline(file,line)) {
            total+=line.size();if(total>16*1024*1024||line.size()>32768)throw std::runtime_error("Capture too large");
            Json::Value r;Json::Reader reader;if(!reader.parse(line,r))throw std::runtime_error("Invalid/truncated JSONL capture");
            std::string kind=r["kind"].asString();
            if(kind=="format") {if(r["version"].asInt()!=1)throw std::runtime_error("Unsupported capture version");format=true;}
            if(kind=="speed")p.speed=r["value"].asInt();
            if(kind=="mouse_report")p.samples[r["endpoint"].asInt()]=unhex(r["data"].asString());
            if(kind!="control"||r["status"].asString()!="ack")continue;
            auto& s=r["setup"];int type=s["type"].asInt();
            if((type!=0x80&&type!=0x81)||s["request"].asInt()!=6)continue;
            auto bytes=unhex(r["data"].asString());auto& stored=p.descriptors[{type,s["value"].asInt(),s["index"].asInt()}];
            if(!std::equal(stored.begin(),stored.begin()+std::min(stored.size(),bytes.size()),bytes.begin()))
                throw std::runtime_error("Conflicting descriptors: record one device/enumeration mode per file");
            if(bytes.size()>stored.size())stored=bytes;
        }
        if(!format)throw std::runtime_error("Missing capture format record");
        auto device=p.lookup(0x80,0x100,0),config=p.lookup(0x80,0x200,0);
        if(!device||device->size()!=18||(*device)[0]!=18||(*device)[1]!=1||(*device)[17]!=1)
            throw std::runtime_error("Replay requires a complete device descriptor with one configuration");
        if(!config||config->size()<9||(*config)[0]!=9||(*config)[1]!=2||(*config)[5]==0||
            unsigned((*config)[2]|((*config)[3]<<8))!=config->size())throw std::runtime_error("Missing complete configuration descriptor");
        p.device=*device;p.config=*config;
        ReplayInterface* iface=nullptr;std::map<int,int> expected,actual;std::map<int,bool> addresses;
        for(size_t pos=9;pos<p.config.size();) {
            int size=p.config[pos];if(size<2||pos+size>p.config.size())throw std::runtime_error("Invalid configuration block");
            int type=p.config[pos+1];auto at=p.config.begin()+pos;
            if(type==4) {
                if(size!=9||p.config[pos+3]!=0||p.config[pos+5]!=3||expected.count(p.config[pos+2]))
                    throw std::runtime_error("Replay supports HID-only configurations with alternate setting zero");
                p.interfaces.push_back({p.config[pos+2],p.config[pos+6],p.config[pos+7],{}, {}});
                iface=&p.interfaces.back();expected[iface->number]=p.config[pos+4];
            } else if(type==0x21) {
                if(!iface||size<9)throw std::runtime_error("Invalid HID descriptor");iface->hid={at,at+size};
            } else if(type==5) {
                if(!iface||size!=7||(p.config[pos+3]&3)!=3||!(p.config[pos+2]&15)||addresses[p.config[pos+2]])
                    throw std::runtime_error("Replay requires unique interrupt endpoints");
                int maxp=p.config[pos+4]|(p.config[pos+5]<<8);
                if(maxp<1||maxp>1024||p.config[pos+6]==0)throw std::runtime_error("Invalid interrupt endpoint limits");
                addresses[p.config[pos+2]]=true;actual[iface->number]++;
                p.endpoints.push_back({iface->number,{at,at+size}});
            }
            pos+=size;
        }
        if(p.interfaces.size()!=p.config[4])throw std::runtime_error("Interface count mismatch");
        bool mouse=false;
        for(auto& i:p.interfaces) {
            auto report=p.lookup(0x81,0x2200,i.number);
            if(i.hid.size()<9||!report||report->empty()||expected[i.number]!=actual[i.number])
                throw std::runtime_error("Incomplete interface/HID report descriptor recording");
            int length=0;for(size_t k=6;k+2<i.hid.size();k+=3)if(i.hid[k]==0x22)length=i.hid[k+1]|(i.hid[k+2]<<8);
            if(report->size()!=unsigned(length))throw std::runtime_error("Truncated HID report descriptor");
            i.report=*report;auto profiles=parse_mouse_descriptor(report->data(),report->size());
            if(!profiles.empty()) {
                int inputs=0;for(auto& e:p.endpoints)if(e.interface_number==i.number&&(e.descriptor[2]&0x80))inputs++;
                if(inputs!=1)throw std::runtime_error("Mouse replay requires exactly one interrupt IN endpoint per mouse interface");
                for(auto& e:p.endpoints)if(e.interface_number==i.number&&(e.descriptor[2]&0x80))
                    for(auto& layout:profiles)if(layout.size>(e.descriptor[4]|(e.descriptor[5]<<8)))
                        throw std::runtime_error("Mouse report exceeds its endpoint packet size");
                mouse=true;
            }
            p.descriptors[{0x81,0x2100,i.number}]=i.hid;
        }
        if(!mouse)throw std::runtime_error("No supported relative HID mouse in capture");
        if(p.speed<1||p.speed>3)throw std::runtime_error("Capture speed must be low, full or high USB speed");
        return p;
    }
};
