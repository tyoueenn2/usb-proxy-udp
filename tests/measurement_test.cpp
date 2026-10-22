#include "usb_measurement.h"
#include <fstream>
#include <string>
// No Raw Gadget or USB device: exercise the actual diagnostic serializer.
struct Report {
    bool mouse_managed=true;
    unsigned mouse_report_kind=REPORT_TRANSACTION_RETURN;
    unsigned long long click_session=18446744073709551615ull, click_command=3, mouse_generation=2;
    struct { unsigned length=8; } inner;
    int synthetic_x=-4, synthetic_y=6;
};
int main(int argc, char** argv) {
    if(argc!=2) return 2;
    setenv("USB_PROXY_MEASUREMENT_LOG",argv[1],1);
    Report io;
    measure_usb_write(io,100,250,-1,5);
    measure_usb_write(io,300,500,8,0);
    return 0;
}
