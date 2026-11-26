#!/usr/bin/env python3
import importlib.util
import math
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('pcm', Path(__file__).with_name('extract-ipc4-pcm.py'))
pcm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pcm)


class PcmTests(unittest.TestCase):
    def header(self, payload, lpib=None):
        return struct.pack('<8I', 0x314d4350, 1, 48000, 2, 32, len(payload),
                           len(payload) if lpib is None else lpib, 1000) + payload

    def test_stereo_sine_and_no_normalisation(self):
        data = b''.join(struct.pack('<ii', int(2**29 * math.sin(2*math.pi*i/48)),
                                   int(2**28 * math.sin(2*math.pi*i/48))) for i in range(4800))
        metadata, parsed = pcm.parse_capture(self.header(data))
        self.assertEqual(parsed, data)
        self.assertEqual(metadata['duration_seconds'], 0.1)
        stats = pcm.analyse(parsed)
        self.assertAlmostEqual(stats['channels'][0]['rms_dbfs'], -15.0515, places=3)
        self.assertAlmostEqual(stats['channels'][1]['rms_dbfs'], -21.0721, places=3)
        self.assertAlmostEqual(stats['stereo_correlation'], 1.0)

    def test_silence_and_guard_are_not_audio(self):
        stats = pcm.analyse(bytes(80))
        self.assertIsNone(stats['channels'][0]['rms_dbfs'])
        self.assertTrue(stats['channels'][0]['constant'])
        self.assertIsNone(stats['stereo_correlation'])
        guard = pcm.analyse(bytes([0xa5])*80)
        self.assertEqual(guard['channels'][0]['guard_samples'], 10)
        self.assertIsNone(guard['channels'][0]['ac_rms_dbfs'])

    def test_length_format_and_bounds(self):
        valid = self.header(bytes(80))
        for data in [valid[:-1], valid[:20], self.header(bytes(79)), self.header(b''),
                     self.header(bytes(80), 88)]:
            with self.assertRaises(ValueError):
                pcm.parse_capture(data)
        for index, value in [(0, 0), (1, 2), (2, 44100), (3, 1), (4, 16), (7, 2000)]:
            data = bytearray(valid)
            struct.pack_into('<I', data, index*4, value)
            with self.assertRaises(ValueError):
                pcm.parse_capture(data)

    def test_full_scale_signed_extremes(self):
        stats = pcm.analyse(struct.pack('<4i', -2147483648, 2147483647, 2147483647, -2147483648))
        self.assertEqual(stats['channels'][0]['near_full_scale_samples'], 2)
        self.assertEqual(stats['channels'][0]['peak_dbfs'], 0)


if __name__ == '__main__':
    unittest.main()
