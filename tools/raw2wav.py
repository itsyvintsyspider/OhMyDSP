#!/usr/bin/env python3
"""Convert the kext's raw capture (S32_LE, 2 ch, 48 kHz interleaved) to WAV and print a level summary.

Usage: python3 -I tools/raw2wav.py <capture-raw.bin> <out.wav> [--norm]
--norm also writes <out>-norm.wav scaled so the peak is at -3 dBFS (for listening; levels from the DMICs are low).
Prints per-100 ms peak/RMS per channel (dBFS) as a bar graph so claps/speech are visible, plus the DC offset. Never commit the recordings.
"""
import array, math, sys, wave

RATE, CH = 48000, 2


def dbfs(x):
    return -99.0 if x <= 0 else max(-99.0, 20 * math.log10(x / 2147483648.0))


def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(CH); w.setsampwidth(4); w.setframerate(RATE)
        w.writeframes(samples.tobytes())


def main():
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    if len(a) != 2:
        sys.exit(__doc__)
    raw = open(a[0], "rb").read()
    raw = raw[:len(raw) // (4 * CH) * 4 * CH]
    s = array.array("i"); s.frombytes(raw)
    if sys.byteorder != "little":
        s.byteswap()
    frames = len(s) // CH
    print("%d frames = %.3f s" % (frames, frames / RATE))
    for c in range(CH):
        ch = s[c::CH]
        dc = sum(ch) / len(ch)
        print("ch%d: min %d max %d DC offset %.0f (%.1f dBFS) peak %.1f dBFS" % (c, min(ch), max(ch), dc, dbfs(abs(dc)), dbfs(max(abs(min(ch)), abs(max(ch))))))
    step = RATE // 10
    print("\n 100 ms blocks, RMS (DC removed) per channel; bar = dBFS from -90 (empty) to -20 (full)")
    for i in range(0, frames - step + 1, step):
        row = []
        for c in range(CH):
            ch = s[2 * i + c:2 * (i + step):2]
            m = sum(ch) / len(ch)
            rms = math.sqrt(sum((x - m) ** 2 for x in ch) / len(ch))
            d = dbfs(rms); row.append((d, "#" * int(max(0, min(35, (d + 90) / 70 * 35)))))
        print("%5.1fs  L %6.1f %-35s  R %6.1f %s" % (i / RATE, row[0][0], row[0][1], row[1][0], row[1][1]))
    write(a[1], s)
    print("\nwrote", a[1])
    if "--norm" in sys.argv:
        m = sum(s) / len(s)
        pk = max(abs(x - m) for x in s) or 1
        g = 0.708 * 2147483647 / pk
        n = array.array("i", (int((x - m) * g) for x in s))
        out = a[1].rsplit(".", 1)[0] + "-norm.wav"
        write(out, n); print("wrote %s (DC removed, gain x%.1f)" % (out, g))


if __name__ == "__main__":
    main()
