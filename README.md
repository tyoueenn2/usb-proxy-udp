# USB proxy with low-latency UDP mouse control

A Raspberry Pi 4 forwards a physical USB mouse to a computer through raw-gadget and accepts mouse commands over Ethernet. Detection/CUDA stays on the PC; only movement and button commands reach the Pi. See [USAGE.md](USAGE.md) for the complete protocol and Python client.

For enumeration recording and standalone mouse simulation without a physical mouse connected, see [ENUMERATION.md](ENUMERATION.md). `make` now builds both `usb-proxy` and `usb-replay`.

## Hardware and build

Connect the mouse or its USB receiver to a Pi USB host port, the Pi USB-C device port to the controlled PC, and the Pi Ethernet port to the LAN. The controlled PC can also run inference. The USB connection must support data, and the Pi needs adequate power.

This requires a Linux kernel with dwc2 device mode and raw-gadget support. Enable the `dwc2` overlay in your Pi boot configuration, reboot, load your kernel's `raw_gadget` module, and verify `/dev/raw-gadget` and `/sys/class/udc/`. If the module is absent, build it for your installed kernel following [raw-gadget](https://github.com/xairy/raw-gadget).

```sh
sudo apt-get install build-essential libusb-1.0-0-dev libjsoncpp-dev python3
make -j4
make test
lsusb
ls /sys/class/udc/
```

Use your mouse/receiver's vendor and product IDs from `lsusb`. Example (replace the IP and USB IDs):

```sh
sudo env USB_PROXY_PEER=192.168.1.10 ./usb-proxy \
  --device=fe980000.usb --driver=fe980000.usb \
  --vendor_id=046d --product_id=c539 --enable_injection --debug_level=0
```

The UDC name must match your Pi. UDP is enabled only by `--enable_injection`; the same option also loads the existing `injection.json` rules. Run from the checkout or specify `--injection_file` explicitly. Default rules are disabled.

## Changes in this version

- HID descriptor-driven relative mouse reports, including report IDs and packed axes; no fixed Logitech byte offsets.
- Physical and injected button state merged immediately before forwarding.
- Immediate queue notification, bounded queues, and no per-report console output by default.
- Direct interrupt reads into the report buffer, avoiding an allocation per mouse read.
- A 16-byte binary command path with sequence checking, replaceable aiming corrections, and stale-motion expiry.
- Automatic splitting to the mouse's native axis range, plus wheel, horizontal pan, buttons, timed click, release, and status commands.
- One UDP controller at a time; injected buttons release after 250 ms without accepted commands.

Translation supports standard relative HID mouse layouts with 1–16-bit axes and up to eight buttons. Boot mouse protocol is also handled. Proprietary formats, absolute pointing devices, and unusual reports combining multiple independent input collections require further work. The first observed supported mouse report selects the injection endpoint/report ID; other traffic is forwarded. A G Pro is not assumed to have a particular report layout and still needs hardware validation.

## Validation and limits

`make test` runs HID parser/encoding tests, malformed-descriptor cases, a hardware-free UDP/state integration test, and Python sender tests. The GitHub workflow builds on Linux and adds parser sanitizer checks when run.

USB host polling, the Pi controller, OS scheduling, and Ethernet still determine end-to-end latency. The implementation does not alter advertised polling intervals and makes no measured latency claim. The proxy emits relative HID counts, not absolute screen positions or guaranteed pixels. Cursor acceleration and sensitivity must be accounted for by the controller.

Upstream: [AristoChen/usb-proxy](https://github.com/AristoChen/usb-proxy), [xairy/raw-gadget](https://github.com/xairy/raw-gadget). Original license retained.
