# UDP Vision Receiver

C++20 / Windows 11 receiver for raw UDP frames, YOLO11 TensorRT inference, and relative mouse commands to a Raspberry Pi USB proxy. Includes a Dear ImGui/DirectX 11 GUI, a headless runner, a synthetic sender, and a loopback-only Pi simulator.

**Delivery status:** the default Windows builds and CI artifacts are **test builds without TensorRT**. They support the GUI, network pipeline, and explicit simulation mode. The production TensorRT backend is implemented in source. Build it on the RTX 3060 Ti PC using the instructions below. See [validation results](docs/VALIDATION.md) for exactly what has been tested.

The exported sample `models/yolo11n.onnx` and its manifest are included in this directory. This is the standard COCO detection model, not a model trained for a particular application. Real desktop capture on the target PC is intentionally outside this first delivery.

## Quick hardware-free test

Install Python 3.12 or newer. From the receiver directory, open three terminals:

```powershell
py tools/mock_pi.py --seconds 60
```

```powershell
py tools/test_sender.py --width 320 --height 320 --fps 120 --seconds 60
```

```powershell
.\receiver_headless.exe --simulate --arm --seconds 30 --metrics simulation.csv
```

For a source build, executables are under `build/tests/Release/` instead. The mock Pi reports a held right button by default and records received commands in `mock_commands.json`. It never accesses USB or moves the local mouse. Simulation refuses any sender/Pi IP other than `127.0.0.1`.

For the GUI, run `receiver.exe`, check **Loopback simulation**, then **Start receiver**. Enable **Detection preview** to inspect frames. Output stays disarmed until **Arm mouse output** is selected. **Delete** disarms immediately when processed by the Pi worker. A synthetic test detection appears in simulation; it is not YOLO inference.

## Production build on the RTX 3060 Ti PC

Use Windows 11 x64, Visual Studio 2022 with the Desktop development with C++ workload and a Windows SDK, CMake 3.24+, Git, **CUDA Toolkit 12.9**, and the **TensorRT 10.13 Windows x64 CUDA 12 SDK**. Use an NVIDIA driver supported by that CUDA version. This backend deliberately targets TensorRT **10.x**; TensorRT 11's precision/export API is different and is rejected at compile time.

Official SDK references: [TensorRT Windows installation](https://docs.nvidia.com/deeplearning/tensorrt/latest/installing-tensorrt/install-zip.html), [TensorRT compatibility matrix](https://docs.nvidia.com/deeplearning/tensorrt/latest/getting-started/support-matrix.html). Select the 10.13 release, rather than substituting the latest major version.

In an x64 Developer PowerShell, from this directory:

```powershell
$env:TENSORRT_ROOT = 'C:\SDKs\TensorRT-10.13.0.35'
$env:PATH = "$env:TENSORRT_ROOT\lib;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin;$env:PATH"
cmake --preset windows-cuda
cmake --build --preset windows-cuda
ctest --preset windows-cuda
.\build\cuda\Release\receiver.exe
```

Set `TENSORRT_ROOT` to your actual extracted SDK directory containing `include` and `lib`. CUDA kernels target SM 8.6, the RTX 3060 Ti architecture. CMake fetches pinned Dear ImGui 1.92.1 and nlohmann/json 3.12.0 sources; subsequent builds can run offline.

For a build without GPU dependencies:

```powershell
cmake --preset windows-tests
cmake --build --preset windows-tests
ctest --preset windows-tests
py tests/integration_test.py build/tests/Release/receiver_headless.exe
```

The headless networking/control code also has POSIX socket support for tests; the production GPU backend and GUI are Windows targets. Linux test configure: `cmake -S . -B build/tests -DRECEIVER_GUI=OFF -DRECEIVER_TENSORRT=OFF`.

## Connect the real system

1. Apply the supplied Pi telemetry patch to proxy commit `c3c09153feabe4be2eb3a711fe3c3e957f6eb794`, or use the included updated proxy source. Build/run it with `--enable_injection`. Follow its existing USB setup instructions. Move the physical mouse once after enumeration.
2. Set the receiver's **Sender IPv4**, **Pi IPv4**, and listen port. If configured on the Pi, `USB_PROXY_PEER` must be the processing PC's IPv4 address. The receiver uses one persistent source port for subscriptions and movement.
3. Allow inbound UDP on the configured frame port in the processing PC's firewall. The frame path should use wired 2.5 GbE or faster as planned. The Pi's control link only carries small commands/status messages.
4. Load `profiles/yolo11n.json`. Paths in profiles are relative to the process working directory; launch from this directory or use absolute model paths. Set model input to 160 or 320 as desired. Load your application-specific exported model when available.
5. Start the receiver. The first load builds and warms a TensorRT engine on this GPU; it can take minutes. The GUI remains responsive. Stop runs asynchronously, but cannot interrupt an in-progress TensorRT builder call.
6. Send frames conforming to [the protocol](docs/PROTOCOL.md). The provided test sender can run on the target PC and send a raw image file for end-to-end testing, but does not capture its screen.
7. Arm the GUI and hold the configured physical mouse button (right button by default). Output requires fresh telemetry, clock synchronization, fresh frames, and a current detection inside the FOV.

The application sends **relative HID counts**, not absolute cursor coordinates. Tune X/Y gain against the target application's sensitivity. The default reference is the capture center, so a centered screen crop is the natural sender configuration. A different reference can be set in capture pixels. Input/model changes require restart; detection/control changes apply between frames and invalidate pending corrections.

Old proxy versions without UPT1 telemetry will show “extension required” and cannot activate mouse output. UPX1 movement framing remains unchanged. Existing `+state`, ASCII, and MAKCU-inspired proxy commands remain usable by their existing clients, subject to the proxy's ownership rules.

## Model export and validation

The sample is already exported. For other YOLO11 detection weights, create an isolated Python environment:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools/requirements-export.txt
.\.venv\Scripts\python.exe tools/export_model.py --weights path/to/custom.pt --size 320
```

The exporter writes an ONNX file and `<model>.onnx.json` containing class names, the raw detection contract, and a SHA-256 digest. Default exports have dynamic spatial dimensions, allowing separate optimized engines at 160 and 320. `--fixed` exports require the GUI input size to match exactly. Batch size is always one; model input dimensions must be multiples of 32, from 32 to 1024. Capture sizes need not be multiples of 32.

The runtime validates float32 NCHW input and raw `[1, 4 + classes, candidates]` output. Internal TensorRT layers use FP16 where supported; I/O remains float32. Segmentation, pose, classification, embedded-NMS, external-data ONNX, and arbitrary prebuilt engines are not supported. Only locally generated cache engines are loaded. Engine keys include the model hash, input size, GPU model, runtime/driver versions, and precision mode. Cache files live under `cache/`; deleting a cache file forces a rebuild.

For a GPU/reference comparison on a tightly packed RGB24 file:

```powershell
.\.venv\Scripts\python.exe tools/validate_gpu.py --verify-exe build/cuda/Release/receiver_verify.exe --profile profiles/yolo11n.json --raw sample.rgb --width 320 --height 320
```

This compares preprocessing plus inference against ONNX Runtime CPU FP32. Review representative images as well as numeric differences. Reduced input resolution and FP16 may affect detection accuracy. ONNX Runtime CUDA is a possible future backend; it is not a hidden CPU fallback in this application.

## Performance and troubleshooting

- The GUI runs at up to 60 Hz; the preview is sampled at 30 Hz. Disable preview when benchmarking. CPU conversion/upload for preview only occurs when a new preview frame is displayed.
- The receiver preallocates a 28 MiB pool of seven maximum-size frames: three incomplete frames plus slots held by inference, pending results, and preview. Pool exhaustion drops work safely. CUDA staging/device buffers are allocated once per model load. Postprocessing uses bounded candidate lists.
- Metrics include reassembly, host/device copies plus preprocessing, inference, postprocessing, command submission, receiver-to-submission, and a conservative capture-age bound. GPU stages use CUDA events. Reported percentiles cover the most recent 2,048 samples; sample counts are lifetime totals. The GPU free-memory figure is a device-wide snapshot after initialization, not exclusive application memory.
- No UDP ACK is available for successful UPX1 commands. “Submitted” means the local socket accepted a datagram, not that the target received USB movement. Already submitted USB reports cannot be recalled.
- A dropped frame, stopped sender, or lost telemetry stops future corrections. Physical mouse holds remain physical; v1 never injects clicks or button holds. On a Pi error, restart the receiver after resolving it. GPU errors require stopping/restarting the pipeline; there is no automatic CPU fallback.
- Profiles never store an armed state. The headless `--arm` option is an explicit per-run choice. The GUI always starts disarmed.

See [benchmark procedure](docs/BENCHMARK.md), [validation record](docs/VALIDATION.md), and [third-party notices](docs/THIRD_PARTY.md).
