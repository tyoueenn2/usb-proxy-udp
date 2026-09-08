#pragma once
// Kept independent of host headers so the exact kernel can also be checked by NVRTC.
namespace receiver {
extern "C" __global__ void preprocess_kernel(const unsigned char* raw, float* tensor, int w, int h,
                                             int channels, int size, int rw, int rh, int left, int top) {
    int x = int(blockIdx.x * blockDim.x + threadIdx.x), y = int(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= size || y >= size)
        return;
    for (int c = 0; c < 3; ++c) {
        float value = 114.f;
        if (x >= left && x < left + rw && y >= top && y < top + rh) {
            float sx = fminf(fmaxf((x - left + .5f) * w / rw - .5f, 0.f), float(w - 1));
            float sy = fminf(fmaxf((y - top + .5f) * h / rh - .5f, 0.f), float(h - 1));
            int x0 = int(sx), y0 = int(sy), x1 = x0 + 1 < w ? x0 + 1 : w - 1,
                y1 = y0 + 1 < h ? y0 + 1 : h - 1, channel = channels == 4 ? 2 - c : c;
            float fx = sx - x0, fy = sy - y0;
            float a = raw[(y0 * w + x0) * channels + channel], b = raw[(y0 * w + x1) * channels + channel],
                  d = raw[(y1 * w + x0) * channels + channel], e = raw[(y1 * w + x1) * channels + channel];
            value = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
        }
        tensor[c * size * size + y * size + x] = value / 255.f;
    }
}
} // namespace receiver
