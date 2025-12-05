# USB-Proxy with UDP Injection (Raspberry Pi 4 + Logitech Mouse Fork)

> ⚠️ **Disclaimer**: This is a fork specifically optimized for **Raspberry Pi 4** with **Logitech mice**. The code has been "vibe coded" due to the complexity of USB protocols and my limited competence in this area. Use at your own risk! "It works on my machine"

This software is a USB proxy based on [raw-gadget](https://github.com/xairy/raw-gadget) and libusb, with added UDP injection capabilities for remote mouse control. It allows you to:
- Proxy USB mouse traffic between a device and host
- Inject mouse movements and clicks via UDP commands
- Preserve physical mouse button states during injection (e.g., drag while injecting movement)
- Automatically detect and adapt to Logitech mouse packet formats

## Hardware Setup

```
┌──────────────┐     ┌────────────────────────────────────┐     ┌─────────────┐
│              │     │                                    │     │             │
│   Logitech   │ USB │      Raspberry Pi 4 (RPi4)        │ USB │   Target    │
│   Mouse      ├─────┤  USB Host Port    USB OTG Port    ├─────┤   Computer  │
│              │     │  (Any USB port)   (USB-C port)    │     │             │
└──────────────┘     └────────────────────────────────────┘     └─────────────┘
```

## Prerequisites for Raspberry Pi 4

### 1. Enable USB OTG (dwc2) Module

The USB-C port on RPi4 can act as a USB device (OTG mode). Enable it:

```bash
echo "dtoverlay=dwc2" | sudo tee -a /boot/config.txt
echo "dwc2" | sudo tee -a /etc/modules
sudo reboot
```

After reboot, verify the UDC (USB Device Controller) is available:
```bash
ls /sys/class/udc/
# Should output: fe980000.usb
```

### 2. Build and Load raw-gadget Kernel Module

Clone and build raw-gadget:
```bash
git clone https://github.com/xairy/raw-gadget.git
cd raw-gadget
make
sudo insmod raw_gadget.ko
```

Verify it's loaded:
```bash
ls /dev/raw-gadget
# Should exist
```

### 3. Install Dependencies

```bash
sudo apt-get update
sudo apt-get install build-essential libusb-1.0-0-dev libjsoncpp-dev
```

## Building

```bash
cd usb-proxy-udp
make
```

## Usage

### Step 1: Find Your Mouse Details

Plug your Logitech mouse into **any regular USB port** on the RPi4 (not the USB-C OTG port):

```bash
lsusb
```

Example output:
```
Bus 001 Device 004: ID 046d:c539 Logitech, Inc. USB Receiver
```

Here, `046d` is the vendor ID and `c539` is the product ID.

### Step 2: Run the USB Proxy

Basic command:
```bash
sudo ./usb-proxy --device=fe980000.usb --driver=fe980000.usb --vendor_id=046d --product_id=c539
```

With debugging and injection enabled:
```bash
sudo ./usb-proxy \
    --device=fe980000.usb \
    --driver=fe980000.usb \
    --vendor_id=046d \
    --product_id=c539 \
    --debug_level=2 \
    --enable_injection
```

### Command Line Arguments

| Argument | Description | Example |
|----------|-------------|---------|
| `--device` | UDC device name (always `fe980000.usb` on RPi4) | `--device=fe980000.usb` |
| `--driver` | UDC driver name (always `fe980000.usb` on RPi4) | `--driver=fe980000.usb` |
| `--vendor_id` | USB vendor ID in hex | `--vendor_id=046d` |
| `--product_id` | USB product ID in hex | `--product_id=c539` |
| `--debug_level` | Debug verbosity: 0=off, 1=basic, 2=detailed, 3=full hex dumps | `--debug_level=2` |
| `--enable_injection` | Enable UDP injection and file-based injection | `--enable_injection` |
| `--injection_file` | JSON file with injection rules (default: `injection.json`) | `--injection_file=rules.json` |
| `--descriptor_file` | File to save USB descriptors (default: `usb_descriptors.json`) | `--descriptor_file=desc.json` |
| `-v/--verbose` | Increase general verbosity | `-v` |
| `-h/--help` | Show help message | `-h` |

## UDP Injection Commands

The proxy includes a UDP server on port **12345** that accepts injection commands.

### Mouse Movement: `+move X Y`

Inject a relative mouse movement.

**Format:** `+move X Y`
- `X`: Horizontal movement (-32768 to 32767, negative = left, positive = right)
- `Y`: Vertical movement (-32768 to 32767, negative = up, positive = down)

**Examples:**
```bash
# Move right 10 pixels
echo "+move 10 0" | nc -u -w1 localhost 12345

# Move left 50, down 20
echo "+move -50 20" | nc -u -w1 localhost 12345

# Move up 100
echo "+move 0 -100" | nc -u -w1 localhost 12345
```

**Button State Preservation:** The injected movement **preserves the current physical mouse button state**. If you're holding down the left button on the physical mouse, the injected movement will maintain that button press, enabling drag operations!

### Mouse Click: `+click`

Inject a quick left mouse button click (down + 10ms delay + up).

**Format:** `+click`

**Example:**
```bash
echo "+click" | nc -u -w1 localhost 12345
```

### Mouse Button Down: `+mousedown [button]`

Press and hold a mouse button.

**Format:** `+mousedown [button]`
- `button`: Optional button number (1=left, 2=right, 3=middle). Default: 1

**Examples:**
```bash
# Hold left button
echo "+mousedown 1" | nc -u -w1 localhost 12345

# Hold right button
echo "+mousedown 2" | nc -u -w1 localhost 12345
```

### Mouse Button Up: `+mouseup [button]`

Release a mouse button.

**Format:** `+mouseup [button]`
- `button`: Optional button number (1=left, 2=right, 3=middle). Default: 1

**Examples:**
```bash
# Release left button
echo "+mouseup 1" | nc -u -w1 localhost 12345

# Drag operation example
echo "+mousedown 1" | nc -u -w1 localhost 12345
echo "+move 100 50" | nc -u -w1 localhost 12345
echo "+move 50 25" | nc -u -w1 localhost 12345
echo "+mouseup 1" | nc -u -w1 localhost 12345
```

### Raw Packet Injection: `[EP] [HEX_DATA]`

Inject raw bytes into a specific endpoint.

**Format:** `[endpoint_hex] [hex_payload]`
- Endpoint address in hex (e.g., `81`, `82`)
- Payload as hex bytes (space-separated or concatenated)

**Examples:**
```bash
# Inject into endpoint 0x82
echo "82 02000a0005000000" | nc -u -w1 localhost 12345

# Space-separated hex
echo "81 01 02 03 04" | nc -u -w1 localhost 12345
```

## Mouse Packet Format (Logitech)

The Logitech mouse uses a **9-byte report format**:

| Byte | Offset | Description | Values |
|------|--------|-------------|--------|
| 0 | 0x00 | Magic/Report ID | Always `0x02` |
| 1 | 0x01 | Button state | `0x00`=none, `0x01`=left, `0x02`=right, `0x03`=both |
| 2 | 0x02 | Padding | `0x00` |
| 3 | 0x03 | X coordinate (low byte) | 16-bit signed little-endian |
| 4 | 0x04 | X coordinate (high byte) | |
| 5 | 0x05 | Y coordinate (low byte) | 16-bit signed little-endian |
| 6 | 0x06 | Y coordinate (high byte) | |
| 7 | 0x07 | Scroll wheel | `0xff`=down, `0x01`=up, `0x00`=none |
| 8 | 0x08 | Padding | `0x00` |

**Example packet:** `02 00 00 ce ff 0c 00 00 00`
- Magic: `02`
- Buttons: `00` (no buttons)
- Padding: `00`
- X: `ce ff` = -50 in little-endian (`0xFFCE`)
- Y: `0c 00` = 12 in little-endian (`0x000C`)
- Scroll: `00` (no scroll)
- Padding: `00`

## Button State Tracking

One of the key features is **automatic button state tracking**:

1. The proxy **monitors real mouse packets** from your physical mouse
2. It extracts the **button state** (byte 1) from each packet
3. When you inject a `+move` command, it **preserves** the current button state
4. This allows you to **drag while injecting movement** or perform complex operations

**Example workflow:**
```bash
# 1. Physical: Hold left mouse button on physical mouse
# 2. UDP: Inject movement commands
echo "+move 10 0" | nc -u -w1 localhost 12345    # Moves RIGHT while button held
echo "+move 10 0" | nc -u -w1 localhost 12345    # Moves RIGHT while button held
echo "+move 0 10" | nc -u -w1 localhost 12345    # Moves DOWN while button held
# 3. Physical: Release left mouse button on physical mouse
# Result: You've performed a drag operation!
```

## Debug Levels

Use `--debug_level` to control output verbosity:

- **0** (Off): No debug output, only errors
- **1** (Basic): Shows commands received, endpoints detected
  ```
  [UDP] Received: +move 10 5
  [CMD] Processing command: +move (using EP 0x82)
  [INJ] EP 0x82: Injected 9 bytes
  ```

- **2** (Detailed): Adds command details, button states
  ```
  [CMD] Mouse move: X=10, Y=5 (button: 0x01)
  [INIT] Found mouse endpoint: 0x82 (max packet: 16 bytes)
  ```

- **3** (Full Hex): Shows complete packet dumps byte-by-byte
  ```
  EP82(int_in): wrote 9 bytes to host: 02 01 00 0a 00 05 00 00 00
  ```

## Troubleshooting

### "unrecognized option" Error

Make sure you've recompiled after pulling updates:
```bash
make clean && make
```

### "Device or resource busy" Error

Another program is using the USB device or UDC:
```bash
# Check what's using the UDC
sudo lsof | grep fe980000

# Kill conflicting processes or reboot
sudo reboot
```

### "Could not find mouse endpoint"

Ensure your mouse is plugged in and detected:
```bash
lsusb | grep -i logitech
```

### Mouse format not learned

Move your physical mouse after starting the proxy. The proxy needs to see at least one real packet to learn the format.

## Advanced: File-Based Injection Rules

For more complex injection scenarios, use JSON rules with `--injection_file`:

```json
{
    "control": {
        "modify": [],
        "ignore": [],
        "stall": []
    },
    "int": [
        {
            "ep_address": 130,
            "enable": true,
            "content_pattern": ["\\x01\\x00\\x00\\x00"],
            "replacement": "\\x02\\x00\\x00\\x00"
        }
    ],
    "bulk": [],
    "isoc": []
}
```

This swaps left and right clicks by replacing button byte patterns.

## Project Structure

Key files:
- `usb-proxy.cpp` - Main entry point, argument parsing
- `proxy.cpp` - USB proxy logic, endpoint handling
- `udp_server.cpp` - UDP server, command processing
- `device-libusb.cpp` - Physical USB device interaction
- `host-raw-gadget.cpp` - Virtual USB device (gadget) side
- `misc.cpp` - Utilities for hex parsing, descriptors

## License

Same as upstream project.

## Credits

Original USB proxy: https://github.com/xairy/raw-gadget
Forked from: https://github.com/xairy/usb-proxy

Raspberry Pi 4 + Logitech mouse optimizations and UDP injection by me (vibe-coded with very limited competence).
