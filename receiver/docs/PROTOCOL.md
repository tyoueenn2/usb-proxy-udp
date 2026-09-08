# Network protocol v1

All integers are network byte order. One message per datagram. These are unauthenticated LAN protocols; only configured peers are accepted by the receiver. Never pack native C++ structs directly onto the wire. Reference encoders are in `tools/wire.py`.

## UVF1: raw frames, default UDP port 5000

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 4 | ASCII `UVF1` |
| 4 | 1 | Version = 1 |
| 5 | 1 | Format: 1 = RGB24, 2 = BGRA32 |
| 6 | 2 | Header length = 48 |
| 8 | 8 | Nonzero sender session ID |
| 16 | 4 | Frame sequence, modulo 2^32 |
| 20 | 8 | Capture timestamp in sender monotonic nanoseconds |
| 28 | 2 | Width |
| 30 | 2 | Height |
| 32 | 4 | Complete raw frame length |
| 36 | 2 | Fragment index, zero-based |
| 38 | 2 | Fragment count |
| 40 | 4 | Payload byte offset |
| 44 | 2 | Fragment payload stride |
| 46 | 2 | Reserved = 0 |
| 48 | variable | Raw image bytes |

Rows are tightly packed, top-to-bottom, left-to-right. BGRA alpha is ignored. Width and height each range from 1 to 1024. Frame length must equal width × height × channels. Payload stride is 1024–1352; default 1352 gives a 1,400-byte UDP payload including this header. Fragment count is `ceil(frame_length / stride)`, offset is `index * stride`, and only the final fragment may be shorter than stride. The final fragment must have its exact remaining length. No compression, IP fragmentation dependency, FEC, or retransmission is used.

Metadata must agree across every fragment of a frame. Identical duplicates are ignored; a conflicting duplicate or conflicting metadata invalidates the in-flight assembly. Incomplete frames expire 20 ms after their first received fragment. Up to three incomplete frames are retained; the oldest assembly is evicted when a fourth arrives. Completing a newer frame retires older assemblies and suppresses subsequently received older/duplicate frame IDs. Sequence comparison uses the unsigned half-range rule; a sender must not jump by 2^31 or more within a session.

Start each sender process with a fresh random session ID and send a hello every 50 ms:

`UVH1 | reserved:u32=0 | session:u64` (16 bytes)

Frames are accepted only after a hello from the configured sender IP. The active sender endpoint includes its UDP source port. A different endpoint may take over after no hello has been seen for 100 ms. A session change flushes incomplete/pending frames, sync, preview, and controller state; recently retired sessions are rejected. Use one socket for hello, frames, and timestamp exchange.

## Clock exchange and capture age

Receiver request, 24 bytes:

`UVC1 | reserved:u32=0 | sender_session:u64 | t0:u64`

Sender reply, 40 bytes:

`UVS1 | reserved:u32=0 | sender_session:u64 | t0:u64 | t1:u64 | t2:u64`

`t0` is the receiver's request timestamp, echoed unchanged. `t1` is the sender timestamp immediately after receiving the request; `t2` is the sender timestamp immediately before replying. The receiver records `t3` on receipt. The sender clock offset relative to the receiver lies in `[t2-t3, t1-t0]`, without assuming symmetric transit. A request is sent every 250 ms; only its matching reply is accepted. Exchanges with negative/impossible durations or RTT over 50 ms are rejected. Sync expires after two seconds.

The upper capture-age estimate is `receiver_now - capture_timestamp + offset_upper`, expanded by a 200 ppm relative-clock-drift allowance and 0.1 ms timestamp margin. Timestamps must come from the same monotonic clock as the sender's sync responses. Stamp actual capture time, not packet-send time. Sleep/resume or clock discontinuities require a new sender session. The bound assumes the stated drift envelope and correct sender timestamps; it is not a hardware measurement.

Unsynchronized frames can be inferred/previewed but cannot produce movement. Capture age and receiver-local age are both checked before command submission. Default maximum age is 50 ms (configurable 1–250 ms).

## UPX1: existing proxy mouse commands, UDP port 12345

The receiver uses the proxy's unchanged 16-byte format:

`UPX1 | sequence:u32 | dx:i16 | dy:i16 | wheel:i8 | pan:i8 | injected_buttons:u8 | reserved:u8=0`

This application always sends wheel, pan, and injected buttons as zero. X/Y are calibrated relative HID counts. It clamps each correction to both the configured cap and native axis limits reported by telemetry. It sends each processed correction at most once and does not resend trajectories after packet loss.

The proxy's movement owner is the source IP and UDP port, with a 250 ms lease. Binary commands receive no success ACK. Error strings and `busy` disarm the receiver. Existing proxy behavior includes duplicate/older sequence rejection within a lease, replacement of eligible queued corrections, and 25 ms age expiry measured from Pi enqueue. That expiry does not cover network transit, USB kernel blocking, or reports already sent.

## UPS1/UPT1: compatible Pi telemetry extension

UPS1 subscription/renewal, 24 bytes:

`UPS1 | reserved:u32=0 | receiver_session:u64 | token:u64`

Both session and token are nonzero. Receiver session changes on each receiver start. Tokens increase within a session. Renew every 20 ms; the subscription expires after 100 ms. One subscriber is allowed; another endpoint receives `busy` while a subscription is alive. Subscribing never acquires or renews movement ownership. It uses the same existing Pi command port.

UPT1 snapshot, 56 bytes:

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 4 | ASCII `UPT1` |
| 4 | 1 | Ready, 0 or 1 |
| 5 | 1 | Physical mouse button mask |
| 6 | 2 | Reserved = 0 |
| 8 | 8 | Receiver session from UPS1 |
| 16 | 8 | Random Pi server session |
| 24 | 8 | Latest accepted subscription token |
| 32 | 4 | Snapshot sequence, modulo 2^32 |
| 36 | 4 | Native X minimum, signed |
| 40 | 4 | Native X maximum, signed |
| 44 | 4 | Native Y minimum, signed |
| 48 | 4 | Native Y maximum, signed |
| 52 | 4 | Reserved = 0 |

The UDP worker publishes on a state change on its next iteration (its poll timeout is at most 2 ms), on renewal, and otherwise every 10 ms. The USB writer never performs telemetry network I/O. State is the proxy's processed physical report, not proof of host delivery.

The receiver accepts only its configured Pi endpoint, its own session, increasing snapshot sequences, and tokens it issued in the past 50 ms. Older tokens cannot roll state backward. Changing Pi server epoch requires a newer token. Activation requires both the snapshot receipt and its token to remain within 50 ms, ready=true, GUI armed, and the selected physical button held. This limits delayed heartbeat replay without comparing Pi and PC clocks.

UPT1 capability is mandatory for v1 mouse-driven activation. The receiver does not silently fall back to uncorrelated `+state` polling. Existing `+state` and MAKCU-inspired ASCII commands are unchanged. MAKCU serial framing and MAKCU V2 binary packets are not used.
