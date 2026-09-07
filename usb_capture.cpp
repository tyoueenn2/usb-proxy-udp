#include "usb_capture.h"
#include <jsoncpp/json/json.h>
#include <fstream>
#include <mutex>
#include <chrono>
#include <atomic>

namespace {
std::ofstream output;
std::mutex mutex;
std::atomic<bool> enabled{false};
Json::Value pending;
auto started=std::chrono::steady_clock::now();
void write(Json::Value record) {
    record["us"]=Json::UInt64(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-started).count());
    Json::StreamWriterBuilder builder;builder["indentation"]="";
    output<<Json::writeString(builder,record)<<'\n';output.flush();
    if(!output) {enabled=false;fprintf(stderr,"USB capture write failed; recording stopped\n");}
}
std::string hex(const void* data,unsigned size) {
    const auto* bytes=static_cast<const uint8_t*>(data);std::string text;
    const char* digits="0123456789abcdef";
    for(unsigned i=0;i<size;++i){text+=digits[bytes[i]>>4];text+=digits[bytes[i]&15];}
    return text;
}
}
bool capture_open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    if(enabled)return false;
    // Avoid silently overwriting a previous device recording.
    std::ifstream existing(path);if(existing.good())return false;
    output.clear();output.open(path);if(!output)return false;
    started=std::chrono::steady_clock::now();enabled=true;
    Json::Value record;record["kind"]="format";record["version"]=1;write(record);return bool(output);
}
void capture_close() {
    std::lock_guard<std::mutex> lock(mutex);enabled=false;output.close();pending=Json::Value();
}
void capture_speed(int speed) {
    if(!enabled)return;std::lock_guard<std::mutex> lock(mutex);
    Json::Value record;record["kind"]="speed";record["value"]=speed;write(record);
}
void capture_event(unsigned type,const CaptureSetup* request) {
    if(!enabled)return;std::lock_guard<std::mutex> lock(mutex);
    Json::Value record;record["kind"]="event";record["type"]=type;
    pending=Json::Value();
    if(request) {
        pending["type"]=request->type;pending["request"]=request->request;
        pending["value"]=request->value;pending["index"]=request->index;pending["length"]=request->length;
        record["setup"]=pending;
    }
    write(record);
}
void capture_reply(const char* status,const void* data,unsigned size) {
    if(!enabled)return;std::lock_guard<std::mutex> lock(mutex);
    if(pending.isNull())return;
    Json::Value record;record["kind"]="control";record["setup"]=pending;
    record["status"]=status;record["data"]=hex(data,size);write(record);pending=Json::Value();
}
void capture_report(unsigned endpoint,unsigned interface_number,const void* data,unsigned size) {
    if(!enabled)return;std::lock_guard<std::mutex> lock(mutex);
    Json::Value record;record["kind"]="mouse_report";record["endpoint"]=endpoint;
    record["interface"]=interface_number;record["data"]=hex(data,size);write(record);
}
