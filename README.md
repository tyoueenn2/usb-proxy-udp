# USB Proxy UDP

A low-latency Raspberry Pi 4 USB mouse proxy with UDP movement, persistent synthetic holds, reliable Pi-scheduled clicks, and descriptor-based HID report translation.

The Pi appears to the target PC as a USB HID mouse. It can merge a real mouse with UDP commands, record a device's USB enumeration, or replay a captured HID identity without the physical mouse attached. Video transport, object detection, and CUDA processing run on the controller and are outside this repository.

```text
Physical mouse ──USB──> Raspberry Pi 4 ──USB gadget──> Target PC
Controller PC ──UDP───────────┘
```

## Features

- Live physical mouse passthrough with descriptor-driven report translation.
- Independent physical, persistent injected, and scheduled-click button state:
  `final_buttons = physical_buttons | persistent_injected_buttons | scheduled_click_buttons`.
- Physical holds cannot be cleared by synthetic releases.
- Ordered physical reports and button edges, with coalescing limited to compatible movement corrections.
- Idempotent scheduled clicks timed on the Pi and completed through the USB writer.
- Bounded queues, explicit overload responses, USB backpressure handling, and a 250 ms controller watchdog.
- Enumeration capture and standalone HID mouse replay.
- Physical-only direction and motion telemetry.

## Modes

| Mode | Command | Physical mouse |
|---|---|---|
| Live proxy | `usb-proxy --enable_injection` | Required |
| Enumeration recording | `usb-proxy --record_usb=mouse.jsonl` | Required while recording |
| Standalone replay | `usb-replay mouse.jsonl UDC_NAME UDC_DRIVER` | Not required after capture |

## Protocols

UDP listens on port **12345**.

| Protocol | Purpose |
|---|---|
| `UPX1` | Stable 16-byte movement packet with wheel, pan, and the complete persistent injected-button mask |
| `UPC1` / `UPA1` | Versioned, idempotent click scheduling and acknowledgments with accepted and completed counts |
| `UPS3` / `UPT3` | Primary request/telemetry pair, including queue, writer, scheduler, and physical-only motion state |
| `UPS2` / `UPT2` | Canonical compatibility telemetry format |
| ASCII | Movement, buttons, clicks, release-all, and state commands, including a MAKCU-style subset |

Exact layouts, status codes, limits, retry behavior, and Python examples are in [USAGE.md](USAGE.md). Existing `UPX1` packets and ASCII commands remain supported.

## Quick start

The Pi requires `dwc2` device mode, Raw Gadget support, a USB data connection to the target, and the exact UDC and physical mouse VID/PID values.

```sh
sudo apt-get install git build-essential libusb-1.0-0-dev libjsoncpp-dev python3
make -j4
make test

sudo env USB_PROXY_PEER=CONTROLLER_IP ./usb-proxy \
  --device=UDC_NAME --driver=UDC_DRIVER \
  --vendor_id=VID --product_id=PID \
  --enable_injection --debug_level=0
```

Follow [DEPLOYMENT.md](DEPLOYMENT.md) for Pi configuration, device discovery, startup, shutdown, validation, and troubleshooting.

## Scope

- Live translation supports standard relative HID mouse layouts, boot protocol, packed 1–16-bit axes, and up to eight declared buttons.
- Replay supports one HID-only configuration, alternate setting zero, and one interrupt IN endpoint per supported mouse interface.
- Replay does not emulate Logitech HID++, feature reports, firmware operations, keyboard input, or proprietary output effects. Use live mode when the host requires these behaviors.
- Simulated 10, 25, and 50 clicks-per-second tests verify scheduler logic only. A supported physical rate must be measured on a Raspberry Pi 4 with the actual USB hardware and target PC.

## Documentation

- [Deployment and operation](DEPLOYMENT.md)
- [UDP protocol and Python client](USAGE.md)
- [Enumeration recording and replay](ENUMERATION.md)
- [GitHub Actions build and tests](.github/workflows/build.yml)

Based on [AristoChen/usb-proxy](https://github.com/AristoChen/usb-proxy) and [xairy/raw-gadget](https://github.com/xairy/raw-gadget). The [original license](LICENSE) is retained.
