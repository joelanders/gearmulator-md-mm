"""Independent signals check the measurement oracles without a plugin or ROM."""
import tempfile
from pathlib import Path
import unittest

import numpy as np
from scipy.io import wavfile

from analyze import input_delay, note_onsets, performance, wave


class MeasurementTests(unittest.TestCase):
    def test_early_and_missing_note(self):
        signal = np.zeros(20 * 8000)
        signal[80000 - 24] = .1
        onset = note_onsets(signal, 8000, [{'sample': 80000}])[0]
        self.assertEqual(onset['thresholds']['0.001']['delay_samples'], -24)
        self.assertTrue(onset['thresholds']['0.001']['prequiet'])
        signal[:] = 0
        self.assertIsNone(note_onsets(signal, 8000, [{'sample': 80000}])[0]
                          ['thresholds']['0.001']['delay_samples'])

    def test_input_delay_sign_and_polarity(self):
        source = np.random.default_rng(781).uniform(-.1, .1, (20 * 8000, 2)).astype(np.float32)
        with tempfile.TemporaryDirectory() as tmp:
            case = Path(tmp)
            wavfile.write(case / 'capture.input.wav', 8000, source)
            for delay in (-29, 53):
                output = -np.roll(source, delay, axis=0)
                result = input_delay(case, output, 8000)
                self.assertEqual([w['delay_samples'] for w in result['windows']], [delay] * 4)
                self.assertTrue(all(w['correlation'] < -.999 for w in result['windows']))
            self.assertTrue(all(w['delay_samples'] is None
                                for w in input_delay(case, np.zeros_like(source), 8000)['windows']))

    def test_wav_scaling_and_finite_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'test.wav'
            wavfile.write(path, 8000, np.array([[16384, -16384]], dtype=np.int16))
            self.assertEqual(wave(path)[1].tolist(), [[.5, -.5]])
            wavfile.write(path, 8000, np.array([[np.nan, 0]], dtype=np.float32))
            with self.assertRaises(ValueError):
                wave(path)

    def test_callback_budget(self):
        blocks = [dict(sample=12 * 48000 + 128 * n, count=128, render_ms=t, late_ms=0)
                  for n, t in enumerate((1, 3, 2))]
        result = performance(blocks, 48000, [])
        self.assertEqual(result['warm_p50_ms'], 2)
        self.assertEqual(result['warm_callbacks'], 3)
        self.assertEqual(result['warm_over_budget'], 1)


if __name__ == '__main__':
    unittest.main()
