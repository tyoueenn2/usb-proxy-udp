# Validation record — 2026-09-08

## Implemented and locally verified

Development host: Windows, Intel Core i7-1165G7, NVIDIA GeForce MX450, NVIDIA driver 592.82. This is not the planned RTX 3060 Ti deployment system.

- Native Windows test build: C++20 receiver core, headless executable, and Dear ImGui/DirectX 11 GUI compile and link with LLVM-MinGW 20260826 (Clang 23.1.0), CMake 4.4.3, Dear ImGui 1.92.1, and nlohmann/json 3.12.0.
- C++ core tests pass: binary framing, malformed dimensions/lengths, duplicates and conflicting fragments, reversed fragment order, assembly expiry/eviction, sender session changes, held-buffer lifetime/pool exhaustion, 10,000 malformed fuzz datagrams, clock offset bounds, telemetry token/sequence checks, wraparound, NMS/class filtering, letterbox reversal, selection/persistence, movement clamping, fractional accumulation, smoothing, and profile validation.
- Actual C++ receiver loopback integration passes using Python UDP peers: physical activation/release, capture stalls, telemetry loss, old capture timestamps, positive/negative sender clock offsets, sender restart, RGB/BGRA, Pi-not-ready handling, and default-disarmed startup. No real USB/mouse input is generated during these tests.
- GUI smoke test passes using the application's own hidden DirectX framebuffer: live preview, clock synchronization, telemetry, rendering, and disarmed startup. A rendered image is included separately in the delivery. This is a render/startup check, not exhaustive manual interaction testing.
- Pi telemetry wire codec compiles/runs on Windows. The proxy's Linux integration test has been extended to exercise subscriptions, physical-button updates, and expiry.
- Sample `yolo11n.onnx` was exported from official `yolo11n.pt` with Ultralytics 8.3.199 / PyTorch 2.8.0 CPU, ONNX opset 17, dynamic dimensions, raw detection output, and a SHA-256 manifest. ONNX checker passes.
- ONNX Runtime 1.22.1 CPU executes the actual sample at input sizes 160 and 320. C++ decoding matches independent torchvision NMS and coordinate mapping on 13 detections across a real-image fixture, two capture/input combinations, and class filtering.
- TensorRT host code passes C++ syntax checking against official TensorRT 10.13 and CUDA 12.9 headers. This caught and resolved ownership of TensorRT optimization profiles. This check does not link the TensorRT backend.
- The production CUDA preprocessing kernel compiles with NVIDIA NVRTC 12.9 for compute_86 and compute_75. It executes on the local MX450 through the CUDA driver API and matches the CPU reference across RGB/BGRA, square/non-square frames, odd dimensions, and a 1×1 edge case. Maximum absolute tensor error observed: approximately **1.2e-7**.

## Still required on deployment hardware

- A complete MSVC + CUDA 12.9 + TensorRT 10.13 production build, including the NVCC host launch wrapper and SDK linking. MSVC and a full CUDA toolkit are not installed on this development host. The portable test executables do not contain TensorRT.
- TensorRT engine build, warmup, FP16 inference, numerical comparison, GPU memory measurement, and latency measurements on the RTX 3060 Ti.
- Linux `make test` for the modified Pi proxy. WSL/Linux is not installed on this development host. The existing raw-gadget/libusb implementation cannot be executed as a Windows application.
- Physical mouse/USB enumeration, Pi report merging and telemetry behavior under actual USB backpressure, button release, and endpoint disconnect/reconnect.
- LAN performance at 160/320 captures and 120/240 FPS, preview overhead, and true capture-to-USB timing.

## Delivery boundaries

Real target-PC screen capture is not implemented; a documented protocol and synthetic/raw-file sender are included. No code has been deployed to the Pi and no system CUDA/Visual Studio installation has been performed. The Pi changes are based on commit `c3c09153feabe4be2eb3a711fe3c3e957f6eb794` and are included in this repository.

All zero-millisecond GPU entries in simulation CSV files mean the inference backend was bypassed. They are not performance claims. The lack of UPX1 success ACK means no software-only receiver test can establish USB delivery latency.
