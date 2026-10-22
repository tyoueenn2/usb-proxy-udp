import json
import os
import shlex
from contextlib import nullcontext
from pathlib import Path
import subprocess
import tempfile
import unittest

class MeasurementTest(unittest.TestCase):
    def test_writer_fields_and_flush(self):
        root = Path(__file__).resolve().parents[1]
        artifact_root = os.environ.get('USB_PROXY_MEASUREMENT_TEST_DIR')
        if artifact_root:
            Path(artifact_root).mkdir(parents=True, exist_ok=True)
            directory_context = nullcontext(tempfile.mkdtemp(prefix='writer-', dir=artifact_root))
        else:
            directory_context = tempfile.TemporaryDirectory()
        with directory_context as d:
            exe, log = Path(d)/'test', Path(d)/'events.jsonl'
            subprocess.run(shlex.split(os.environ.get('CXX', 'g++')) + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-I'+str(root), str(root/'tests/measurement_test.cpp'), '-o', str(exe)], check=True)
            subprocess.run([str(exe), str(log)], check=True)
            events = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual(events[0]['event'], 'log_start')
            self.assertEqual(events[-1]['event'], 'log_end')
            self.assertEqual(events[-1]['dropped'], 0)
            a, b = events[1:3]
            self.assertEqual(a['session'], '18446744073709551615')
            self.assertEqual(a['counts'], [-4, 6])
            self.assertFalse(a['success'])
            self.assertEqual(a['errno'], 5)
            self.assertEqual(a['duration_ns'], 150)
            self.assertTrue(b['success'])
            self.assertEqual(b['duration_ns'], 200)

if __name__ == '__main__': unittest.main()
