# UDP mouse control

## Start and connect

Build and start the Pi proxy as shown in [README.md](README.md). UDP listens on port **12345** only with `--enable_injection`. `USB_PROXY_BIND` selects the local IPv4 address (default `0.0.0.0`). `USB_PROXY_PEER` optionally restricts control to one source IPv4 address. The protocol is unauthenticated; use it only on a trusted LAN.

Move the physical mouse once after enumeration. `+state` returns `not_ready` until a supported descriptor and matching physical report have been observed. Unsupported physical reports continue through the proxy without modification.

One controller owns the endpoint for 250 ms after each accepted command, identified by source IP and UDP port. `UPC1` also binds the lease to its client session ID. Keep one socket open. Other controllers receive `busy`; telemetry and state queries do not acquire or renew ownership.

The proxy keeps three independent button masks:

```text
final_buttons = physical_buttons | persistent_injected_buttons | scheduled_click_buttons
```

Only real HID reports update `physical_buttons`. `UPX1` and ASCII button commands update `persistent_injected_buttons`. The Pi click scheduler alone updates `scheduled_click_buttons`. Clearing either synthetic mask therefore cannot release a button that the physical mouse still holds.

## Python client

Copy [client.py](client.py) to the controller. It uses only the Python standard library.

```python
from client import MouseProxy

with MouseProxy('192.168.1.20') as mouse:
    mouse.move(12, -6)
    mouse.button(1, True)       # persistent injected hold
    mouse.move(8, 0)
    mouse.button(1, False)

    result = mouse.schedule_clicks(
        button=1,
        count=25,
        press_ms=8,
        interval_ms=20,        # press-to-press: requested 50 clicks/second
        timeout=2.0,
    )
    print(result['accepted_clicks'], result['completed_clicks'])
```

`dx` and `dy` are signed relative HID counts. They are not absolute positions or guaranteed pixels. Apply sensitivity and acceleration calibration on the controller PC. Video capture, YOLO, and CUDA processing are outside this repository.

`schedule_clicks()` retries one immutable request with the same IDs and waits for completion by default. Those retries also renew the 250 ms lease. If `wait_complete=False` is used for a sequence longer than 250 ms, the caller must send accepted commands or same-ID retries frequently enough to keep the lease alive.

## UPX1 movement and persistent holds

`UPX1` remains the original 16-byte datagram in network byte order:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPX1` |
| 4 | 4 | Unsigned sequence number |
| 8 | 2 | Signed relative X |
| 10 | 2 | Signed relative Y |
| 12 | 1 | Signed wheel |
| 13 | 1 | Signed horizontal pan |
| 14 | 1 | Complete persistent injected button mask |
| 15 | 1 | Reserved; must be zero |

Python encoding:

```python
struct.pack('!4sIhhbbBB', b'UPX1', sequence, dx, dy, wheel, pan, buttons, 0)
```

The button byte is a complete persistent synthetic-hold snapshot. It never owns the Pi scheduler's temporary click state. A new `UPX1` snapshot can change a persistent hold while a scheduled click is active, but cannot cancel that click's press or release edge.

Sequence numbers advance modulo 2^32. Duplicate and older packets are ignored during a controller lease; the baseline resets after timeout. Successful `UPX1` packets have no acknowledgment. Errors remain silent for compatibility.

Only consecutive, unsent `UPX1` movement-only reports with an identical persistent button snapshot may replace earlier movement. The proxy never coalesces or moves a report across:

- a persistent button transition;
- a scheduled click press or release;
- wheel or pan input;
- an ASCII command; or
- a physical HID report.

Movement older than 25 ms since it entered the Pi queue is zeroed before USB submission, while its button snapshots are still applied. This expiry does not measure network age and cannot cancel a report already submitted to Raw Gadget.

## UPC1 reliable click requests

`UPC1` is a 40-byte request in network byte order. All reserved fields must be zero.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPC1` |
| 4 | 1 | Protocol version, currently `1` |
| 5 | 1 | Operation: `1` schedule, `2` release all |
| 6 | 1 | HID button usage 1–8; zero for release all |
| 7 | 1 | Reserved |
| 8 | 8 | Nonzero client session ID |
| 16 | 8 | Nonzero, monotonically increasing command ID |
| 24 | 4 | Click count |
| 28 | 4 | Press duration in microseconds |
| 32 | 4 | Press-to-press interval in microseconds |
| 36 | 4 | Reserved |

Python encoding:

```python
struct.pack('!4sBBBBQQIIII', b'UPC1', 1, operation, button, 0,
            session_id, command_id, count, press_us, interval_us, 0)
```

For operation 1, count must be 1–10,000 and the button must exist in the selected HID report. Press duration must be at least the endpoint's advertised polling interval and no more than 5 seconds. The interval must allow the press duration plus one polling interval and must be no more than 60 seconds. Timing starts from successful USB-writer completion of the press report. The release is not queued until that completion is known, so USB backpressure cannot reverse a press/release pair.

The Pi executes one click sequence at a time and keeps up to 64 accepted sequences. The shared USB report queue holds 32 reports and limits synthetic reports to 8, leaving capacity and service opportunities for physical input. If bounded capacity is unavailable, the command receives `queue_full`; accepted click edges are not silently discarded.

If the requested button is physically held or persistently injected when a sequence starts, the command terminates with `button_active`. The same status is returned if a hold appears while the sequence is running and prevents a host-visible release. Completed counts include only press/release pairs whose reports both completed in the USB writer.

Operation 2 is release all. Button, count, and both timing fields must be zero. It clears persistent holds, cancels queued or active click sequences, invalidates stale synthetic reports, and queues a final merged release. Physical holds remain in that report.

## UPA1 acknowledgments

Every valid or recognizable `UPC1` request receives a 48-byte `UPA1` response:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPA1` |
| 4 | 1 | Protocol version, currently `1` |
| 5 | 1 | Status code |
| 6 | 1 | Button usage |
| 7 | 1 | Reserved |
| 8 | 8 | Client session ID |
| 16 | 8 | Command ID |
| 24 | 4 | Accepted click count for this command |
| 28 | 4 | Completed click count for this command |
| 32 | 8 | Pi server epoch |
| 40 | 2 | Current scheduler queue depth |
| 42 | 2 | Reserved |
| 44 | 4 | Reserved |

| Code | Name | Meaning |
|---:|---|---|
| 1 | `accepted` | Command stored exactly once in this server epoch |
| 2 | `duplicate` | Same accepted command is still active or queued |
| 3 | `completed` | All accepted clicks completed, or release all completed |
| 4 | `busy` | Another controller or session owns the lease |
| 5 | `queue_full` | A bounded scheduler, cache, session, or report queue cannot accept work |
| 6 | `unsupported_button` | Usage is outside 1–8 or absent from the HID descriptor |
| 7 | `invalid` | Version, reserved bytes, operation, count, or timing is invalid |
| 8 | `cancelled` | Accepted work was stopped by release, timeout, reset, failure, or shutdown |
| 9 | `button_active` | A physical or persistent hold prevents a host-visible click |
| 10 | `not_ready` | No supported mouse report layout is active |
| 11 | `stale_command` | Command ID is below the session's accepted high-water mark and no longer cached |

Generate a random nonzero session ID when the client starts and increase its command ID for every new request. Retransmit the exact request after an ACK loss. A repeated `(session ID, command ID)` never schedules twice during the current Pi server epoch. Active and recent results are cached; a bounded per-session high-water mark prevents an evicted command from running again. The server accepts at most 32 distinct client sessions and caches up to 256 command records per epoch.

The epoch changes when the UDP server process restarts. Idempotency does not cross a restart. After observing a different epoch, create a new random session ID and reconcile application state before issuing new clicks.

## Watchdog, reset, and shutdown

After 250 ms without an accepted controller command or same-ID retry, the proxy releases ownership, cancels click work, clears both synthetic masks, invalidates queued synthetic reports, and queues a final merged report when an endpoint is available. The same cleanup runs for endpoint/layout generation changes, USB writer errors, release all, and shutdown. Physical state is read only from physical HID reports and is preserved while the physical endpoint remains present.

## Telemetry subscriptions

For compatibility with the earlier Receiver telemetry patch, the `UPS1`/`UPT1` layouts remain unchanged. A subscription is a 24-byte packet:

| Offset | Bytes | `UPS1` / `UPS2` field |
|---:|---:|---|
| 0 | 4 | ASCII `UPS1` for legacy telemetry or `UPS2` for extended telemetry |
| 4 | 4 | Reserved; must be zero |
| 8 | 8 | Nonzero receiver session ID |
| 16 | 8 | Increasing nonzero subscription token |

Refresh a subscription within 100 ms. Changed telemetry is sent immediately and unchanged telemetry is refreshed at up to 100 Hz.

`UPT1` remains 56 bytes:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPT1` |
| 4 | 1 | Ready flag |
| 5 | 1 | Physical button mask only |
| 6 | 2 | Reserved |
| 8 | 8 | Receiver session ID |
| 16 | 8 | Pi server epoch |
| 24 | 8 | Subscription token |
| 32 | 4 | Telemetry sequence |
| 36 | 4 | Signed X minimum |
| 40 | 4 | Signed X maximum |
| 44 | 4 | Signed Y minimum |
| 48 | 4 | Signed Y maximum |
| 52 | 4 | Reserved |

`UPT2` adds scheduler state without changing `UPT1`:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPT2` |
| 4 | 1 | Ready flag |
| 5 | 1 | Physical button mask only |
| 6 | 1 | Applied persistent injected mask |
| 7 | 1 | Applied scheduled click mask |
| 8 | 8 | Receiver session ID |
| 16 | 8 | Pi server epoch |
| 24 | 8 | Subscription token |
| 32 | 4 | Telemetry sequence |
| 36 | 16 | Signed X/Y minimum and maximum |
| 52 | 4 | Last physical X delta |
| 56 | 4 | Last physical Y delta |
| 60 | 4 | Total accepted clicks in this epoch, saturated at 2^32−1 |
| 64 | 4 | Total completed clicks in this epoch, saturated at 2^32−1 |
| 68 | 2 | Active click sequence count |
| 70 | 2 | Queued click sequence count |
| 72 | 8 | Mouse layout generation |

Physical direction and physical button fields come only from real HID reports. Injected movement and synthetic button masks never contaminate those fields.

## ASCII compatibility

Existing ASCII and MAKCU-inspired spellings remain available:

| Command | Meaning |
|---|---|
| `+move X Y` | Ordered relative movement |
| `+wheel N` | Vertical wheel |
| `+pan N` | Horizontal pan |
| `+mousedown [B]` | Set one persistent injected hold |
| `+mouseup [B]` | Clear one persistent injected hold |
| `+click [B]` | Schedule one 10 ms click on the Pi |
| `+release` | Cancel click work and release all synthetic holds |
| `+state` | Query applied physical/synthetic state and axis limits |

Button numbers are HID usages 1–8, usually left, right, middle, side 1, and side 2. `+state` keeps its existing format; its synthetic field is the union of persistent and scheduled masks:

```text
state PHYSICAL_MASK SYNTHETIC_MASK COMBINED_MASK X_MIN X_MAX Y_MIN Y_MAX
```

MAKCU-inspired aliases include `km.move(10,-5)`, `km.wheel(1)`, `km.left(1)`, `km.right(0)`, `km.click(1)`, and `km.release()`. This is a command-spelling subset, not MAKCU framing or complete API compatibility.

The 10, 25, and 50 clicks-per-second checks in `make test` use a simulated USB writer. They verify scheduler ordering and accepted edge accounting, not Raspberry Pi hardware throughput. No physical click rate is claimed until it is measured on a Raspberry Pi 4 with an actual USB host and Raw Gadget endpoint.
