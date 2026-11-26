#!/usr/bin/env python3
"""Recover raw IPC4 mtrace pages from ohmydsp-ioreg; no firmware dictionary assumed."""
import argparse
import json
import re
import struct
from pathlib import Path

ROWS = re.compile(r'MTRACE ([\w-]+) slot=(\d+) off=([0-9a-f]{4}) data=([0-9a-f]{8}(?: [0-9a-f]{8}){7})')
META = re.compile(r'MTRACE ([\w-]+) slot=(\d+) core=(\d+) base=([0-9a-f]+) read-before=(\d+) write-before=(\d+) read-after=(\d+) write-after=(\d+)')
STREAM = re.compile(r'"ipc4-mtrace-stream"\s*=\s*<([0-9a-fA-F\s]+)>')


def extract_stream(data):
    """Validate the bounded MTR1 archive before treating any payload as log text."""
    if len(data) < 16 or len(data) > 65536:
        raise ValueError('invalid MTR1 archive size')
    magic, version, flags, full = struct.unpack_from('<4I', data)
    if magic != 0x3152544d or version != 1 or flags & ~7:
        raise ValueError('invalid MTR1 header/version/flags')
    records, end_by_slot, pos = [], {}, 16
    while pos < len(data):
        if pos + 32 > len(data):
            raise ValueError('truncated MTR1 record header')
        size, seq, phase, slot, kind, read, write, count = struct.unpack_from('<8I', data, pos)
        if (seq != len(records) or phase > 6 or slot >= 15 or kind & 0xffffff00 != 0x474f4c00
                or read >= 4088 or write >= 4088 or not 0 < count < 4088
                or count != (write - read) % 4088 or size != 32 + ((count + 3) & ~3)
                or pos + size > len(data)):
            raise ValueError(f'invalid MTR1 record {seq}')
        if slot in end_by_slot and read != end_by_slot[slot]:
            raise ValueError(f'discontinuous MTR1 slot {slot} at record {seq}')
        end_by_slot[slot] = write
        payload = data[pos + 32:pos + 32 + count]
        records.append((dict(sequence=seq, phase=phase, slot=slot, core=kind & 0xff,
                             read=read, write=write, bytes=count), payload))
        pos += size
    return dict(flags=flags, full_ring_observations=full, records=len(records),
                possible_drops=bool(flags & 1), invalid_metadata=bool(flags & 2),
                archive_capacity_hit=bool(flags & 4)), records


def extract(text):
    pages, metadata = {}, {}
    for m in META.finditer(text):
        key = (m[1], int(m[2]))
        if key in metadata:
            raise ValueError(f'duplicate snapshot metadata: {key}')
        metadata[key] = dict(core=int(m[3]), base=int(m[4], 16),
                             read_before=int(m[5]), write_before=int(m[6]),
                             read_after=int(m[7]), write_after=int(m[8]))
    for m in ROWS.finditer(text):
        key, offset = (m[1], int(m[2])), int(m[3], 16)
        rows = pages.setdefault(key, {})
        if offset % 32 or offset >= 4096 or offset in rows:
            raise ValueError(f'invalid/duplicate row: {key} offset={offset}')
        rows[offset] = struct.pack('<8I', *(int(w, 16) for w in m[4].split()))
    result = []
    for key in sorted(pages.keys() | metadata.keys()):
        rows = pages.get(key, {})
        if set(rows) != set(range(0, 4096, 32)) or key not in metadata:
            raise ValueError(f'incomplete snapshot: {key} ({len(rows)}/128 rows)')
        page = b''.join(rows[o] for o in range(0, 4096, 32))
        meta = metadata[key]
        read, write = struct.unpack_from('<II', page)
        valid = all(0 <= p < 4088 for p in (read, write, meta['read_before'],
                    meta['write_before'], meta['read_after'], meta['write_after']))
        stable = (read == meta['read_before'] == meta['read_after'] and
                  write == meta['write_before'] == meta['write_after'])
        ring = page[8:]
        pending = None
        if valid and stable:
            pending = ring[read:write] if write >= read else ring[read:] + ring[:write]
        result.append((key, page, pending, dict(meta, pointers_valid=valid,
                      pointers_stable=stable, pending_bytes=None if pending is None else len(pending))))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ioreg', type=Path)
    parser.add_argument('output', type=Path, help='new output directory (never overwrites)')
    args = parser.parse_args()
    try:
        text = args.ioreg.read_text()
        result = extract(text)
        streams = STREAM.findall(text)
        if len(streams) > 1:
            raise ValueError('multiple mtrace stream properties; select one probe')
        raw_stream = bytes.fromhex(streams[0]) if streams else None
        stream = extract_stream(raw_stream) if raw_stream is not None else None
        if not result and stream is None:
            parser.error('no mtrace snapshots found; inspect enable reply and slot descriptors')
        args.output.mkdir(parents=True, exist_ok=False)
        if stream is not None:
            stream_summary, records = stream
            (args.output / 'stream.bin').write_bytes(raw_stream)
            (args.output / 'stream-records.json').write_text(
                json.dumps([meta for meta, _ in records], indent=2) + '\n')
            (args.output / 'stream-summary.json').write_text(json.dumps(stream_summary, indent=2) + '\n')
            for slot in sorted({meta['slot'] for meta, _ in records}):
                payload = b''.join(payload for meta, payload in records if meta['slot'] == slot)
                (args.output / f'stream-slot{slot}.bin').write_bytes(payload)
                (args.output / f'stream-slot{slot}.log').write_text(payload.decode('utf-8', errors='replace'))
            print('stream:', json.dumps(stream_summary, indent=2))
        summary = []
        for (stage, slot), page, pending, meta in result:
            stem = f'{stage}-slot{slot}'
            (args.output / f'{stem}.bin').write_bytes(page)
            if pending is not None:
                (args.output / f'{stem}.pending.bin').write_bytes(pending)
                # Best-effort readable strings, NOT a decoder for encoded Zephyr logs.
                strings = re.findall(rb'[\x20-\x7e\t]{5,}', pending)
                (args.output / f'{stem}.strings.txt').write_text(
                    '\n'.join(s.decode('ascii') for s in strings) + '\n')
            summary.append(dict(stage=stage, slot=slot, **meta))
        (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
    except (ValueError, OSError) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
