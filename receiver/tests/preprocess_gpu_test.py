"""Execute the production kernel via the CUDA driver and compare with a CPU reference."""
import argparse
import ctypes as c
import sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from validate_gpu import preprocess


def main():
    p = argparse.ArgumentParser(); p.add_argument('ptx'); a = p.parse_args()
    cuda = c.WinDLL('nvcuda.dll')
    def call(name, types, *args):
        f = getattr(cuda, name); f.argtypes = types; f.restype = c.c_int
        status = f(*args)
        if status:
            raise RuntimeError(f'{name}: CUDA status {status}')
    ctx, module, function = c.c_void_p(), c.c_void_p(), c.c_void_p()
    device = c.c_int()
    call('cuInit', [c.c_uint], 0); call('cuDeviceGet', [c.POINTER(c.c_int), c.c_int], c.byref(device), 0)
    call('cuCtxCreate_v2', [c.POINTER(c.c_void_p), c.c_uint, c.c_int], c.byref(ctx), 0, device)
    source = c.create_string_buffer(Path(a.ptx).read_bytes())
    try:
        call('cuModuleLoadData', [c.POINTER(c.c_void_p), c.c_void_p], c.byref(module), source)
        call('cuModuleGetFunction', [c.POINTER(c.c_void_p), c.c_void_p, c.c_char_p], c.byref(function), module, b'preprocess_kernel')
        rng = np.random.default_rng(32)
        for w, h, channels, size in [(160, 160, 3, 320), (320, 160, 4, 160), (127, 91, 3, 320), (1, 1, 4, 32)]:
            raw = rng.integers(0, 256, (h, w, channels), dtype=np.uint8)
            rgb = raw if channels == 3 else np.ascontiguousarray(raw[:, :, 2::-1])
            expected = preprocess(rgb.tobytes(), w, h, size)
            actual = np.empty((1, 3, size, size), dtype=np.float32)
            d_raw, d_out = c.c_uint64(), c.c_uint64()
            call('cuMemAlloc_v2', [c.POINTER(c.c_uint64), c.c_size_t], c.byref(d_raw), raw.nbytes)
            try:
                call('cuMemAlloc_v2', [c.POINTER(c.c_uint64), c.c_size_t], c.byref(d_out), actual.nbytes)
                call('cuMemcpyHtoD_v2', [c.c_uint64, c.c_void_p, c.c_size_t], d_raw, raw.ctypes.data, raw.nbytes)
                scale = min(size / w, size / h); rw, rh = int(np.floor(w * scale + .5)), int(np.floor(h * scale + .5))
                values = [d_raw, d_out] + [c.c_int(v) for v in [w, h, channels, size, rw, rh, (size - rw) // 2, (size - rh) // 2]]
                args = (c.c_void_p * len(values))(*[c.cast(c.byref(v), c.c_void_p) for v in values])
                call('cuLaunchKernel', [c.c_void_p] + [c.c_uint] * 7 + [c.c_void_p, c.c_void_p, c.c_void_p], function,
                     (size + 15) // 16, (size + 15) // 16, 1, 16, 16, 1, 0, None, args, None)
                call('cuCtxSynchronize', [])
                call('cuMemcpyDtoH_v2', [c.c_void_p, c.c_uint64, c.c_size_t], actual.ctypes.data, d_out, actual.nbytes)
                delta = float(np.max(np.abs(actual - expected)))
                assert delta < 3e-5, f'Preprocessing mismatch: {delta}'
                print(f'{w}x{h} {channels} channels -> {size}: max absolute error {delta:.8f}')
            finally:
                if d_out.value: call('cuMemFree_v2', [c.c_uint64], d_out)
                call('cuMemFree_v2', [c.c_uint64], d_raw)
        print('Production CUDA preprocessing matches CPU reference on the local GPU.')
    finally:
        if module.value: call('cuModuleUnload', [c.c_void_p], module)
        call('cuCtxDestroy_v2', [c.c_void_p], ctx)


if __name__ == '__main__':
    main()
