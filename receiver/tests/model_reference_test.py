"""Check the actual sample ONNX model and C++ decoding against independent reference NMS."""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
import numpy as np
import onnx
import onnxruntime as ort
import torch
from PIL import Image
from torchvision.ops import batched_nms
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from validate_gpu import preprocess


def reference(output, width, height, size, class_filter):
    raw = output[0].T
    scores = raw[:, 4:]
    if class_filter:
        masked = np.full_like(scores, -1)
        masked[:, class_filter] = scores[:, class_filter]
        scores = masked
    classes = np.argmax(scores, axis=1); confidence = np.max(scores, axis=1)
    select = confidence >= .45
    raw, classes, confidence = raw[select], classes[select], confidence[select]
    scale = min(size / width, size / height)
    rw, rh = int(np.floor(width * scale + .5)), int(np.floor(height * scale + .5))
    offset = np.array([(size - rw) // 2, (size - rh) // 2], dtype=np.float32)
    xy1 = (raw[:, :2] - raw[:, 2:4] * .5 - offset) / scale
    xy2 = (raw[:, :2] + raw[:, 2:4] * .5 - offset) / scale
    xy1 = np.clip(xy1, [0, 0], [width, height]); xy2 = np.clip(xy2, [0, 0], [width, height])
    boxes = np.concatenate([xy1, xy2], axis=1).astype(np.float32)
    keep = batched_nms(torch.from_numpy(boxes), torch.from_numpy(confidence), torch.from_numpy(classes), .45).numpy()
    return np.column_stack([xy1[keep], xy2[keep] - xy1[keep], confidence[keep], classes[keep]])


def main():
    p = argparse.ArgumentParser(); p.add_argument('--model', required=True); p.add_argument('--image', required=True); p.add_argument('--decoder', required=True); a = p.parse_args()
    model = onnx.load(a.model); onnx.checker.check_model(model)
    session = ort.InferenceSession(a.model, providers=['CPUExecutionProvider'])
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for width, height, size, classes in [(160, 160, 160, []), (320, 240, 320, []), (320, 240, 320, [0])]:
            image = Image.open(a.image).convert('RGB').resize((width, height))
            tensor = preprocess(image.tobytes(), width, height, size)
            output = session.run(None, {session.get_inputs()[0].name: tensor})[0]
            expected_count = (size // 8) ** 2 + (size // 16) ** 2 + (size // 32) ** 2
            assert output.shape == (1, 84, expected_count), output.shape
            assert np.isfinite(output).all()
            config = tmp / 'config.json'; config.write_text(json.dumps(dict(version=1, input_size=size, classes=classes)))
            raw = tmp / 'output.f32'; output.astype('<f4').tofile(raw)
            result = subprocess.run([a.decoder, str(config), str(raw), str(width), str(height), '80', str(expected_count)], check=True, capture_output=True, text=True)
            actual = np.array(json.loads(result.stdout)); expected = reference(output, width, height, size, classes)
            assert len(expected) > 0, 'Reference fixture produced no detections'
            np.testing.assert_allclose(actual, expected, atol=1e-4, rtol=1e-5)
            total += len(expected)
            print(f'{width}x{height} -> input {size}, classes={classes or "all"}: {len(expected)} detections match FP32 reference NMS and coordinate mapping')
    print(f'Sample model and C++ decoding validation passed ({total} matched detections). TensorRT inference is tested separately on deployment hardware.')


if __name__ == '__main__':
    main()
