"""The C sound encoder and the plain-Python LPC fit give exactly what the numpy versions gave."""
import math, os, random, sys, time, unittest
from unittest import mock
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.dirname(__file__))
import sndconv
try:
    import ref_dsp_numpy as ref
except ImportError:
    ref = None


def signals():
    r = random.Random(29)
    out = []
    for n in (1, 13, 14, 15, 1000, 44100):
        out.append([r.randint(-32768, 32767) for _ in range(n)])
        out.append([int(12000 * math.sin(i * 0.05)) for i in range(n)])
        out.append([0] * n)
        out.append([32767 if (i // 37) % 2 else -32768 for i in range(n)])
    for _ in range(26):
        n = r.randint(20, 3000); a = r.randint(1, 30000)
        out.append([int(a * math.sin(i * r.uniform(0.001, 1.5)) + r.randint(-200, 200)) for i in range(n)])
    return [[max(-32768, min(32767, v)) for v in s] for s in out]


@unittest.skipIf(ref is None, 'numpy reference unavailable')
class MatchesNumpyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.enc = sndconv.DspEncoder()

    @classmethod
    def tearDownClass(cls):
        cls.enc.close()

    def test_encode_matches_numpy_reference(self):
        for k, s in enumerate(signals()):
            co = sndconv.coefs_for(s)
            self.assertEqual(self.enc.encode(s, co), ref.dsp_encode(s, co), 'signal %d' % k)

    def test_lpc2_matches_numpy(self):
        cases = signals() + [[5] * 100, [3, -3] * 50, [1, 2, 4, 8, 16, 32, 64, 128], [7, 0, 0, 0, 0]]
        for k, s in enumerate(cases):
            self.assertEqual(sndconv.lpc2(s), ref.lpc2(s), 'case %d' % k)

    def test_faster_than_numpy(self):
        s = [int(9000 * math.sin(i * 0.03)) for i in range(32000 * 10)]
        co = sndconv.coefs_for(s)
        t = time.time(); self.enc.encode(s, co); c = time.time() - t
        t = time.time(); ref.dsp_encode(s[:32000], co); n = (time.time() - t) * 10
        self.assertLess(c, n)


class CompilerTest(unittest.TestCase):
    def test_no_compiler_message(self):
        with mock.patch.dict(os.environ, {'PATH': '/nonexistent', 'CC': ''}):
            with self.assertRaises(ValueError) as c:
                sndconv.DspEncoder(cc='/nonexistent/cc')
        self.assertEqual(str(c.exception), 'no C compiler found to build the sound encoder')

    def test_broken_first_compiler_falls_through(self):
        # SteamOS: `cc` can be a clang with no C headers. A broken first candidate must not stop the build.
        import shutil, tempfile
        real = next((shutil.which(c) for c in ('gcc', 'clang', 'cc') if shutil.which(c)), None)
        if not real:
            self.skipTest('no C compiler')
        d = tempfile.mkdtemp()
        broken = os.path.join(d, 'cc')
        with open(broken, 'w') as f:
            f.write('#!/bin/sh\necho "fatal error: stdio.h: No such file" >&2\nexit 1\n')
        os.chmod(broken, 0o755)
        with mock.patch.dict(os.environ, {'PATH': d + os.pathsep + os.path.dirname(real), 'CC': ''}):
            enc = sndconv.DspEncoder()
        enc.p.kill()


if __name__ == '__main__':
    unittest.main()
