# USB Proxy UDP Server - Usage Guide

## Starting the Server

### Basic Usage
```bash
sudo ./usb-proxy
```

### With Debug Output
```bash
# Level 1: Basic events
sudo ./usb-proxy --debug_level 1

# Level 2: Detailed packet info
sudo ./usb-proxy --debug_level 2

# Level 3: Full hex dumps
sudo ./usb-proxy --debug_level 3
```

### Custom Descriptor File
```bash
sudo ./usb-proxy --descriptor_file my_device.json
```

### All Options
```bash
sudo ./usb-proxy --help
```

Available options:
- `--device`: Specify USB raw gadget device (default: `dummy_udc.0`)
- `--driver`: Specify USB driver (default: `dummy_udc`)
- `--vendor_id`: Filter by vendor ID (hex)
- `--product_id`: Filter by product ID (hex)
- `--debug_level`: Set debug verbosity 0-3 (default: 0)
- `--descriptor_file`: USB descriptor output file (default: `usb_descriptors.json`)
- `--enable_injection`: Enable injection feature
- `--injection_file`: Injection rules file (default: `injection.json`)

## Sending Commands via UDP

The server listens on **UDP port 12345**. You can send commands using various tools:

### Using netcat (nc)

#### Mouse Movement
```bash
echo "+move -10 5" | nc -u -w1 localhost 12345
```
Moves mouse -10 pixels in X, +5 pixels in Y

#### Mouse Click
```bash
echo "+click" | nc -u -w1 localhost 12345
```
Performs a left mouse button click

### Using Python

```python
import socket

# Create UDP socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server_address = ('localhost', 12345)

# Send mouse movement
sock.sendto(b'+move 10 -5', server_address)

# Send mouse click
sock.sendto(b'+click', server_address)

# Send raw hex data to endpoint 0x81
sock.sendto(b'81 00010203', server_address)

sock.close()
```

### Using socat
```bash
# Mouse movement
echo "+move 20 10" | socat - UDP:localhost:12345

# Click
echo "+click" | socat - UDP:localhost:12345
```

## Command Format

### High-Level Commands (starts with `+`)

#### Mouse Movement
```
+move X Y
```
- `X`: Horizontal movement (-127 to 127)
- `Y`: Vertical movement (-127 to 127)
- Example: `+move -19 10`

#### Mouse Click
```
+click
```
Simulates a left mouse button click (down then up)

### Raw Endpoint Injection

```
ENDPOINT HEXDATA
```

- `ENDPOINT`: Hex endpoint address (e.g., `81` for EP 0x81)
- `HEXDATA`: Hex bytes to send (spaces optional)

Examples:
```bash
# All of these formats work:
echo "81 00010203" | nc -u -w1 localhost 12345
echo "81 00 01 02 03" | nc -u -w1 localhost 12345
echo "81 000102030405" | nc -u -w1 localhost 12345
```

## USB Descriptor File

When the server starts, it automatically saves the USB device descriptors to a JSON file.

### Default Location
`usb_descriptors.json` in the current directory

### File Contents
The JSON file contains:
- **Device descriptor**: Vendor ID, Product ID, USB version, etc.
- **Configuration descriptors**: Power, attributes, interfaces
- **Interface descriptors**: Class, subclass, protocol, endpoints
- **Endpoint descriptors**: Address, attributes, max packet size, interval

### Example Structure
```json
{
  "device": {
    "idVendor": 1133,
    "idProduct": 49970,
    "bDeviceClass": 0,
    "bMaxPacketSize0": 64,
    ...
  },
  "configurations": [
    {
      "bConfigurationValue": 1,
      "bNumInterfaces": 1,
      "interfaces": [
        {
          "altsettings": [
            {
              "bInterfaceClass": 3,
              "bNumEndpoints": 1,
              "endpoints": [
                {
                  "bEndpointAddress": 129,
                  "bmAttributes": 3,
                  "wMaxPacketSize": 8
                }
              ]
            }
          ]
        }
      ]
    }
  ]
}
```

## Debug Output Examples

### Level 1 (Basic)
```
[UDP] Received: +move 10 5
[CMD] Processing command: +move (using EP 0x81)
[INJ] EP 0x81: Injected 4 bytes
```

### Level 2 (Detailed)
```
[UDP] Received: +move 10 5
[CMD] Processing command: +move (using EP 0x81)
[CMD] Mouse move: X=10, Y=5
[INJ] EP 0x81: Injected 4 bytes
```

### Level 3 (Full Hex Dumps)
```
[UDP] Received: +move 10 5
[CMD] Processing command: +move (using EP 0x81)
[CMD] Mouse move: X=10, Y=5
[INJ] EP 0x81: Injected 4 bytes
[INJ] Data: 00 0a 05 00
```

## Testing the Setup

### 1. Start the server with debug output
```bash
sudo ./usb-proxy --debug_level 2
```

### 2. Check that the descriptor file was created
```bash
ls -la usb_descriptors.json
cat usb_descriptors.json | head -20
```

### 3. Send a test command
```bash
echo "+move 5 5" | nc -u -w1 localhost 12345
```

### 4. Verify in the debug output
You should see output like:
```
[UDP] Received: +move 5 5
[CMD] Processing command: +move (using EP 0x81)
[CMD] Mouse move: X=5, Y=5
[INJ] EP 0x81: Injected 4 bytes
```

## Troubleshooting

### "Could not find mouse endpoint for injection"
- Check your USB device descriptors in the JSON file
- Look for an endpoint with:
  - `bmAttributes` = 3 (Interrupt transfer)
  - `bEndpointAddress` with bit 7 set (IN direction)
- Ensure your device is properly enumerated

### "Endpoint 0xXX not found for injection"
- Verify the endpoint address in your descriptor file
- Check that the interface is active
- Make sure the device is in the correct configuration

### Commands not responding
- Check if the UDP server started (should show "UDP Server started on port 12345")
- Verify firewall settings aren't blocking UDP port 12345
- Test with debug level 1 or higher to see if packets are received

### Descriptor file not created
- Check file permissions in the current directory
- Look for error messages about file writing
- Try specifying an absolute path with `--descriptor_file`
