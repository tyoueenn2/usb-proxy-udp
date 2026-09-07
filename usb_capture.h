#pragma once
#include <cstdint>
#include <string>
struct CaptureSetup {unsigned type,request,value,index,length;};
bool capture_open(const std::string& path);
void capture_close();
void capture_speed(int speed);
void capture_event(unsigned type, const CaptureSetup* request);
void capture_reply(const char* status, const void* data, unsigned size);
void capture_report(unsigned endpoint, unsigned interface_number, const void* data, unsigned size);
