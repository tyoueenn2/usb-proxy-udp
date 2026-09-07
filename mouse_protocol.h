#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
#include <algorithm>
struct MouseCommand { int x=0,y=0,wheel=0,pan=0; uint8_t buttons=0; };
struct HidField {
    int offset=0, bits=0, minimum=0, maximum=0;
    int get(const uint8_t* p) const {
        if (!bits) return 0;
        uint32_t v=0;
        for(int i=0;i<bits;++i) v |= uint32_t((p[(offset+i)/8]>>((offset+i)%8))&1)<<i;
        if(minimum<0 && (v & (1u<<(bits-1)))) return int(v)-int(1u<<bits);
        return int(v);
    }
    bool put(uint8_t* p,int value) const {
        if(!bits) return value==0;
        if(value<minimum || value>maximum) return false;
        for(int i=0;i<bits;++i) {
            uint8_t mask=uint8_t(1u<<((offset+i)%8));
            p[(offset+i)/8]=(p[(offset+i)/8]&~mask) | (((uint32_t(value)>>i)&1)?mask:0);
        }
        return true;
    }
};
struct MouseProfile {
    int id=0, size=0;
    HidField x,y,wheel,pan;
    std::map<int,HidField> buttons;
    std::vector<HidField> relative;
    bool matches(const uint8_t* p,unsigned n) const {return n==unsigned(size) && (!id || p[0]==id);}
    uint8_t button_mask(const uint8_t* p) const {
        uint8_t v=0; for(auto& b:buttons) if(b.second.get(p)) v|=1u<<(b.first-1); return v;
    }
    void put_buttons(uint8_t* p,uint8_t mask) const {
        for(auto& b:buttons) b.second.put(p,(mask>>(b.first-1))&1);
    }
    bool encode(const MouseCommand& c,uint8_t* p) const {
        for(auto& f:relative) f.put(p,0);
        put_buttons(p,c.buttons);
        return x.put(p,c.x)&&y.put(p,c.y)&&wheel.put(p,c.wheel)&&pan.put(p,c.pan);
    }
};
// HID short items, global push/pop, report IDs, usage ranges, and nested collections.
// Only relative X/Y in a Mouse application collection are injectable.
inline std::vector<MouseProfile> parse_mouse_descriptor(const uint8_t* p,size_t n) {
    struct Global {int page=0,size=0,count=0,id=0,min=0,max=0;};
    Global g; std::vector<Global> stack; std::vector<bool> collections;
    std::map<int,MouseProfile> profiles; std::map<int,int> offsets;
    std::map<int,bool> shared_report;
    std::vector<uint32_t> usages; uint32_t umin=0,umax=0; bool range=false;
    for(size_t pos=0;pos<n;) {
        uint8_t prefix=p[pos++]; if(prefix==0xfe) return {};
        int len=prefix&3; if(len==3)len=4; if(pos+len>n)return {};
        uint32_t v=0; for(int i=0;i<len;++i)v|=uint32_t(p[pos++])<<(8*i);
        int sv=int(v); if(len && len<4 && (v&(1u<<(len*8-1))))sv=int(v)-int(1u<<(len*8));
        int type=(prefix>>2)&3,tag=prefix>>4;
        if(type==1) {
            switch(tag) {
            case 0:g.page=int(v);break; case 1:g.min=sv;break;
            case 2:g.max=g.min<0?sv:int(v);break; case 7:g.size=int(v);break;
            case 8:if(!v||v>255)return {};g.id=int(v);break;
            case 9:g.count=int(v);break;
            case 10:stack.push_back(g);break;
            case 11:if(stack.empty())return {};g=stack.back();stack.pop_back();break;
            } continue;
        }
        if(type==2) {
            uint32_t u=len==4?v:(uint32_t(g.page)<<16)|v;
            if(tag==0)usages.push_back(u);
            if(tag==1){umin=u;range=true;} if(tag==2)umax=u;
            continue;
        }
        if(type!=0)continue;
        if(tag==10) {
            bool mouse=v==1 ? (!usages.empty() && usages[0]==0x10002) :
                (!collections.empty()&&collections.back());
            collections.push_back(mouse);
        } else if(tag==12) {
            if(collections.empty())return {};
            collections.pop_back();
        } else if(tag==8) {
            if(g.size<0||g.size>32||g.count<0||g.count>4096)return {};
            int& off=offsets[g.id]; if(!off&&g.id)off=8;
            if(off+int64_t(g.size)*g.count>32768)return {};
            auto& r=profiles[g.id];r.id=g.id;
            bool mouse=!collections.empty()&&collections.back();
            if(!mouse && !(v&1))shared_report[g.id]=true;
            if(mouse && !(v&1) && (v&4) &&
               (!(v&2)||g.size<1||g.size>16||g.min>0||g.max<0||g.min>=g.max))return {};
            for(int i=0;i<g.count;++i) {
                uint32_t u=i<int(usages.size())?usages[i]:
                    (range&&umin+unsigned(i)<=umax?umin+i:0);
                HidField f{off+i*g.size,g.size,g.min,g.max};
                if(mouse && !(v&1) && (v&2) && g.size>0 && g.size<=16 &&
                   g.min<=0 && g.max>=0 && g.min<g.max) {
                    int lower=g.min<0?-(1<<(g.size-1)):0;
                    int upper=g.min<0?(1<<(g.size-1))-1:(1<<g.size)-1;
                    if(g.min<lower||g.max>upper)return {};
                    if(v&4)r.relative.push_back(f);
                    if(u==0x10030 && (v&4)){if(r.x.bits)return {};r.x=f;}
                    if(u==0x10031 && (v&4)){if(r.y.bits)return {};r.y=f;}
                    if(u==0x10038 && (v&4))r.wheel=f;
                    if(u==0xc0238 && (v&4))r.pan=f;
                    if((u>>16)==9 && (u&65535)>=1 && (u&65535)<=8 && g.size==1)
                        r.buttons[u&65535]=f;
                }
            }
            off+=g.size*g.count;r.size=(off+7)/8;
        }
        usages.clear();range=false;umin=umax=0;
    }
    if(!collections.empty()||!stack.empty())return {};
    std::vector<MouseProfile> result;
    for(auto& p:profiles) if(!shared_report[p.first]&&p.second.x.bits&&p.second.y.bits&&!p.second.buttons.empty())result.push_back(p.second);
    return result;
}
// Split relative deltas to the actual report limits; never silently wrap or clamp.
inline std::vector<MouseCommand> split_mouse_command(const MouseProfile& p, MouseCommand c) {
    uint8_t supported=0;
    for (const auto& b:p.buttons) supported|=uint8_t(1u<<(b.first-1));
    if (c.buttons & ~supported) return {};
    std::vector<MouseCommand> result;
    do {
        MouseCommand part; part.buttons=c.buttons;
        auto take=[](const HidField& f,int& remaining,int& value) {
            if (!f.bits) return remaining==0;
            value=std::max(f.minimum,std::min(f.maximum,remaining));
            if (remaining && !value) return false;
            remaining-=value; return true;
        };
        if (!take(p.x,c.x,part.x)||!take(p.y,c.y,part.y)||
            !take(p.wheel,c.wheel,part.wheel)||!take(p.pan,c.pan,part.pan)) return {};
        result.push_back(part);
        if (result.size()==32 && (c.x||c.y||c.wheel||c.pan)) return {};
    } while(c.x||c.y||c.wheel||c.pan);
    return result;
}
inline uint32_t read_be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
inline int read_i16(const uint8_t* p){unsigned n=unsigned(p[0])*256+p[1];return n<32768?int(n):int(n)-65536;}
inline bool sequence_newer(uint32_t a,uint32_t b){uint32_t d=a-b;return d&&d<0x80000000u;}
