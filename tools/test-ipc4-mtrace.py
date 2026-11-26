#!/usr/bin/env python3
import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('mtrace', Path(__file__).with_name('decode-ipc4-mtrace.py'))
mtrace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mtrace)


def snapshot(read, write, after=None):
    ring = bytes(i % 256 for i in range(4088))
    page = struct.pack('<II', read, write) + ring
    lines = [f'MTRACE before-cleanup slot=0 core=0 base=000c1000 read-before={read} write-before={write} read-after={read} write-after={write if after is None else after}']
    for offset in range(0, 4096, 32):
        words = struct.unpack_from('<8I', page, offset)
        lines.append(f'MTRACE before-cleanup slot=0 off={offset:04x} data=' + ' '.join(f'{w:08x}' for w in words))
    return '\n'.join(lines), ring


class TraceTests(unittest.TestCase):
    def stream(self, records, flags=0):
        data = struct.pack('<4I', 0x3152544d, 1, flags, 0)
        for seq, (read, write, payload) in enumerate(records):
            padded = (len(payload) + 3) & ~3
            data += struct.pack('<8I', 32 + padded, seq, 3, 0, 0x474f4c00, read, write, len(payload))
            data += payload + bytes(padded - len(payload))
        return data

    def test_stream_wrap_and_partial_line(self):
        data = self.stream([(4086, 2, b'err:'), (2, 6, b'bad\n')])
        summary, records = mtrace.extract_stream(data)
        self.assertEqual(summary['records'], 2)
        self.assertEqual(b''.join(p for _, p in records), b'err:bad\n')

    def test_stream_bounds_gaps_and_flags(self):
        valid = self.stream([(0, 4, b'abc\n')], flags=5)
        summary, _ = mtrace.extract_stream(valid)
        self.assertTrue(summary['possible_drops'])
        self.assertTrue(summary['archive_capacity_hit'])
        bad = [valid[:-1], bytes(16), self.stream([(0, 5, b'abc\n')]),
               self.stream([(0, 4, b'abc\n'), (5, 9, b'def\n')]),
               self.stream([], flags=8), valid + bytes(4)]
        for data in bad:
            with self.assertRaises(ValueError):
                mtrace.extract_stream(data)

    def test_wrap(self):
        text, ring = snapshot(4080, 12)
        self.assertEqual(mtrace.extract(text)[0][2], ring[4080:] + ring[:12])

    def test_empty_and_linear(self):
        self.assertEqual(mtrace.extract(snapshot(4, 4)[0])[0][2], b'')
        text, ring = snapshot(8, 20)
        self.assertEqual(mtrace.extract(text)[0][2], ring[8:20])

    def test_invalid_or_changing_pointers_preserve_raw_only(self):
        for text in (snapshot(0, 4096)[0], snapshot(0, 12, after=20)[0]):
            result = mtrace.extract(text)[0]
            self.assertEqual(len(result[1]), 4096)
            self.assertIsNone(result[2])

    def test_truncated_or_duplicate_rejected(self):
        text = snapshot(0, 12)[0]
        with self.assertRaises(ValueError):
            mtrace.extract(text.rsplit('\n', 1)[0])
        with self.assertRaises(ValueError):
            mtrace.extract(text + '\n' + text.splitlines()[-1])


if __name__ == '__main__':
    unittest.main()
