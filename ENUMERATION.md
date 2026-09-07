# Record enumeration and replay a USB mouse

The live proxy forwards the physical mouse's USB descriptors and control requests. The host communicates with a USB HID interface over the Pi's USB device port. UDP is a separate input to the Pi; it is not a mouse driver installed on the host.

`usb-replay` can also enumerate from a recording with the physical mouse disconnected. It serves captured descriptor bytes in response to the host's requests and generates live HID mouse reports from UDP commands. It does not replay the timing/order of an old enumeration transcript: the current host determines the request order.

## 1. Record with the real mouse connected

Build both executables with `make -j4`. Connect the mouse/receiver to a Pi host port and the Pi's USB device port to the target PC. Replace the example USB IDs with those from `lsusb`:

```sh
sudo ./usb-proxy --device=fe980000.usb --driver=fe980000.usb \
  --vendor_id=046d --product_id=c539 --record_usb=mouse.jsonl
```

Allow the host to finish enumeration, then move the mouse once. Stop with Ctrl+C. The recording option requires a new filename and refuses to overwrite an existing recording. If needed, reconnect the Pi device cable while recording to capture enumeration again; keep the mouse in the same operating mode. `--enable_injection` is optional during recording.

Each JSONL record contains a monotonic timestamp in microseconds and one of:

- `format`: capture schema version.
- `speed`: host-facing gadget speed.
- `event`: raw-gadget events, including control SETUP packets and reset/disconnect events.
- `control`: host-facing ACK/STALL and exact control data bytes as hexadecimal. OUT bytes are what the host sent; an ACK records the host-facing result, not a guarantee of subsequent vendor-device effects.
- `mouse_report`: the first supported report observed for a mouse endpoint after layout discovery, with its interface and endpoint address.

The descriptor responses include device identity, configurations, interfaces, endpoints, strings/language IDs, HID descriptors and HID report descriptors **when requested by the host**. The earlier `--descriptor_file` summary remains available; it is not a replay profile. Physical movement is not continuously logged. Recordings include any serial-number strings requested by the host and are ignored by Git by default.

Recording adds disk writes during enumeration and initial report discovery. Omit `--record_usb` for normal low-latency operation.

## 2. Validate before using standalone replay

```sh
./usb-replay mouse.jsonl --check
```

This does not open the USB gadget or start UDP. It checks complete descriptors, endpoint limits and supported mouse report layouts. Re-record if a required descriptor was never requested or a file was truncated. A host may cache descriptors, so reconnecting to a fresh port or host can be useful when gathering a complete capture.

## 3. Replay without the physical mouse

Stop the live proxy, disconnect the physical mouse if desired, and run:

```sh
sudo env USB_PROXY_PEER=192.168.1.10 \
  ./usb-replay mouse.jsonl fe980000.usb fe980000.usb
```

Use your actual UDC name/driver. Replay starts the same UDP server on port 12345; the [Python client and command protocol](USAGE.md) work unchanged. Once the host selects the configuration, the replay mouse is initialized with neutral buttons and zero movement, so no physical movement is needed to make it ready. Captured relative motion and held buttons are cleared rather than replayed.

The host should enumerate a HID mouse with the captured identity and report format. Check the HID/mouse device and hardware IDs on the host, then send a small move using `client.py`. This recognition and movement still require verification on your Pi and target PC.

## Supported simulation

- Exact captured descriptor payloads, with short replies limited to the new host's requested length; strings remain keyed by language ID.
- One configuration containing HID interfaces, alternate setting zero, and interrupt endpoints.
- One interrupt IN endpoint per supported mouse interface, using the existing descriptor-driven mouse encoder.
- Configuration selection/unconfiguration, interface queries, device/interface status, and reset/disconnect handling.
- Mouse input `GET_REPORT`, `SET_IDLE`/`GET_IDLE`, and report/boot mouse protocol switching. Idle repeats and `GET_REPORT` contain current buttons with zero relative deltas.
- Additional HID interfaces remain enumerated; interrupt OUT traffic is consumed. No keyboard input or vendor-specific output effects are generated.

Unsupported requests stall. In particular, proprietary feature reports, firmware update protocols, Logitech configuration/HID++ behavior, non-HID interfaces, multiple configurations, and nonzero alternate settings are not simulated. Remote wake enable state is accepted when advertised, but replay does not initiate a remote wake. Endpoint halt feature requests are not implemented. Use the live proxy when the host needs proprietary device behavior.

The Pi's controller, electrical link, scheduling, and responses to unsupported requests can differ from the original device. This is functional HID mouse emulation, not a guarantee that the Pi is indistinguishable from the original hardware. Low-speed physical mice are presented at full speed because dwc2 does not provide low-speed gadget operation.

## Tests and references

`make test` includes capture-to-profile roundtrips, partial descriptor assembly, language-indexed strings, stalls, and malformed capture rejection. `usb-replay --check` validates an actual capture without hardware access. Hardware enumeration, suspend/resume, and motion tests must run on the Pi and target PC.

The implementation uses [Linux Raw Gadget](https://docs.kernel.org/usb/raw-gadget.html) and the standard USB HID model described by [USB-IF HID specifications](https://www.usb.org/hid).
