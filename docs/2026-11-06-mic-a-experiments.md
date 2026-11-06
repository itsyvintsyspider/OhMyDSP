# Mic A investigation (DAI_CONFIG variants), 2026-11-06

Question: why is the left channel (labelled mic A) exactly zero while the right channel is live? NHLT says 2 mics on one PDM controller
(mic array, offsets -42/+42). Method: change ONE field of pdm[0] in the DAI_CONFIG message of the pipeline blob (the file is read when the
kext starts, so no rebuild), reload the kext, record 6 s in raw mode (A = left, B = right, no join), read the per-channel counters.
Tools: `tools/mica-exp.py`, `tools/mica-run.sh`; raw mode and `chan:` log line need the 5.3b kext build.

| # | Change (everything else as the Linux topology) | Left | Right |
|---|---|---|---|
| 0 | baseline: PDM 0, mic A+B, clk_edge 0, skew 0 | 0 non-zero | live (full resolution, claps reach full scale raw) |
| 1 | clk_edge=1 | 0 | live, unchanged |
| 2 | enable_mic_b=0 (A only) | 0 | live |
| 3 | enable_mic_a=0 (B only) | 0 | **0 (all silent)** |
| 4 | pdm id=1 (A+B on controller 1) | 0 | tiny constant floor (peak 342752 = -76 dBFS, does not follow taps) = unconnected line |
| 5 | enable_mic_b=0 + clk_edge=1 | 0 | live, unchanged |

## Conclusions
- The enable bits are honoured: the one live source is the mic enabled by `enable_mic_a`; it lands on the RIGHT channel of the 2-ch stream
  (so the earlier labels "A=left, B=right" are just FIFO order; the live mic is "mic A" in DAI_CONFIG terms). `enable_mic_b` alone gives nothing.
- `clk_edge` has no effect on which signal is seen (rows 1 and 5), so the firmware either ignores it or the live mic is read the same either way.
  This test therefore cannot tell edge assignment problems apart; it only shows that nothing reachable through these IPC3 fields makes a second mic appear.
- PDM controller 1 is not connected (floating line).
- Best explanation: only one DMIC is actually reachable on this path (the second mic of the "array" is absent, wired elsewhere, or needs the NHLT register
  blob / IPC4 firmware to be enabled). Not proven.
- Not tried: skew, polarity (cannot turn zero into signal), IPC4 + the NHLT 3072-byte register blob (Phase 0 open question 6), two active PDMs.

## Product decision
Keep the join as implemented: both channels carry the one live mic (no -6 dB loss while the other channel is exactly zero). If a second mic ever works,
the existing logic averages automatically. The original blob was restored after the experiments (pristine copy: local/dmic-p10.ipc.orig, git-ignored).
