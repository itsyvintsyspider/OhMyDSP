# OhMyDSP boot-args

Parsed once at module start (`ohmydsp/src/Config.cpp`); out-of-range values are logged and replaced by the default. The effective
configuration is logged as `OhMyDSP: config: ...` at start.

| Boot-arg | Default | Meaning |
|---|---|---|
| `-ohmydspoff` | off | do not start at all (kill switch; also honoured by Lilu `shouldLoad` in the Lilu build) |
| `-ohmydspdbg` | off | verbose runtime logging (per-2 s stats, per-channel levels, health counters, wrap timestamps); Lilu debug flag |
| `-ohmydspbeta` | off | accepted by Lilu's policy check (reserved, no effect) |
| `ohmydsp_audio=N` | 2 (0 if `ohmydsp_p2` is set) | 0 no audio device, 1 synthetic 1 kHz tone, 2 live microphones |
| `ohmydsp_delay=N` | 30 | seconds between kext start and the first register read (1..600) |
| `ohmydsp_ring=N` | 128 | DMA ring in KiB, multiple of 8 (16..1024); bigger = fewer timestamps, more latency |
| `ohmydsp_sd=N` | 5 | capture stream descriptor (0..6); must be one AppleHDA does not use |
| `ohmydsp_tick=N` | 5 | position poll interval in ms (1..20) |
| `ohmydsp_ts=N` | 0 | 0 timestamp at the exact wrap time, 1 at "now" |
| `ohmydsp_raw=1` | 0 | no mic join: left = mic A, right = mic B (diagnostics) |
| `ohmydsp_gain=N` | 9 | initial input gain in dB (-28..21); the Input volume slider changes it at runtime |
| `ohmydsp_p2` `ohmydsp_p2_keep` `ohmydsp_p3_n` `ohmydsp_p3_dump` `ohmydsp_p4` `ohmydsp_p4_sd` | unset | Phase 2-4 test harness (see `DSPBoot.cpp`); experimental |

Runtime tunables without a reboot: `sudo local/engine-tune tsmode=1 tick_ms=2 gain_db=N raw=1 inject=1|2` (inject = 5.5 fault injection).

Build variants: `make -C ohmydsp` = Lilu plugin (inject with OpenCore, after Lilu); `make -C ohmydsp LILU=0` = standalone (kmutil / /Library/Extensions).
