#!/usr/bin/env python3
"""Offline tests for IPC4 EQ and TDFB configuration messages."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('generator', Path(__file__).with_name('gen-ipc4-messages.py'))
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class NHLTBoundsTests(unittest.TestCase):
    def test_endpoint_limits_and_absolute_offsets(self):
        spec = importlib.util.spec_from_file_location('nhlt', Path(__file__).with_name('extract-nhlt-vendor-blob.py'))
        nhlt = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(nhlt)
        endpoint = bytearray(19 + 4 + 1 + 40 + 4 + 4)
        struct.pack_into('<I', endpoint, 0, len(endpoint))
        endpoint[23] = 1
        struct.pack_into('<I', endpoint, 64, 4)
        table = bytearray(37) + endpoint + endpoint
        table[:4] = b'NHLT'
        struct.pack_into('<I', table, 4, len(table))
        table[36] = 2
        records = nhlt.parse_endpoints(table)
        self.assertEqual(records[1]['formats'][0]['capabilities_offset'], 37 + len(endpoint) + 68)
        for size in (19, 22, 23, 24, 63, 66, 71):
            with self.subTest(size=size):
                struct.pack_into('<I', table, 37, size)
                with self.assertRaisesRegex(nhlt.NHLTError, 'endpoint 0'):
                    nhlt.parse_endpoints(table)


class EqConfigTests(unittest.TestCase):
    def widget(self):
        header = [88, 2, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0]
        # Synthetic signed values distinguish sign extension and word ordering.
        biquad = [-2147483648, 2147483647, -1, 0, 1, -4, 256]
        data = struct.pack('<15I7i', *header, *biquad)
        return {'kcontrols': [{'type': 2, 'priv':
                struct.pack('<8I', 0x34464f53, 0, 88, 0x0301a000, 0, 0, 0, 0) + data}]}

    def test_wire_format_and_preserve_signed_coefficients(self):
        widget = self.widget()
        msg = generator.eq_config_message(widget, 10, 0)
        self.assertEqual((msg['primary'], msg['extension'], msg['payload_bytes']),
                         (0x4400000a, 0x30000058, 88))
        self.assertEqual(struct.pack('<22I', *msg['payload_words']), widget['kcontrols'][0]['priv'][32:])
        self.assertEqual(struct.unpack('<i', struct.pack('<I', msg['payload_words'][-2]))[0], -4)

    def test_invalid_layout_and_response_references(self):
        for index, value in [(1, 0), (1, 1), (2, 0), (3, 1), (7, 1), (8, 1),
                             (9, 0), (9, 2), (10, 2), (11, 1)]:
            with self.subTest(index=index, value=value):
                widget = self.widget()
                blob = bytearray(widget['kcontrols'][0]['priv'])
                struct.pack_into('<I', blob, 32 + index * 4, value)
                widget['kcontrols'][0]['priv'] = bytes(blob)
                with self.assertRaises(ValueError):
                    generator.eq_config_message(widget, 10, 0)

    def test_truncated_header_and_coefficient_data(self):
        for length in (4, 32, 88, 116, 119):
            widget = self.widget()
            widget['kcontrols'][0]['priv'] = widget['kcontrols'][0]['priv'][:length]
            with self.assertRaises(ValueError):
                generator.eq_config_message(widget, 10, 0)

    def test_private_record_size(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'eq.i4pc'
            generator.write_binary([generator.eq_config_message(self.widget(), 10, 0)], path)
            blob = path.read_bytes()
            self.assertEqual(len(blob), 108)
            self.assertEqual(struct.unpack_from('<III', blob, 8), (100, 0x30000058, 0x4400000a))


class TdfbConfigTests(unittest.TestCase):
    def widget(self, size=788):
        data = struct.pack('<I', size) + bytes(size - 4)
        header = struct.pack('<8I', 0x34464f53, 0, size, 0x0301a000, 0, 0, 0, 0)
        return {'kcontrols': [{'type': 2, 'priv': header + data}]}

    def test_wire_header_and_strip_abi(self):
        msg = generator.tdfb_config_message(self.widget(), 20, 0)
        self.assertEqual(msg['primary'], 0x44000014)
        self.assertEqual(msg['extension'], 0x30000314)
        self.assertEqual(msg['payload_bytes'], 788)
        self.assertEqual(msg['payload_words'][0], 788)
        self.assertEqual(len(msg['payload_words']), 197)

    def test_invalid_wrappers(self):
        for offset, value in [(0, 0), (4, 1), (8, 784), (12, 0x02000000), (16, 1), (32, 784)]:
            with self.subTest(offset=offset):
                w = self.widget()
                blob = bytearray(w['kcontrols'][0]['priv'])
                struct.pack_into('<I', blob, offset, value)
                w['kcontrols'][0]['priv'] = bytes(blob)
                with self.assertRaises(ValueError):
                    generator.tdfb_config_message(w, 20, 0)

    def test_truncation_and_oversize(self):
        for size in (787, 4100):
            with self.assertRaises(ValueError):
                generator.tdfb_config_message(self.widget(size), 20, 0)
        w = self.widget()
        w['kcontrols'][0]['priv'] = b'SOF4'
        with self.assertRaises(ValueError):
            generator.tdfb_config_message(w, 20, 0)

    def test_ambiguous_control_or_id(self):
        w = self.widget()
        w['kcontrols'] *= 2
        with self.assertRaises(ValueError):
            generator.tdfb_config_message(w, 20, 0)
        for module_id, instance_id in [(65536, 0), (20, 256)]:
            with self.assertRaises(ValueError):
                generator.tdfb_config_message(self.widget(), module_id, instance_id)


if __name__ == '__main__':
    unittest.main()
