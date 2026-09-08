"""Compile the production preprocessing kernel to PTX with NVRTC; no GPU required."""
import argparse
import ctypes as c
import os
from pathlib import Path


def main():
    p = argparse.ArgumentParser(); p.add_argument('--nvrtc', required=True)
    p.add_argument('--output', default='preprocess.ptx'); p.add_argument('--arch', default='compute_86'); a = p.parse_args()
    library = Path(a.nvrtc).resolve()
    dll_dir = os.add_dll_directory(str(library.parent)) if os.name == 'nt' else None
    # NVRTC loads its builtins internally with legacy DLL search semantics.
    builtins = [c.CDLL(str(x)) for x in library.parent.glob('nvrtc-builtins*.dll')]
    nvrtc = c.CDLL(str(library))
    program = c.c_void_p()
    nvrtc.nvrtcCreateProgram.argtypes = [c.POINTER(c.c_void_p), c.c_char_p, c.c_char_p, c.c_int, c.c_void_p, c.c_void_p]
    nvrtc.nvrtcCompileProgram.argtypes = [c.c_void_p, c.c_int, c.POINTER(c.c_char_p)]
    nvrtc.nvrtcGetProgramLogSize.argtypes = [c.c_void_p, c.POINTER(c.c_size_t)]
    nvrtc.nvrtcGetProgramLog.argtypes = [c.c_void_p, c.c_void_p]
    nvrtc.nvrtcGetPTXSize.argtypes = [c.c_void_p, c.POINTER(c.c_size_t)]
    nvrtc.nvrtcGetPTX.argtypes = [c.c_void_p, c.c_void_p]
    nvrtc.nvrtcDestroyProgram.argtypes = [c.POINTER(c.c_void_p)]
    source = (Path(__file__).resolve().parents[1] / 'src/preprocess_kernel.cuh').read_bytes()
    assert nvrtc.nvrtcCreateProgram(c.byref(program), source, b'preprocess_kernel.cuh', 0, None, None) == 0
    try:
        options = (c.c_char_p * 2)(f'--gpu-architecture={a.arch}'.encode(), b'--std=c++17')
        result = nvrtc.nvrtcCompileProgram(program, 2, options)
        length = c.c_size_t(); nvrtc.nvrtcGetProgramLogSize(program, c.byref(length))
        log = c.create_string_buffer(length.value); nvrtc.nvrtcGetProgramLog(program, log)
        if result:
            raise RuntimeError(log.value.decode())
        assert nvrtc.nvrtcGetPTXSize(program, c.byref(length)) == 0
        ptx = c.create_string_buffer(length.value); assert nvrtc.nvrtcGetPTX(program, ptx) == 0
        Path(a.output).write_bytes(ptx.value)
        print(f'CUDA kernel compilation passed ({a.arch}): {len(ptx.value)} PTX bytes. This is not a GPU execution test.')
    finally:
        nvrtc.nvrtcDestroyProgram(c.byref(program))
        if dll_dir:
            dll_dir.close()


if __name__ == '__main__':
    main()
