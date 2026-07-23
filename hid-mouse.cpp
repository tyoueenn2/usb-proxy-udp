#include "hid-mouse.h"
#include <map>
#include <algorithm>

uint32_t hid_get_bits(const uint8_t *data, unsigned bit, unsigned size) {
    uint32_t value = 0;
    for (unsigned i = 0; i < size && i < 32; ++i)
        if (data[(bit + i) / 8] & (1u << ((bit + i) % 8))) value |= 1u << i;
    return value;
}
void hid_set_bits(std::vector<uint8_t>& data, unsigned bit, unsigned size, int value) {
    for (unsigned i = 0; i < size && i < 32; ++i) {
        uint8_t mask = 1u << ((bit + i) % 8);
        uint8_t& out = data[(bit + i) / 8];
        if ((uint32_t)value & (1u << i)) out |= mask; else out &= ~mask;
    }
}

static int signed_value(uint32_t v, unsigned size) {
    if (size && size < 32 && (v & (1u << (size - 1)))) v |= ~((1u << size) - 1);
    return (int)v;
}

std::vector<HidMouseReport> parse_hid_mouse_reports(int iface, const std::vector<uint8_t>& d) {
    struct Global { unsigned page = 0, size = 0, count = 0; int min = 0, max = 0; uint8_t id = 0; } g;
    std::vector<Global> stack;
    std::map<unsigned, HidMouseReport> reports;
    std::vector<unsigned> usages; unsigned usage_min = 0, usage_max = 0;
    std::map<unsigned, unsigned> positions;
    for (size_t p = 0; p < d.size();) {
        uint8_t prefix = d[p++];
        if (prefix == 0xfe) { if (p + 1 >= d.size()) break; p += 2 + d[p]; continue; }
        unsigned bytes = prefix & 3; if (bytes == 3) bytes = 4;
        unsigned type = (prefix >> 2) & 3, tag = (prefix >> 4) & 15;
        if (p + bytes > d.size()) break;
        uint32_t value = 0; for (unsigned i = 0; i < bytes; ++i) value |= (uint32_t)d[p++] << (8*i);
        int svalue = signed_value(value, bytes * 8);
        if (type == 1) { // global
            if (tag == 0) g.page = value; else if (tag == 1) g.min = svalue;
            else if (tag == 2) g.max = svalue; else if (tag == 7) g.size = value;
            else if (tag == 8) g.id = value; else if (tag == 9) g.count = value;
            else if (tag == 10) stack.push_back(g); else if (tag == 11 && !stack.empty()) { g = stack.back(); stack.pop_back(); }
        } else if (type == 2) { // local
            if (tag == 0) usages.push_back(value); else if (tag == 1) usage_min = value; else if (tag == 2) usage_max = value;
        } else if (type == 0 && tag == 8) { // Input
            unsigned& pos = positions[g.id];
            if (!(value & 1)) { // data, not constant
                HidMouseReport& r = reports[g.id]; r.interface_number = iface; r.report_id = g.id; r.has_report_id = g.id != 0;
                for (unsigned i = 0; i < g.count; ++i) {
                    unsigned u = i < usages.size() ? usages[i] : (usage_min || usage_max ? usage_min + i : 0);
                    HidField f = {pos + i*g.size, g.size, g.min, g.max};
                    if (g.page == 9) r.buttons.push_back(f);
                    else if ((value & 0x04) && g.page == 1 && u == 0x30) { r.x = f; r.has_x = true; }
                    else if ((value & 0x04) && g.page == 1 && u == 0x31) { r.y = f; r.has_y = true; }
                }
            }
            pos += g.size * g.count;
            usages.clear(); usage_min = usage_max = 0;
        } else if (type == 0) { usages.clear(); usage_min = usage_max = 0; }
    }
    std::vector<HidMouseReport> out;
    for (std::map<unsigned, HidMouseReport>::iterator it = reports.begin(); it != reports.end(); ++it) {
        it->second.report_bits = positions[it->first];
        if (it->second.has_x && it->second.has_y) out.push_back(it->second);
    }
    return out;
}
