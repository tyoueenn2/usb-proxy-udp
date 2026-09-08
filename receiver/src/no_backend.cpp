#include "receiver/backend.hpp"
#include <stdexcept>
namespace receiver {
std::unique_ptr<Backend> make_backend(const Settings&) {
    throw std::runtime_error(
        "This build has no CUDA backend. Use a TensorRT build, or explicit loopback simulation for testing.");
}
} // namespace receiver
