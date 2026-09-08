# Reproducible performance checks

Do not use the included simulation timings as RTX 3060 Ti inference estimates. Simulation bypasses the model. No real capture-to-USB latency figure has been measured in this workspace.

## Functional checks before timing

1. Run `ctest --preset windows-tests` and `py tests/integration_test.py build/tests/Release/receiver_headless.exe`.
2. On the Pi, run `make test`. This exercises the real UDP server, telemetry, report splitting, button merging, sequencing, and watchdog logic without a USB device. Then test the actual connected mouse/USB endpoint.
3. Build the CUDA receiver on the deployment PC. Use `tools/validate_gpu.py` against representative RGB captures at both input sizes. Initial comparison thresholds are 2 model-input pixels and 0.05 class confidence on meaningful detections; inspect detection agreement as well.

## Matrix on deployment hardware

Run 160×160 and 320×320 captures at 120 and 240 FPS, with preview off and on. Use the same YOLO11n or application-specific model, confidence, FOV, gain, target content, and mouse settings for every comparison. Fixed-size model exports must match the selected inference size; dynamic exports can build separate engines for each size.

Record Windows version, CPU, GPU/driver, CUDA/TensorRT versions, model SHA-256, capture and model sizes, pixel format, NIC speed, frame rate, preview setting, and profile. Close competing GPU workloads. Disable proxy per-packet logging. Build/cache/warm up engines before timing. Allow at least five seconds of steady input, then collect 30 seconds. Percentiles use a rolling window of the most recent 2,048 samples; export the CSV and record its sample counts and duration.

Use the headless runner when preview is disabled:

```powershell
.\build\cuda\Release\receiver_headless.exe --profile profiles/yolo11n.json --arm --seconds 40 --metrics run-320-120.csv
```

The profile must contain the real sender and Pi addresses. Movement still requires the configured physical button. To measure inference alone, omit `--arm`; submission-related metrics will have zero samples. The sample sender can replay `--raw frame.rgb` and emulate loss/reordering; its Python scheduling/packetization is not a production screen-capture benchmark.

## What the metrics mean

- **Reassembly:** first received fragment to completed frame.
- **Copies/preprocess:** host staging copy, host-to-device transfer, fused CUDA preprocessing, and device-to-host detection output transfer. GPU portions use CUDA events.
- **Inference:** TensorRT GPU execution, excluding engine build and warmup.
- **Postprocess:** CPU decoding, filtering, and NMS. Selection/movement computation is included in receiver-to-submission, not this row.
- **Command submission:** the local UDP send call.
- **Receiver-to-submission:** first fragment receipt to command submission, including waiting, inference, selection, and output scheduling.
- **Capture-age upper bound:** estimated maximum age at submission under the documented timestamp/drift assumptions. It is not a directly measured one-way network latency.

Also record expired/evicted incomplete frames, invalid/duplicate packets, replaced pending frames, stale results, pool drops, and GPU free/total memory at initialization. The host frame pool is fixed at 28 MiB; CUDA staging, input/output, TensorRT workspace, and optional preview add memory on top.

For actual capture-to-USB or capture-to-visible-motion latency, instrument the capture sender and Pi or use an external high-speed camera/logic analyzer. UPX1 does not acknowledge USB delivery. A local timing result cannot establish that latency.

## Regression expectations

Memory must remain bounded under sustained overload. Frame loss must not produce commands from incomplete images. Frame/telemetry loss must stop output after the configured freshness cutoffs, within OS scheduling tolerance. Physical release must stop newly generated corrections; already submitted USB reports are not reversible. Preview should not create a queue of frames or block inference. Treat an observed performance regression as a reason to profile, not as evidence that a different language is inherently faster.
