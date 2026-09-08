#pragma once
#include "protocol.hpp"
#include <optional>
#include <string>
#include <vector>
namespace receiver {
struct Detection {
    float x = 0, y = 0, w = 0, h = 0, score = 0;
    int cls = 0;
};
struct Settings {
    int version = 1;
    std::string bind_ip = "0.0.0.0", sender_ip = "127.0.0.1", pi_ip = "127.0.0.1", model = "", metadata = "";
    int frame_port = 5000, pi_port = 12345, input_size = 320;
    float confidence = .45f, nms_iou = .45f, fov_radius = 160, reference_x = -1, reference_y = -1;
    std::vector<int> classes;
    bool highest_confidence = false, persistence = false, preview = false;
    float persistence_iou = .2f, aim_x = .5f, aim_y = .5f, offset_x = 0, offset_y = 0;
    float gain_x = .2f, gain_y = .2f, smoothing_ms = 0, deadzone = 1;
    int max_step = 32, activation_button = 2, max_age_ms = 50;
};
void validate(const Settings& s);
Settings load_settings(const std::string& path);
void save_settings(const Settings& s, const std::string& path);
struct Letterbox {
    float scale = 1;
    int resized_w = 0, resized_h = 0, left = 0, top = 0, size = 0;
};
Letterbox letterbox(int width, int height, int input);
std::vector<Detection> decode_yolo(std::span<const float> output, int candidates, int classes, int width,
                                   int height, int input, const Settings& settings);
float iou(const Detection& a, const Detection& b);
struct Correction {
    int dx = 0, dy = 0;
    std::optional<Detection> target;
    float aim_x = 0, aim_y = 0;
};
class Controller {
    std::optional<Detection> previous_;
    double residual_x_ = 0, residual_y_ = 0, smooth_x_ = 0, smooth_y_ = 0;
    int64_t last_ = 0;

  public:
    void reset();
    Correction update(const std::vector<Detection>& detections, int width, int height, const Settings& s,
                      const Telemetry& limits, int64_t now);
};
} // namespace receiver
