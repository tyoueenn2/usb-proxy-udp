"""Export a YOLO11 detection model plus the receiver's verified I/O manifest."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--weights', default='yolo11n.pt')
    p.add_argument('--size', type=int, default=320)
    p.add_argument('--fixed', action='store_true')
    a = p.parse_args()
    if a.size < 32 or a.size > 1024 or a.size % 32:
        p.error('Size must be 32..1024 in multiples of 32')
    from ultralytics import YOLO
    import onnx
    model = YOLO(a.weights)
    if model.task != 'detect':
        raise ValueError('Only YOLO11 detection weights are supported')
    output = Path(model.export(format='onnx', imgsz=a.size, batch=1, dynamic=not a.fixed,
                               simplify=False, opset=17, half=False, nms=False, device='cpu'))
    graph = onnx.load(output)
    inp, out = graph.graph.input, graph.graph.output
    if len(inp) != 1 or len(out) != 1 or len(inp[0].type.tensor_type.shape.dim) != 4 or len(out[0].type.tensor_type.shape.dim) != 3:
        raise ValueError('Unsupported exported graph layout')
    names = [model.names[i] for i in range(len(model.names))]
    manifest = dict(version=1, task='detect', layout='NCHW', output='raw_yolo11', names=names,
                    sha256=hashlib.sha256(output.read_bytes()).hexdigest(), input_size=a.size, dynamic=not a.fixed)
    Path(str(output) + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Exported {output} and {output}.json. TensorRT engines will be built on the receiver GPU.')


if __name__ == '__main__':
    main()
