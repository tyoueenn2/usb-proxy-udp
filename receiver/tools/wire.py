"""UVF1 frame transport and UPX1/UPT1 reference codecs (network byte order)."""
import struct
import time

FRAME = struct.Struct('!4sBBHQIQHHIHHIHH')
HELLO = struct.Struct('!4sIQ')
SYNC_REQUEST = struct.Struct('!4sIQQ')
SYNC_REPLY = struct.Struct('!4sIQQQQ')
SUBSCRIBE = struct.Struct('!4sIQQ')
TELEMETRY = struct.Struct('!4sBBHQQQIiiiiI')
MOVE = struct.Struct('!4sIhhbbBB')


def fragments(pixels, width, height, session, sequence, stamp=None, pixel_format=1, datagram_size=1400):
    stride = datagram_size - FRAME.size
    if not (1 <= width <= 1024 and 1 <= height <= 1024 and pixel_format in (1, 2) and 1024 <= stride <= 1352):
        raise ValueError('Invalid dimensions, format, or datagram size')
    size = width * height * (3 if pixel_format == 1 else 4)
    if len(pixels) != size or not session:
        raise ValueError('Wrong pixel buffer length or zero session')
    stamp = time.perf_counter_ns() if stamp is None else stamp
    count = (size + stride - 1) // stride
    view = memoryview(pixels)
    for index in range(count):
        offset = index * stride
        yield FRAME.pack(b'UVF1', 1, pixel_format, 48, session, sequence & 0xffffffff,
                         stamp, width, height, size, index, count, offset, stride, 0) + view[offset:offset + stride]


def newer(a, b):
    return 0 < ((a - b) & 0xffffffff) < 0x80000000
