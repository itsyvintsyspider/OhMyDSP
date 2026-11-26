#!/usr/bin/env python3
"""Extract a stopped PCM1 capture from IORegistry; save WAVs and unnormalised statistics."""
import argparse
import array
import json
import math
import re
import struct
import sys
import wave
from pathlib import Path


def parse_capture(data):
    if len(data) < 32:
        raise ValueError('truncated PCM1 header')
    magic, version, rate, channels, bits, size, lpib, wall_ms = struct.unpack_from('<8I', data)
    if (magic, version, rate, channels, bits) != (0x314d4350, 1, 48000, 2, 32):
        raise ValueError('unexpected PCM1 format')
    if (not size or size % 8 or size >= 1048576 or size != len(data) - 32 or
            size != (lpib & ~7) or lpib >= 1048576 or wall_ms >= 2000):
        raise ValueError('invalid PCM1 length/position/time bounds')
    return dict(rate=rate, channels=channels, bits=bits, bytes=size, frames=size // 8,
                duration_seconds=size / (rate * 8), stop_lpib=lpib, wall_ms=wall_ms), data[32:]


def dbfs(value):
    return 20 * math.log10(value / 2147483648) if value else None


def channel_stats(samples):
    n = len(samples)
    mean = sum(samples) / n
    rms = math.sqrt(sum(x * x for x in samples) / n)
    ac_rms = math.sqrt(sum((x - mean) ** 2 for x in samples) / n)
    peak = max(abs(x) for x in samples)
    return dict(min=min(samples), max=max(samples), mean=mean, rms=rms,
                rms_dbfs=dbfs(rms), ac_rms_dbfs=dbfs(ac_rms), peak_dbfs=dbfs(peak),
                zero_samples=sum(x == 0 for x in samples),
                near_full_scale_samples=sum(abs(x) >= 0.999 * 2147483648 for x in samples),
                guard_samples=sum(x == -1515870811 for x in samples),
                constant=min(samples) == max(samples))


def analyse(pcm):
    samples = array.array('i')
    samples.frombytes(pcm)
    if sys.byteorder != 'little':
        samples.byteswap()
    left, right = samples[::2], samples[1::2]
    stats = [channel_stats(left), channel_stats(right)]
    covariance = sum((a - stats[0]['mean']) * (b - stats[1]['mean']) for a, b in zip(left, right))
    energy = [sum((x - stats[i]['mean']) ** 2 for x in channel) for i, channel in enumerate((left, right))]
    corr = covariance / math.sqrt(energy[0] * energy[1]) if all(energy) else None
    timeline = []
    for start in range(0, len(left), 4800):
        timeline.append(dict(start_seconds=start / 48000,
                             channels=[channel_stats(ch[start:start + 4800]) for ch in (left, right)]))
    return dict(channels=stats, stereo_correlation=corr, windows_100ms=timeline)


def write_wav(path, pcm, width):
    with wave.open(str(path), 'wb') as out:
        out.setnchannels(2)
        out.setsampwidth(width)
        out.setframerate(48000)
        out.writeframes(pcm)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ioreg', type=Path)
    parser.add_argument('output', type=Path, help='new directory, never overwrites')
    args = parser.parse_args()
    try:
        matches = re.findall(r'"ipc4-pcm-capture"\s*=\s*<([0-9a-fA-F\s]+)>', args.ioreg.read_text())
        if len(matches) != 1:
            raise ValueError('expected one PCM capture; inspect PCM export/publish logs')
        data = bytes.fromhex(matches[0])
        metadata, pcm = parse_capture(data)
        report = dict(format=metadata, analysis=analyse(pcm),
                      note='No gain or normalisation. Guard/nonzero counts alone do not prove speech.')
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / 'capture.pcm1').write_bytes(data)
        write_wav(args.output / 'capture-s32.wav', pcm, 4)
        # Convenience playback version: fixed conversion, no automatic gain.
        s16 = b''.join(struct.pack('<h', x[0] >> 16) for x in struct.iter_unpack('<i', pcm))
        write_wav(args.output / 'capture-s16.wav', s16, 2)
        (args.output / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(dict(format=metadata, channels=report['analysis']['channels']), indent=2))
    except (ValueError, OSError, struct.error) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
