#pragma once
#include "control.hpp"
#include <memory>
namespace receiver {
struct Inference {
    std::span<const float> output;
    int candidates = 0, classes = 0;
    double upload_ms = 0, inference_ms = 0;
};
class Backend {
  public:
    virtual ~Backend() = default;
    virtual Inference run(const Frame& frame) = 0;
    virtual std::string description() const = 0;
    virtual std::pair<size_t, size_t> device_memory() const {
        return {};
    }
};
std::unique_ptr<Backend> make_backend(const Settings& settings);
} // namespace receiver
