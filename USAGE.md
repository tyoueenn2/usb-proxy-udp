# UDP mouse control

## Start and connect

Build and start the Pi proxy as shown in [README.md](README.md). UDP listens on port **12345** only with `--enable_injection`. `USB_PROXY_BIND` selects the local IPv4 address (default `0.0.0.0`). `USB_PROXY_PEER` optionally restricts control to one source IPv4 address. The protocol is unauthenticated; use it only on a trusted LAN.

Move the physical mouse once after enumeration. `+state` returns `not_ready` until a supported descriptor and matching physical report have been observed. Unsupported physical reports continue through the proxy without modification.

One controller owns the endpoint for 250 ms, identified only by source IP and UDP source port. Keep one socket open. Valid UPX1 traffic, accepted UPC1 commands, and exact UPC1 retries from that endpoint renew the lease. A changed client session on the same UDP endpoint does not create a second owner. Other endpoints receive `busy`; telemetry and state queries do not acquire or renew ownership.

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
        interval_ms=12,        # v2: release completion to next press
        protocol_version=2,
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

Only consecutive, unsent `UPX1` movement-only reports with an identical persistent button snapshot may replace earlier movement. This is a newest-correction policy: successful UDP transmission does not guarantee that every movement packet reaches USB. The proxy never coalesces or moves a report across:

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
| 4 | 1 | Protocol version: `1` or `2` |
| 5 | 1 | Operation: `1` schedule, `2` release all |
| 6 | 1 | HID button usage 1–8; zero for release all |
| 7 | 1 | Reserved |
| 8 | 8 | Nonzero client session ID |
| 16 | 8 | Nonzero, monotonically increasing command ID |
| 24 | 4 | Click count |
| 28 | 4 | Press duration in microseconds |
| 32 | 4 | v1 press-to-press interval; v2 release-completion-to-next-press gap, in microseconds |
| 36 | 4 | Reserved |

Python encoding:

```python
struct.pack('!4sBBBBQQIIII', b'UPC1', 1, operation, button, 0,
            session_id, command_id, count, press_us, interval_us, 0)
```

Use version byte `2` for release-gap timing. The Python helper selects it with `protocol_version=2`.

For operation 1 in both versions, count must be 1–10,000 and the button must exist in the selected HID report. Press duration must be at least the endpoint's advertised polling interval and no more than 5 seconds. Timing starts only after successful USB-writer completion of the press report, and a click completes only after successful writer completion of its release.

Version 1 is unchanged: the interval is press-to-press, must be at least `press_us + poll_interval_us`, and may not exceed 60 seconds. Version 2 treats the interval as a gap after successful release completion. Its range is 0 through 60 seconds. A zero v2 gap can queue the next press only after the previous release has completed, so writer ordering and destination polling still separate the edges.

The Pi executes one click sequence at a time and keeps up to 64 accepted sequences. Genuine physical reports remain in their ordered 32-report FIFO. Synthetic work uses a separate eight-item overlay queue, and completion/failure feedback uses a bounded 32-event queue. If bounded capacity is unavailable, the command receives `queue_full`; accepted click edges are not silently discarded.

If the requested button is physically held or persistently injected before admission, the request is rejected with `button_active`, `accepted=0`, and `completed=0`. If the conflict appears after admission, the accepted command terminates with `button_active` while retaining its original accepted count and the number of press/release pairs already completed. Completed counts include only pairs whose reports both completed in the USB writer.

Operation 2 is release all. Button, count, and both timing fields must be zero. It clears persistent holds, cancels queued or active click sequences, invalidates stale synthetic reports, and creates a pending final merged-release obligation. Physical holds remain in that report. Temporary loss of a usable report template, endpoint replacement, a layout-generation change, watchdog activity, or a writer failure leaves the accepted ReleaseAll pending. It becomes `completed` only after the full release report succeeds in the USB writer.

An exact retry of a pending ReleaseAll returns `duplicate`; an exact retry after completion returns the cached `completed` result. A higher command ID in the same session may replace the pending ReleaseAll. The old command then becomes terminal `cancelled`, and delayed packets for that old ID replay `cancelled` rather than creating another release. A new session from the current owner can also replace the fence. Lower uncached IDs remain stale.

## UPA1 acknowledgments

Every valid or recognizable `UPC1` request receives a 48-byte `UPA1` response:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPA1` |
| 4 | 1 | Same supported protocol version as the request: `1` or `2` |
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
| 1 | `accepted` | Command admitted and stored as one operation |
| 2 | `duplicate` | Same accepted command is still active or queued |
| 3 | `completed` | Every accepted click release, or the ReleaseAll report, completed in the USB writer |
| 4 | `busy` | Another source IP/UDP-port endpoint owns the lease |
| 5 | `queue_full` | A bounded scheduler, cache, session, or report queue cannot accept work |
| 6 | `unsupported_button` | Usage is outside 1–8 or absent from the HID descriptor |
| 7 | `invalid` | Version, reserved bytes, operation, count, or timing is invalid |
| 8 | `cancelled` | Accepted work was stopped by release, timeout, reset, failure, or shutdown |
| 9 | `button_active` | A physical or persistent hold prevents a host-visible click |
| 10 | `not_ready` | No supported mouse report layout is active |
| 11 | `stale_command` | Command ID is not greater than the retained session high-water mark and is no longer cached |

Generate a random nonzero session ID when the client starts and increase its command ID for every new request. Retransmit the exact request after an ACK loss. Reusing a retained `(session ID, command ID)` with a different version, operation, button, count, or timing payload returns `invalid` without changing the original command. Exact active and recent results are cached in up to 256 command records.

The session tracker keeps 32 active session high-water marks. When a new Receiver session arrives, the least-recently-used session with no nonterminal command is retired; its high-water mark remains in a bounded 256-entry tombstone set for 10 minutes. A safety ReleaseAll may retire the least-recently-used active entry even when every slot owns pending work, because the release cancels that work. Within retained cache/high-water state, a retry cannot schedule twice. After a retired tombstone expires or is evicted by more than 256 newer retired sessions, an old datagram is outside the replay-protection window and must be treated as unsafe by the network/controller. Use random session IDs, keep delayed traffic below this window, and use `USB_PROXY_PEER` on a trusted LAN.

`queue_full` is transient: it creates no cache record and does not advance the session high-water mark, so the same exact ID may be retried later. An accepted ReleaseAll does advance the high-water mark; delayed lower Schedule IDs then return `stale_command` rather than recreating cancelled work.

The Pi generates a random nonzero epoch whenever its UDP server starts. Cache, high-water, and tombstone state do not cross a restart. After observing a different epoch, create a new random session ID and reconcile application state before issuing new clicks.

The result levels have distinct meanings:

- **Submitted locally:** the client sent a UDP datagram; this does not prove Pi acceptance.
- **Accepted by Pi:** an `accepted` or active `duplicate` response proves the command is stored idempotently in the current retained server state.
- **Completed by USB writer:** `completed` proves the required release write returned its full report length.
- **Observed by the destination USB stack:** software simulation and a successful local writer call do not prove this; verify it with the actual Pi, Raw Gadget endpoint, and target host.

## Watchdog, reset, and shutdown

After 250 ms without valid UPX1 traffic, an accepted controller command, or an exact same-ID retry, the proxy releases ownership, cancels click work, clears the desired synthetic masks, invalidates queued synthetic reports, and keeps a final merged release pending until an endpoint can accept it. The same cleanup runs for endpoint/layout generation changes, USB writer errors, release all, and shutdown. Physical state is read only from physical HID reports and is preserved while the physical endpoint remains present. A successful physical report that was queued or submitted before a synthetic reset still updates ordered host-visible physical completion state; a failed physical write does not.

UPT3 applied masks change only after a full successful USB write. UPT2 intentionally has no synthetic-mask fields. If a layout change temporarily removes the usable report template, the release remains pending and is merged after the next matching physical report establishes the new layout. This avoids reporting a release as applied while no endpoint could receive it.

## Poll-aware HID mixing

The physical reader puts every received report into a bounded ordered FIFO and backpressures at 32 reports. It never intentionally coalesces physical reports. The mixer copies each physical report, including its report ID and unknown/vendor bytes, and changes only fields identified by the active HID layout.

The next eligible synthetic movement, wheel, pan, or button state is fused into the oldest compatible physical report when all sums fit their native HID ranges. If any movement component would overflow, the physical movement is sent unchanged and synthetic movement stays pending. A button transition or release in the same item can still be applied while that movement waits. Synthetic state is committed only after the USB writer returns the full report length.

When no compatible physical report is ready, the mixer creates a standalone report from the last successful physical template. If physical input is queued, standalone synthetic output has a burst limit of one and receives at most one of every four successful output opportunities. A physical queue depth of two or more always sends physical next. Fusion does not consume this standalone budget.

The endpoint polling period comes from the forwarded descriptor: high speed uses `125 µs << (bInterval - 1)` and full/low speed uses `bInterval × 1000 µs`. Raw Gadget and destination host polling remain authoritative. Under a responsive endpoint and sustainable simulated load, the policy target is p99 physical FIFO residence within two polling periods. An unresponsive endpoint or source rate above destination capacity causes visible queue growth/backpressure and can exceed the target; the software does not claim losslessness outside sustainable load.

## Telemetry subscriptions

Use `UPS3`/`UPT3` as the primary telemetry protocol. The canonical `UPS2`/`UPT2` format is the compatibility fallback used by the audited Receiver parser; `UPS1`/`UPT1` also remains unchanged. A subscription is a 24-byte packet:

| Offset | Bytes | `UPS1` / `UPS2` / `UPS3` field |
|---:|---:|---|
| 0 | 4 | ASCII `UPS1`, `UPS2`, or `UPS3` |
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

`UPT2` is exactly 80 bytes. Its first 56 bytes are UPT1-compatible, including zero reserved fields:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPT2` |
| 4 | 1 | Ready flag |
| 5 | 1 | Physical button mask only |
| 6 | 2 | Reserved, zero |
| 8 | 8 | Receiver session ID |
| 16 | 8 | Pi server epoch |
| 24 | 8 | Subscription token |
| 32 | 4 | Telemetry sequence |
| 36 | 16 | Signed X/Y minimum and maximum |
| 52 | 4 | Reserved, zero |
| 56 | 8 | Mouse endpoint/layout generation |
| 64 | 8 | Pi monotonic sample time in nanoseconds |
| 72 | 2 | Last physical X delta, signed |
| 74 | 2 | Last physical Y delta, signed |
| 76 | 4 | Microseconds since last nonzero physical motion, saturated; `0xffffffff` if none |

Physical direction and physical button fields come only from real HID reports. Injected movement and synthetic button masks never contaminate those fields.

`UPT3` is exactly 128 bytes and extends telemetry without changing UPT1 or the public 80-byte UPT2:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII `UPT3` |
| 4 | 1 | Ready flag |
| 5 | 1 | Physical button mask only |
| 6 | 1 | Applied persistent injected mask |
| 7 | 1 | Applied scheduled-click mask |
| 8 | 8 | Receiver session ID |
| 16 | 8 | Pi server epoch shared with UPA1 |
| 24 | 8 | Echoed subscription token |
| 32 | 4 | Telemetry sequence modulo 2^32 |
| 36 | 4 | Native X minimum, signed |
| 40 | 4 | Native X maximum, signed |
| 44 | 4 | Native Y minimum, signed |
| 48 | 4 | Native Y maximum, signed |
| 52 | 4 | Destination endpoint polling interval in microseconds |
| 56 | 8 | Mouse endpoint/layout generation |
| 64 | 8 | Pi monotonic snapshot time in nanoseconds |
| 72 | 8 | Cumulative physical X counts, signed |
| 80 | 8 | Cumulative physical Y counts, signed |
| 88 | 4 | Microseconds since last nonzero physical motion, saturated; `0xffffffff` if none |
| 92 | 4 | Total accepted clicks this epoch, saturated |
| 96 | 4 | Total completed clicks this epoch, saturated |
| 100 | 2 | Active click-sequence count |
| 102 | 2 | Queued click-sequence count |
| 104 | 4 | Matching physical reports received, saturated |
| 108 | 4 | Matching physical reports successfully submitted, saturated |
| 112 | 2 | Physical endpoint/output FIFO depth, saturated |
| 114 | 2 | Pending synthetic-item depth, saturated |
| 116 | 4 | Superseded synthetic movement count, saturated |
| 120 | 4 | USB writer failure count, saturated |
| 124 | 4 | Reserved, zero |

The physical mask, UPT2 deltas, UPT3 cumulative counts, and physical report counters are captured before UDP overlays are applied. A fused output increments the successfully submitted physical count once. The UDP thread takes snapshots and sends telemetry; the USB writer performs no network I/O.

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

## Raspberry Pi 4 hardware validation

Run this procedure for each physical mouse or wireless-receiver mode before calling it supported:

1. Record `lsusb` and `lsusb -v -d VID:PID` for the exact device. Start the live proxy with those IDs, the actual name from `/sys/class/udc/`, and `USB_PROXY_PEER` restricted to the Receiver PC. Confirm the target PC reports the captured VID/PID and a HID mouse. The sample Logitech IDs in the README are examples only.
2. Move and click every physical button with UDP idle. Confirm press and release on the target, including a queued physical right-button release immediately followed by `+release` and by a 250 ms controller timeout. No button may reappear or remain stuck.
3. Hold each physical button while applying and releasing a different UPX1 persistent hold. Repeat with both sources using the same button. The target must keep the button down until both contributing masks are released.
4. Run single and multi-click UPC1 v2 commands at a conservative rate. Capture `accepted` and `completed` separately. Introduce a physical hold before admission and midway through a sequence; verify zero-count and partial-progress `button_active` responses respectively.
5. Add sustained physical motion while sending newest-correction movement and scheduled clicks. Monitor UPT3 physical received/submitted counts, output depth, synthetic depth, superseded movement, writer failures, and direction fields. Verify click edges remain ordered and physical traffic remains responsive under congestion.
6. Accept a ReleaseAll, then temporarily disconnect/reconnect or reconfigure the target USB link before its completion. Retry the same ID until it reports `completed`; repeat with an induced writer failure and with a higher-ID replacement release.
7. Restart Receiver more than 32 times without restarting the Pi server. Each new random session must still schedule and release. Send delayed lower IDs from retired sessions during the 10-minute retention window and confirm `stale_command`; retry a terminal exact request from a new UDP source port after the 250 ms ownership lease expires.
8. Before stopping the proxy, send ReleaseAll and wait for `completed`, then stop it with one Ctrl+C. Reconnect the physical receiver and target cable, repeat enumeration, and confirm neutral synthetic state. Exercise target suspend/resume and Pi reboot separately.

Record the Pi kernel, Raw Gadget revision, mouse/receiver firmware and mode, USB descriptor capture, endpoint interval, target OS, packet-loss setup, and USB observation method with the results. A successful local writer return is not proof that the target observed an edge; use target-side event logging or USB capture for that distinction.
