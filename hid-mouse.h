#ifndef HID_MOUSE_H
#define HID_MOUSE_H

#include <cstdint>
#include <vector>

struct HidField {
    unsigned bit_offset, bit_size;
    int logical_min, logical_max;
};

struct HidMouseReport {
    int interface_number;
    uint8_t report_id;
    bool has_report_id;
    unsigned report_bits;
    std::vector<HidField> buttons;
    HidField x, y;
    bool has_x, has_y;
};

// Parses the Input items in a HID report descriptor.  Only reports containing
// relative X/Y fields are returned; those are the reports usable by +move.
std::vector<HidMouseReport> parse_hid_mouse_reports(int interface_number,
                                                    const std::vector<uint8_t>& descriptor);
uint32_t hid_get_bits(const uint8_t *data, unsigned bit, unsigned size);
void hid_set_bits(std::vector<uint8_t>& data, unsigned bit, unsigned size, int value);

#endif
