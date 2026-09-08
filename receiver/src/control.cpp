#include "receiver/control.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace receiver {
Letterbox letterbox(int w, int h, int n) {
    if (w <= 0 || h <= 0 || n <= 0)
        throw std::invalid_argument("Invalid image dimensions");
    float scale = std::min(float(n) / w, float(n) / h);
    int rw = std::max(1, int(std::round(w * scale))), rh = std::max(1, int(std::round(h * scale)));
    return {scale, rw, rh, (n - rw) / 2, (n - rh) / 2, n};
}
float iou(const Detection& a, const Detection& b) {
    float area = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x)) *
                 std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    float denom = a.w * a.h + b.w * b.h - area;
    return denom > 0 ? area / denom : 0;
}
std::vector<Detection> decode_yolo(std::span<const float> out, int count, int nc, int width, int height,
                                   int input, const Settings& s) {
    if (count <= 0 || nc <= 0 || out.size() != size_t(count) * size_t(nc + 4))
        throw std::runtime_error("YOLO output must be [1,4+classes,candidates]");
    auto box = letterbox(width, height, input);
    std::vector<Detection> candidates;
    candidates.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        float score = -1;
        int cls = -1;
        for (int c = 0; c < nc; ++c) {
            if (!s.classes.empty() && std::find(s.classes.begin(), s.classes.end(), c) == s.classes.end())
                continue;
            float v = out[size_t(4 + c) * count + i];
            if (std::isfinite(v) && v > score) {
                score = v;
                cls = c;
            }
        }
        if (score < s.confidence || score > 1 || cls < 0)
            continue;
        float cx = out[i], cy = out[count + i], w = out[2 * count + i], h = out[3 * count + i];
        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) || !std::isfinite(h) || w <= 0 ||
            h <= 0)
            continue;
        float x1 = std::clamp((cx - w * .5f - box.left) / box.scale, 0.f, float(width)),
              y1 = std::clamp((cy - h * .5f - box.top) / box.scale, 0.f, float(height));
        float x2 = std::clamp((cx + w * .5f - box.left) / box.scale, 0.f, float(width)),
              y2 = std::clamp((cy + h * .5f - box.top) / box.scale, 0.f, float(height));
        if (x2 > x1 && y2 > y1)
            candidates.push_back({x1, y1, x2 - x1, y2 - y1, score, cls});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](auto& a, auto& b) { return a.score > b.score; });
    // Bound worst-case NMS time on dense/noisy model output.
    if (candidates.size() > 1000)
        candidates.resize(1000);
    std::vector<Detection> kept;
    kept.reserve(300);
    for (auto& d : candidates) {
        bool suppressed = false;
        for (auto& k : kept)
            if (k.cls == d.cls && iou(k, d) > s.nms_iou) {
                suppressed = true;
                break;
            }
        if (!suppressed)
            kept.push_back(d);
        if (kept.size() == 300)
            break;
    }
    return kept;
}
void Controller::reset() {
    previous_.reset();
    residual_x_ = residual_y_ = smooth_x_ = smooth_y_ = 0;
    last_ = 0;
}
Correction Controller::update(const std::vector<Detection>& ds, int width, int height, const Settings& s,
                              const Telemetry& limits, int64_t now) {
    const float rx = s.reference_x < 0 ? width * .5f : s.reference_x,
                ry = s.reference_y < 0 ? height * .5f : s.reference_y;
    auto point = [&](const Detection& d) {
        return std::pair{d.x + d.w * s.aim_x + s.offset_x, d.y + d.h * s.aim_y + s.offset_y};
    };
    auto eligible = [&](const Detection& d) {
        auto [x, y] = point(d);
        float dx = x - rx, dy = y - ry;
        return std::isfinite(x) && std::isfinite(y) && d.score >= s.confidence &&
               (s.classes.empty() ||
                std::find(s.classes.begin(), s.classes.end(), d.cls) != s.classes.end()) &&
               dx * dx + dy * dy <= s.fov_radius * s.fov_radius;
    };
    const Detection* best = nullptr;
    float rank = -1e30f, best_match = s.persistence_iou;
    if (s.persistence && previous_)
        for (auto& d : ds)
            if (eligible(d) && d.cls == previous_->cls) {
                float match = iou(d, *previous_);
                if (match >= best_match) {
                    best = &d;
                    best_match = match;
                }
            }
    if (!best)
        for (auto& d : ds)
            if (eligible(d)) {
                auto [x, y] = point(d);
                float value = s.highest_confidence ? d.score : -((x - rx) * (x - rx) + (y - ry) * (y - ry));
                if (value > rank) {
                    rank = value;
                    best = &d;
                }
            }
    if (!best) {
        reset();
        return {};
    }
    bool same = previous_ && previous_->cls == best->cls && iou(*previous_, *best) >= s.persistence_iou;
    if (!same) {
        reset();
    }
    auto [x, y] = point(*best);
    double ex = x - rx, ey = y - ry;
    if (std::hypot(ex, ey) <= s.deadzone) {
        ex = ey = 0;
        residual_x_ = residual_y_ = smooth_x_ = smooth_y_ = 0;
    }
    double vx = ex * s.gain_x, vy = ey * s.gain_y;
    if (s.smoothing_ms > 0) {
        double dt = last_ ? std::clamp(double(now - last_) / 1e6, 0.0, 100.0) : 1000.0 / 120;
        double a = -std::expm1(-dt / s.smoothing_ms);
        smooth_x_ += a * (vx - smooth_x_);
        smooth_y_ += a * (vy - smooth_y_);
        vx = smooth_x_;
        vy = smooth_y_;
    }
    const int minx = std::max(-s.max_step, limits.xmin), maxx = std::min(s.max_step, limits.xmax),
              miny = std::max(-s.max_step, limits.ymin), maxy = std::min(s.max_step, limits.ymax);
    vx = std::clamp(vx, double(minx), double(maxx));
    vy = std::clamp(vy, double(miny), double(maxy));
    double ax = std::clamp(vx + residual_x_, double(minx), double(maxx)),
           ay = std::clamp(vy + residual_y_, double(miny), double(maxy));
    int dx = int(std::trunc(ax)), dy = int(std::trunc(ay));
    residual_x_ = ax - dx;
    residual_y_ = ay - dy;
    previous_ = *best;
    last_ = now;
    return {dx, dy, *best, x, y};
}
} // namespace receiver
