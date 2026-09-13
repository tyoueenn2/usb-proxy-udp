# Deployment and operation

This guide covers Raspberry Pi 4 setup, live proxy operation, the controller connection, safe shutdown, validation, and troubleshooting. Packet layouts and commands are documented in [USAGE.md](USAGE.md); enumeration capture and replay are documented in [ENUMERATION.md](ENUMERATION.md).

## Hardware layout

```text
Physical mouse / USB receiver ──USB──> Pi 4 USB host port
                                           │
Controller PC ──Ethernet / UDP──────────────>│
                                           │
Target PC <────USB data cable────── Pi 4 USB-C device port
```

The controller and target may be the same PC. The physical mouse is optional during standalone replay. Use a USB data cable and provide adequate power for the Pi.

## Prepare the Pi

The Pi needs `dwc2` device mode and a kernel with Raw Gadget support. Enable the `dwc2` overlay in the Pi boot configuration, reboot, and load the `raw_gadget` module. If the running kernel does not provide it, follow the [Raw Gadget build instructions](https://github.com/xairy/raw-gadget).

Install the build dependencies, clone the repository, and run the test suite:

```sh
sudo apt-get install git build-essential libusb-1.0-0-dev libjsoncpp-dev python3
git clone https://github.com/tyoueenn2/usb-proxy-udp.git
cd usb-proxy-udp
make -j4
make test
```

Confirm Raw Gadget and discover the exact UDC, VID, PID, and descriptors:

```sh
ls /dev/raw-gadget
ls /sys/class/udc/
lsusb
sudo lsusb -v -d VID:PID
```

Use the values reported by the Pi. A Logitech receiver can enumerate with different IDs in wired, wireless, or receiver modes. Save the verbose descriptor output with the hardware test record.

## Start the live proxy

Connect the physical mouse or receiver to a Pi host port and the target PC to the Pi's USB-C device port. Replace every placeholder below:

```sh
sudo env USB_PROXY_PEER=CONTROLLER_IP ./usb-proxy \
  --device=UDC_NAME --driver=UDC_DRIVER \
  --vendor_id=VID --product_id=PID \
  --enable_injection --debug_level=0
```

Let the target finish enumeration, then move the physical mouse once. The proxy becomes ready for UDP commands after it observes a supported descriptor and matching physical report.

`--enable_injection` starts the UDP server and loads `injection.json`. Run from the repository checkout or pass `--injection_file` explicitly. The supplied injection rules are disabled by default. Without this option, live USB forwarding and optional recording continue, but UDP control is disabled.

The controller and target can be the same PC. Copy [client.py](client.py) to the controller and keep one `MouseProxy` instance open so the UDP source port and controller ownership remain stable:

```python
from client import MouseProxy

with MouseProxy("PI_ETHERNET_IP") as mouse:
    mouse.move(12, -6)
    result = mouse.schedule_clicks(
        button=1,
        count=25,
        press_ms=8,
        interval_ms=12,
        protocol_version=2,
        timeout=2.0,
    )
    print(result["accepted_clicks"], result["completed_clicks"])
```

Movement values are relative HID counts. They are not absolute coordinates or guaranteed pixels. Calibrate the controller for target sensitivity and acceleration.

## Controller ownership and safety

UDP uses port **12345**. `USB_PROXY_BIND` selects the Pi's local IPv4 address and defaults to `0.0.0.0`. `USB_PROXY_PEER` restricts accepted traffic to one controller IPv4 address. Ownership is tied to the source IP and port, so use one persistent socket. The protocol is unauthenticated and should be used on a trusted LAN.

While holding injected buttons, send a `UPX1` snapshot at least every 100 ms. A zero-movement snapshot is sufficient. Exact retries of an accepted click command also renew the lease.

After 250 ms without valid `UPX1` traffic, an accepted command, or an exact retry, the watchdog cancels synthetic work, invalidates queued synthetic reports, and queues a merged release while preserving physical holds. The same cleanup applies to controller disconnect, invalid layout generation, endpoint change, and shutdown.

Before a normal stop, send ReleaseAll and wait for its `completed` acknowledgment, then press Ctrl+C once. This confirms that the final merged release passed through the USB writer before endpoint teardown.

After changing the physical mouse, receiver mode, target cable, report protocol, or endpoint, allow enumeration to finish and move the physical mouse again. This supplies a report template for the new descriptor-defined layout. A pending binary ReleaseAll is retried when the writer becomes available.

## Record and replay

Use live recording to capture the real device's USB descriptors and control responses:

```sh
sudo ./usb-proxy \
  --device=UDC_NAME --driver=UDC_DRIVER \
  --vendor_id=VID --product_id=PID \
  --record_usb=mouse.jsonl
```

Wait for enumeration, move the mouse once, and stop with Ctrl+C. Existing capture files are not overwritten. Recording adds disk activity, so omit it during latency measurements.

Validate and replay the capture:

```sh
./usb-replay mouse.jsonl --check
sudo env USB_PROXY_PEER=CONTROLLER_IP \
  ./usb-replay mouse.jsonl UDC_NAME UDC_DRIVER
```

Stop the live proxy before replay because both processes use the same UDC and UDP port. Replay starts UDP automatically and initializes a neutral mouse report after the host selects the configuration. See [ENUMERATION.md](ENUMERATION.md) for capture contents, supported host requests, and replay restrictions.

## Validation

`make test` covers protocol vectors, malformed descriptors, capture/replay roundtrips, Python retries, bounded session lifecycle, deterministic mixer behavior, and a simulated USB writer. It tests independent button masks, physical report ordering, movement coalescing barriers, queue saturation, retries, watchdog cleanup, layout changes, write failures, shutdown, and simulated 10, 25, and 50 clicks per second.

Simulation establishes scheduler correctness. It does not prove that the destination USB stack observed every edge or establish a physical click-rate claim. Complete the [Raspberry Pi 4 hardware validation procedure](USAGE.md#raspberry-pi-4-hardware-validation) with the actual mouse, Pi, cable, target PC, and USB capture or host-side event evidence before publishing a supported rate.

Actual latency depends on Ethernet, OS scheduling, USB polling, and the Pi controller. The mixer uses the selected endpoint's advertised interval and USB backpressure. Under responsive, sustainable simulated load, its policy target is p99 physical queue residence within two polling periods. Input above destination capacity or an unresponsive endpoint can exceed that target.

## Troubleshooting

| Symptom | Check |
|---|---|
| `/dev/raw-gadget` is missing | Build or load Raw Gadget for the running kernel. |
| No UDC appears in `/sys/class/udc/` | Correct the Pi device-mode and `dwc2` configuration, then reboot. |
| UDP returns `not_ready` | Finish enumeration and move the physical mouse; confirm its descriptor is a supported relative mouse layout. |
| UDP returns `busy` | Another source IP/port owns the 250 ms controller lease. Stop that sender and let the lease expire. |
| A hold or click sequence stops | Continue snapshots or same-ID retries so the controller lease stays active. |
| `UPC1` returns `button_active` | The requested button is physically or persistently held, so a separate host-visible click cannot be guaranteed. |
| `UPC1` returns `queue_full` | Wait for recovery and retry the same command ID for the same logical sequence. |
| Replay validation fails | Recapture missing or truncated descriptors. Unsupported configurations require live mode. |
| Replay cannot bind the device or port | Stop any live proxy or previous replay process using the same UDC or UDP port. |
| Large movement or wheel commands fail | Check the native report ranges, declared axes, and available queue capacity. |

Use `--debug_level=3` to print outgoing report bytes during USB troubleshooting. Return to `--debug_level=0` for latency measurements.
