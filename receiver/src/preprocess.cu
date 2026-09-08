#include <cstdint>
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include "preprocess_kernel.cuh"
namespace receiver {
void launch_preprocess(const uint8_t* raw, float* tensor, int w, int h, int channels, int size, int rw,
                       int rh, int left, int top, cudaStream_t stream) {
    preprocess_kernel<<<dim3((size + 15) / 16, (size + 15) / 16), dim3(16, 16), 0, stream>>>(
        raw, tensor, w, h, channels, size, rw, rh, left, top);
}
} // namespace receiver
