# USB Proxy UDP — Raspberry Pi mouse proxy and replay

## Windows YOLO11 receiver

The [receiver application](receiver/README.md) receives configurable raw UDP frames,
runs YOLO11 using CUDA/TensorRT, and sends relative mouse corrections through this
Pi proxy. It includes a Dear ImGui/DirectX 11 GUI, headless mode, saved profiles,
a sample YOLO11n ONNX model, a synthetic sender, and a loopback Pi simulator.

- [Build and setup](receiver/README.md)
- [Frame and telemetry protocols](receiver/docs/PROTOCOL.md)
- [Validation status and remaining hardware checks](receiver/docs/VALIDATION.md)

The production receiver targets Windows 11, RTX 3060 Ti, CUDA 12.9, and TensorRT
10.13. Builds without TensorRT support explicit loopback simulation only.
This proxy now supports UPS1/UPT1 physical-button telemetry while preserving
existing UPX1 and ASCII commands. Real target-PC screen capture remains a separate
implementation; the included sender generates/replays test frames.


Forward a USB mouse through a Raspberry Pi 4, control it over Ethernet, or record its USB enumeration and emulate its HID mouse interface without the physical mouse attached.

The target PC communicates with a USB HID device through the Pi's USB device port. A separate controller sends movement and button commands to the Pi over UDP. The Windows controller implementation is in `receiver/`; real desktop capture remains outside this first delivery.

## Operating modes

| Mode | Executable / option | Physical mouse required? |
|---|---|---|
| Live proxy | `usb-proxy --enable_injection` | Yes; physical input and UDP input are combined |
| Enumeration recording | `usb-proxy --record_usb=mouse.jsonl` | Yes, while recording |
| Standalone HID replay | `usb-replay mouse.jsonl UDC_NAME UDC_DRIVER` | No, once a supported capture exists |

The live proxy forwards the mouse's descriptors and control traffic. Standalone replay serves captured descriptors and generates new mouse reports from UDP commands. It responds to the current host's enumeration requests; it does not play back an old sequence of movements.

**Start here:** [Build](#build-on-the-pi) · [Live proxy](#quick-start-live-mouse-plus-udp) · [Record and replay](#record-enumeration-then-replay-without-the-mouse) · [Python control](#send-mouse-commands-from-python) · [Troubleshooting](#troubleshooting)

## Hardware layout

```text
Physical mouse / USB receiver ──USB──> Pi 4 USB host port
                                           │
Controller PC ──Ethernet / UDP──────────────>│
                                           │
Target PC <────USB data cable────── Pi 4 USB-C device port
```

The controller and target may be the same PC. The physical mouse is optional during standalone replay. Use a USB data cable and provide adequate power for the Pi.

## Build on the Pi

The Pi needs `dwc2` device mode and a kernel with Raw Gadget support. Enable the `dwc2` overlay in your Pi's boot configuration, reboot, and load the `raw_gadget` module. If your kernel does not provide it, follow the [Raw Gadget build instructions](https://github.com/xairy/raw-gadget) for your installed kernel.

```sh
sudo apt-get install git build-essential libusb-1.0-0-dev libjsoncpp-dev python3
git clone https://github.com/tyoueenn2/usb-proxy-udp.git
cd usb-proxy-udp
make -j4
make test

# Verify the gadget interface and find your mouse / receiver IDs:
ls /dev/raw-gadget
ls /sys/class/udc/
lsusb
```

`make` builds **both `usb-proxy` and `usb-replay`**. The commands below use `fe980000.usb` as the example UDC name and driver; check these against your Pi. The example VID/PID `046d:c539` is not a universal G Pro identifier—use the IDs reported for your mouse or receiver.

## Quick start: live mouse plus UDP

Replace `192.168.1.10` with the controller PC's IPv4 address:

```sh
sudo env USB_PROXY_PEER=192.168.1.10 ./usb-proxy \
  --device=fe980000.usb --driver=fe980000.usb \
  --vendor_id=046d --product_id=c539 \
  --enable_injection --debug_level=0
```

Let the target PC finish enumeration, then move the physical mouse once. The proxy learns a supported report layout and becomes ready for UDP input. Physical button holds remain active when movement is injected; releasing an injected button does not release a physical hold.

In **live mode**, `--enable_injection` starts UDP and loads the existing `injection.json` rules. Run from the checkout or pass `--injection_file` explicitly. The supplied rules are disabled by default. Without this option, live USB forwarding and optional recording still work, but UDP control is disabled.

## Record enumeration, then replay without the mouse

### 1. Record the real mouse

```sh
sudo ./usb-proxy \
  --device=fe980000.usb --driver=fe980000.usb \
  --vendor_id=046d --product_id=c539 \
  --record_usb=mouse.jsonl
```

Wait for enumeration to finish, move the mouse once, then stop with Ctrl+C. Use a new capture filename; existing files are not overwritten. Recording and UDP injection can also be enabled together.

The capture stores timestamped USB events, control SETUP requests, ACK/STALL results, descriptor response bytes, and initial supported mouse reports. Identity strings and HID report descriptors are captured when the host requests them. This is not a continuous movement recording. The older `--descriptor_file` JSON summary cannot be used as a replay profile.

### 2. Check the capture

```sh
./usb-replay mouse.jsonl --check
```

This validates the capture without opening the gadget or starting UDP. Missing or truncated descriptors must be captured again.

### 3. Start standalone replay

Stop the live proxy first. You can now disconnect the physical mouse:

```sh
sudo env USB_PROXY_PEER=192.168.1.10 \
  ./usb-replay mouse.jsonl fe980000.usb fe980000.usb
```

**Replay starts UDP automatically.** Once the host configures the device, its mouse state starts with released buttons and zero movement. No physical mouse movement is needed to initialize replay.

See [ENUMERATION.md](ENUMERATION.md) for the capture format, supported control requests, and replay restrictions.

## Send mouse commands from Python

Copy [client.py](client.py) to the controller PC. It uses the Python standard library:

```python
from client import MouseProxy

with MouseProxy('192.168.1.20') as mouse:  # Pi Ethernet IPv4 address
    print(mouse.state())
    mouse.move(12, -6)
    mouse.button(1, True)
    mouse.move(8, 0)
    mouse.button(1, False)
```

Movement values are **relative HID counts**, not absolute screen coordinates or guaranteed pixels. Calibrate sensitivity and acceleration in the controller. A target's offset in a 320×320 image is not automatically the correct mouse movement.

UDP uses port **12345**. `USB_PROXY_BIND` optionally selects the Pi's local IPv4 address; `USB_PROXY_PEER` restricts accepted traffic to a controller IPv4 address. Keep one persistent socket: ownership is tied to the source IP and port. This is an unauthenticated LAN protocol.

While holding injected buttons, send a snapshot at least every 100 ms; `mouse.move()` with no arguments sends zero movement. The client has no background heartbeat. After 250 ms without an accepted command, the proxy queues a release of injected buttons. Delivery still depends on the USB host accepting reports.

## Commands and latency behavior

- **Binary `UPX1`:** 16-byte packets containing a sequence number, X/Y movement, wheel, pan, and a complete injected button mask.
- **ASCII:** movement, wheel, pan, button down/up, timed click, release, and state queries.
- **MAKCU-style subset:** spellings such as `km.move(10,-5)` and `km.left(1)`. This is not full MAKCU API or V2 binary compatibility.
- **Descriptor-driven encoding:** report IDs, packed 1–16-bit axes, and up to eight declared mouse buttons. Commands split automatically to fit the native movement range.
- **Queue behavior:** immediate wakeups, bounded queues, direct interrupt reads, and no per-report logging by default.
- **Fresh corrections:** consecutive unsent binary corrections with the same button state can replace each other. Physical reports and button/wheel events act as ordering barriers.
- **Expiry:** binary motion older than 25 ms since enqueue on the Pi is zeroed before USB submission; its button snapshot is retained. This does not measure network transit time or cancel a USB request already submitted.

For packet layouts, command syntax, replies, limits, and migration from the original raw-hex UDP injector, see [USAGE.md](USAGE.md).

## Compatibility and limitations

| Area | Current scope |
|---|---|
| Live mouse translation | Standard relative HID mouse layouts; boot mouse protocol supported |
| Logitech G Pro | Descriptor-driven, with no assumed packet offsets; hardware validation still required |
| Replay configuration | One HID-only configuration, alternate setting zero, interrupt endpoints |
| Replay mouse interface | One interrupt IN endpoint per supported mouse interface |
| HID state in replay | Configuration/reset handling, input `GET_REPORT`, idle reports, and mouse boot/report protocol switching |
| Proprietary behavior | Logitech HID++, feature reports, firmware operations, and vendor-specific effects are not simulated |
| Additional replay interfaces | HID interfaces remain enumerated; keyboard input and proprietary output effects are not generated |

Unsupported replay requests stall. Use the live proxy when the host needs the physical device's proprietary behavior. Replay presents a captured HID mouse identity and layout, but is not guaranteed to be indistinguishable from the original hardware. Host recognition must be verified on the actual Pi and target PC.

Actual latency depends on Ethernet, OS scheduling, USB host polling, and the Pi controller. No measured end-to-end latency is claimed, and advertised polling intervals are not modified. Low-speed physical mice are presented at full speed because dwc2 does not support low-speed gadget operation.

## Troubleshooting

| Symptom | What to check |
|---|---|
| `/dev/raw-gadget` is missing | Confirm Raw Gadget is built for the running kernel and its module is loaded. |
| No UDC is listed | Check the Pi's device-mode configuration before starting either executable. |
| UDP returns `not_ready` in live mode | Let enumeration finish, then move the physical mouse. Its descriptor and report must match a supported relative mouse layout. |
| UDP returns `busy` | Another source IP/port owns the controller lease. Stop that sender and allow the 250 ms lease to expire. |
| An injected hold releases by itself | Keep sending snapshots while holding buttons; the client does not send background heartbeats. |
| Replay validation rejects the capture | Read the error, then capture any missing descriptors again. Unsupported interface types or configurations require live mode. |
| Replay reports a bind/device error | Stop the live proxy or previous replay process; they cannot share the same UDC and UDP port. |
| Large moves or wheel commands are rejected | Check the native report limits and whether the selected mouse advertises that axis. Queue capacity can also reject a command. |

For live USB troubleshooting, `--debug_level=3` prints outgoing report bytes. Return to `--debug_level=0` for latency measurements.

## Validation

`make test` includes HID encoding and malformed-descriptor tests, a hardware-free UDP/state integration test, capture/replay roundtrips and invalid-capture checks, and Python sender tests. The [GitHub Actions workflow](.github/workflows/build.yml) builds on Linux and includes HID parser sanitizer checks.

During development, the portable HID, capture/replay, and Python tests passed; the proxy sources compiled for ARM64, and the standalone replay executable linked for ARM64. These checks do not replace Pi hardware tests. Verify enumeration, button behavior, disconnect/reconnect, suspend/resume, and latency on your setup.

## Documentation and credits

- [UDP protocol and Python client](USAGE.md)
- [Enumeration recording and replay](ENUMERATION.md)
- Upstream projects: [AristoChen/usb-proxy](https://github.com/AristoChen/usb-proxy) and [xairy/raw-gadget](https://github.com/xairy/raw-gadget)
- [Original license](LICENSE) retained.
