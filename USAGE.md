# UDP mouse control

## Start and connect

Build and start the Pi proxy as shown in [README.md](README.md). UDP listens on port **12345** only with `--enable_injection`. `USB_PROXY_BIND` selects the local IPv4 address (default `0.0.0.0`). `USB_PROXY_PEER` optionally restricts control to one source IPv4 address. This is an unauthenticated LAN protocol; the IP restriction is not cryptographic authentication.

Move the physical mouse once after enumeration. `+state` returns `not_ready` until a supported descriptor and matching physical report have been observed. Unsupported reports keep passing through without synthesized mouse commands.

One controller owns the endpoint for 250 ms after each accepted command, identified by source IP **and UDP port**. Use a persistent socket. Other controllers receive `busy`; status queries do not acquire or renew ownership. While holding an injected button, send a zero-motion snapshot at least every 100 ms. Timeout releases only injected buttons; physical holds remain effective. Release can only reach the host when the USB endpoint is accepting reports.

## Python / inference connection

Copy [client.py](client.py) to the inference PC; it uses only the Python standard library.

```python
from client import MouseProxy

with MouseProxy('192.168.1.20') as mouse:  # Pi Ethernet IP
    print(mouse.state())
    mouse.move(12, -6)  # relative HID counts
    mouse.button(1, True)
    mouse.move(8, 0)
    mouse.button(1, False)
```

Call `move(dx, dy)` for each new tracking correction. `dx`/`dy` are signed 16-bit values; wheel/pan are signed 8-bit values. The sender remembers its injected button mask. `move()` sends a heartbeat. There is no automatic background heartbeat, and closing the client requests release. UDP delivery is not guaranteed; the watchdog covers a lost final release.

A target's offset from the center of a 320×320 image is an input to your movement controller, not automatically a correct cursor delta. Apply sensitivity/calibration on the PC. Video frames never go to this proxy. This change does not implement capture, video transport, YOLO, or CUDA.

## Binary UPX1 protocol

One 16-byte datagram, network byte order:

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | ASCII `UPX1` |
| 4 | 4 | Unsigned sequence number |
| 8 | 2 | Signed relative X |
| 10 | 2 | Signed relative Y |
| 12 | 1 | Signed wheel |
| 13 | 1 | Signed horizontal pan |
| 14 | 1 | Complete injected button mask |
| 15 | 1 | Reserved, must be zero |

Python encoding: `struct.pack('!4sIhhbbBB', b'UPX1', seq, dx, dy, wheel, pan, buttons, 0)`.

Sequence advances modulo 2^32. Duplicate/older packets are ignored within an ownership lease; the sequence baseline resets after timeout. This is not replay protection across leases. Successful binary packets receive no ACK. Errors can return ASCII. Button masks are full snapshots so the next delivered snapshot repairs a lost press/release. Wheel events and brief clicks are not made reliable by UDP.

Consecutive unsent binary corrections with the same button mask can replace one another. Physical reports, wheel/pan commands, ASCII commands, and changes of button mask prevent replacement across that point. Already submitted USB reports cannot be replaced. This is a latest-correction API, not a lossless trajectory API. Binary motion/wheel/pan older than 25 ms **since enqueue on the Pi** is zeroed at USB dequeue; button state is still applied. It does not measure network transit age or cancel an ioctl already blocked in the kernel.

Moves are split across native HID reports (e.g. 320 counts needs three reports on a -127..127 axis). Maximum queued reports: 32. A command needing more than 32 reports, unsupported buttons/axes, or insufficient queue space is rejected as a whole. Physical reports are backpressured rather than intentionally discarded. Use modest corrections per inference step.

## ASCII commands

One command per datagram; optional trailing CR/LF. Success returns `ok`; invalid input returns `error command`, `error syntax`, or `error button`. Unsupported values, readiness and queue rejection return `error not_ready_range_or_queue_full`.

| Command | Meaning |
|---|---|
| `+move X Y` | Ordered relative movement, split to native range |
| `+wheel N` | Vertical wheel |
| `+pan N` | Horizontal pan, if advertised by mouse |
| `+mousedown [B]` | Hold injected button (default 1) |
| `+mouseup [B]` | Release injected button (default 1) |
| `+click [B]` | Press then schedule release after 10 ms; nonblocking |
| `+release` | Release all injected buttons |
| `+state` | Query processed state and native axis limits |

Button numbers are HID usages 1–8, typically left, right, middle, side1, side2. Buttons not present in the selected mouse report are rejected. A click on an already injected hold is rejected. A click during a physical hold cannot create a separate host-visible click. Timer release can be delayed by scheduling and USB backpressure.

`+state` returns:

```text
state PHYSICAL_MASK INJECTED_MASK COMBINED_MASK X_MIN X_MAX Y_MIN Y_MAX
```

State describes reports processed by the proxy writer; it is not proof of host delivery and is not an OS cursor position. Querying does not renew a button hold.

## MAKCU-inspired subset

The [MAKCU API](https://makcu.com/en/api/) was used as a command reference. This proxy accepts these ASCII spellings:

```text
km.move(10,-5)
km.wheel(1)
km.pan(-1)
km.left(1)
km.left(0)
km.right(1)
km.middle(0)
km.side1(1)
km.side2(0)
km.click(1)
km.release()
```

The leading `km` may be omitted. Button setters accept only 0/1. Replies follow this proxy's `ok`/error protocol, not MAKCU echo/prompt framing. Multi-argument clicks, silent release, curves, absolute movement, axis locks, remapping, keyboard commands, streaming, and MAKCU V2 binary framing are not implemented. Use `+state` for status. Raw endpoint hex injection from the earlier fork is no longer supported by the UDP parser.

Avoid mixing ASCII and binary controllers on one socket. ASCII is ordered and never replaces pending movement; binary is intended for current tracking corrections. Turn off verbose/per-packet logging for latency measurements.
