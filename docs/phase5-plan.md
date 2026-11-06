# Phase 5 plan — expose the DMIC stream as a macOS audio input device ("Internal Microphone")

Date: 2026-11-06. Status: **PLAN ONLY. No Phase 5 code written, nothing enabled.** Hardware-touching steps need their own go-ahead (CLAUDE.md).
Builds on: Phase 4 (`2026-11-06-phase4-step5.md`): 2 s capture at exactly 384.1 B/ms, mic B live, mic A silent.

## Goal / done-definition
System Settings > Sound > Input lists **Internal Microphone (Built-in)**; selecting it shows a moving input level meter while the user talks; Voice Memos / QuickTime / FaceTime record
intelligible speech from it; output, headset jack and sleep/wake keep working; AppleALC (alcid=31) and SimpleHDA are untouched.
Not in Phase 5: public release and renaming to OhMyDSP (done at the end), mic A fix (side experiment, not a blocker), 16 kHz pipeline 11.

## Facts checked on this machine (2026-11-06)
- macOS is **15.8.2 (24H232)**, not Tahoe 26 as CLAUDE.md still says. Kext Signing disabled, SIP custom (csr-active-config 0x0a03), AppleALC 1.9.9 + Lilu 1.7.3 loaded.
- **IOAudioFamily 600.2 is loaded** (AppleHDA and AppleHDAController depend on it) and its headers are in the SDK (`Kernel.framework/Headers/IOKit/audio`). So the kext route works today.
- AudioDriverKit is **not** an option now: no DriverKit SDK/Xcode installed, a dext needs entitlements and system-extension approval, and OpenCore cannot inject a dext.
  Keep it as the fallback if a future macOS removes IOAudioFamily (risk R1).

## Architecture
1. **Same kext, new classes** (no second bundle, no AppleHDA/AppleALC hooks): `com_dmicmac_DMICProbe` (the existing PCI-matched provider, own `IOMatchCategory`) owns a
   `DSPDevice` (the Phase 2-4 code, refactored) and attaches an `IOAudioDevice` subclass with one input `IOAudioEngine`. Info.plist gains `com.apple.iokit.IOAudioFamily`.
2. **Zero-copy ring.** The engine's sample buffer *is* the 1 MiB DMA buffer from Phase 4 (cyclic, 131 072 frames = 2.73 s, S32_LE stereo 48 kHz, IOVA < 4 GB). The HDA DMA writes
   straight into it; nothing is copied until CoreAudio asks. A cyclic ring bounds any stray DMA to the buffer by construction.
3. **Position without interrupts.** An `IOTimerEventSource` (5 ms) on the engine's work loop reads SD LPIB, detects wraps and calls `takeTimeStamp()` on each wrap, as IOAudioFamily
   requires; `getCurrentSampleFrame()` returns LPIB/8. INTCTL, SD interrupt enables and DPLBASE stay untouched (shared with AppleHDA).
4. **Channels.** The stream is declared 2 ch S32; custom `convertInputSamples()` converts to Float32 and maps the live channel (mic B) to both outputs (later: average of A and B once
   mic A works). Software gain lives there at first (observed speech RMS is -38..-41 dBFS, far below what apps expect).
5. **Gain control.** `IOAudioLevelControl` ("Input volume" slider in the screenshot). First a software gain; in 5.4 it drives the DSP volume component (`Dmic0`, range 0..+20 dB)
   via `COMP_SET_VALUE`, which gives real analog-equivalent headroom instead of amplified noise.
6. **DSP lifetime = on demand.** `performAudioEngineStart()` powers the DSP, loads firmware, replays the 19 pipeline messages, sets up SD5 (tag 6) + PCM_PARAMS, starts DMA, TRIG_START.
   `performAudioEngineStop()` does the Phase 4 stop order and the Phase 2 teardown after a short idle delay (default 5 s, so quick stop/start does not re-boot). Boot latency is
   unmeasured; 5.2 logs it. Boot-arg `dmic_keep=1` keeps the DSP up (experiment only).
7. **Locking.** One gate (the engine work loop) serialises all register and IPC access; IPC stays polled. Every wait keeps its bounded timeout from Phases 2-4.
8. **Power.** The provider joins the power tree (`PMinit`/`joinPMtree`/`registerPowerDriver`, 2 states). Before the PCI function sleeps: stop engine, stop DMA, TRIG_STOP/PCM_FREE,
   DSP power-down, restore PGCTL/CGCTL/EM2. After wake: nothing runs until the next engine start (AppleHDAController resets the controller on wake; our state is rebuilt, never assumed).
9. **Health check** in the timer: LPIB must advance; GCTL.CRST, ADSPCS and SD5 CTL must still match what we set. On any mismatch (e.g. AppleHDAController reset the controller under us)
   stop the stream, tear down, and report an I/O error to CoreAudio (`stopAudioEngine`), never keep writing registers.
10. **Hard rules stay:** no AppleHDA/AppleALC/AppleHDAController changes, no CORB/RIRB/codec verbs, SD5 only, PPCTL single-bit RMW, log everything, dated note per step, firmware loaded
    from /Library and never committed, manual `kmutil load` first, OpenCore-USB injection for boots, kill switch `dmic_off=1`.

## Step ladder (each = one boot or less; log + dated note before the next)
| Step | What | Hardware writes | Pass criteria |
|---|---|---|---|
| 5.0 | **Offline.** Read IOAudioFamily headers/AppleHDA behaviour: engine/stream/control classes, `takeTimeStamp` contract, device naming/terminal type ("Built-in", "Internal Microphone"), Info.plist libs. `kmutil libraries` check for the symbols we use. | none | Written API sketch; 0 unresolved symbols in a compile-only build |
| 5.1 | **Audio device with a synthetic source.** Register device+engine+stream backed by a timer-generated 440 Hz tone (DSP never started, no DMA, no register access). | none | "Internal Microphone" appears in Sound > Input; meter follows the tone; Voice Memos records the tone; output unaffected |
| 5.2 | **Refactor** Phase 2-4 `bootDSP` into `DSPDevice` (`powerUp`, `loadPipeline`, `startStream`, `stopStream`, `teardown`) with identical register behaviour and boot timing logs. Regression: reproduce the 4.5 capture through the new API. | same set as 4.5 | 4.5 results reproduced: 19/19, 384 B/ms, clean teardown, ADSPCS 0x00000f0f |
| 5.3 | **Connect:** engine start/stop drives `DSPDevice`; ring = DMA buffer; timer + timestamps; software gain; mic B duplicated. Test: open Sound pane, talk, watch the meter; 30 s record in Voice Memos; start/stop 10x. | as 4.5, repeated | Meter moves with speech; recording intelligible; no leaks/stuck RUN; audio output still fine |
| 5.4 | **Gain via DSP volume** (SET_VALUE from the slider), mono/stereo mapping decision, latency/safety offset tuning. | + 2-3 IPC per slider change | Slider changes level audibly; no clipping at +20 dB speech |
| 5.5 | **Power:** idle teardown, sleep/wake (while idle and while recording), kext stop with engine active, health-check fault injection (unplug nothing; force-stop via boot-arg test hook). | teardown paths | 5 sleep/wake cycles clean; `kmutil unload` clean; no panic |
| 5.6 | **Hardening:** 24 h normal use on the OpenCore-USB config, headset-jack and output checks, AppleALC/SimpleHDA coexistence notes. Then decide: inject into internal EFI, rename to OhMyDSP, release docs. | none new | No regressions; user signs off |

**Abort rules (every live step):** any IPC error/timeout, panic magic, SD error flags, LPIB stall > 100 ms after TRIG_START, health-check mismatch, or engine start taking > 5 s ->
stop DMA (RUN clear confirmed) before touching buffers, power down, return an error to CoreAudio. If RUN cannot be cleared, leak the buffers (as in Phase 4).

## Risks / unknowns
- **R1 IOAudioFamily is deprecated** and may vanish in a later macOS. Fine on 15.8; AudioDriverKit is the long-term path (needs Xcode + entitlements, a separate project).
- **R2 Device naming/UI:** the "Internal Microphone / Built-in" label is derived from stream terminal type + transport type; exact keys are settled in 5.0/5.1. The screenshot is the
  target but the exact wording is not guaranteed.
- **R3 Boot latency** of the DSP (firmware load + 19 IPC) is unmeasured; if > ~2 s, apps may time out or users see a delayed meter -> keep-alive window or pre-boot at login.
- **R4 AppleHDAController owns the controller:** a controller reset/D3 transition while our stream runs would kill it; covered by the health check and the sleep ordering, not by hope.
- **R5 Mic A is silent:** mono-in-stereo for now. Side experiment (needs a DAI_CONFIG change: `clk_edge`, `skew`; one variable per boot) can run in parallel after 5.3.
- **R6 CoreAudio expectations:** timestamp discipline (one `takeTimeStamp` per wrap), sample-rate/format advertisement, and `IOAudioEngine` start/stop races are classic IOAudioFamily bug sources; 5.1 proves them without hardware first.
- **R7 Privacy:** the macOS mic indicator and permission prompts will work as for any input device; the kext never stores audio (the Phase 4 raw-dump path stays off by default).

## Approvals needed
1. This plan. 2. 5.0 (offline) and 5.1 (no hardware) can be done back to back after approval. 3. Each hardware step separately (5.2 ... 5.5).
4. Optional: update CLAUDE.md (macOS 15.8.2 not Tahoe; Arch is gone) — not touched without your say-so.

## Progress log
- 2026-11-06: plan written; nothing implemented.
- 2026-11-06: **5.0 done** (`2026-11-06-phase5-step0.md`). **5.1 attempt 1**: kext was not loaded: OpenCore cannot inject a kext that depends on IOAudioFamily (System KC). Switched to manual loading via `tools/load-kext.sh` (`kmutil load -p`), OC entry disabled on the USB.
- 2026-11-06: **5.1 PASS** (`2026-11-06-phase5-step1.md`). Synthetic-tone device works end to end in CoreAudio (471 IOProc calls, tone at the expected level, no overloads). Root cause of the long 'no callbacks' hunt: `convertInputSamples` not overridden. Next: 5.2, awaiting go-ahead (hardware writes).
- 2026-11-06: **5.2 PASS** (`2026-11-06-phase5-step2.md`). `bootDSP` refactored into `DSPDevice`; dry run and live 4.5 regression identical (507/507 writes in the same order, 19/19, 383.9 B/ms, ADSPCS 0x00000f0f). Next: 5.3, awaiting go-ahead.
- 2026-11-06: **5.3 PASS** (`2026-11-06-phase5-step3.md`). Live DMIC input in CoreAudio: meter moves, Voice Memos records, start latency about 140 ms, 11/11 clean start/stop cycles, coexists with AppleHDA output. Default gain lowered to +7 dB (source only). Mic A still silent. Next: 5.4 (DSP gain, mono/stereo, latency) on go-ahead.
- 2026-11-06: **Mic A investigation closed** (`2026-11-06-mic-a-experiments.md`): 6 DAI_CONFIG variants; only one mic reachable on PDM0 (enable_mic_a), clk_edge no effect, PDM1 unconnected. Join stays. Next: 5.4.
- 2026-11-06: **5.5 PASS** (`2026-11-06-phase5-step5.md`). Recovery (inject=1/2), 132 s run, idle sleep/wake and sleep during recording pass. One unreproduced post-sleep ROM failure (stuck at 0x11) documented, read-only ROMDIAG added; unload-during-recording and further sleep/wake cycles pass; `failStarts` leak fixed.
- 2026-11-06: **Phase 6.1a PASS** (`2026-11-06-phase6-step1a.md`): core kext without IOAudioFamily + root user client; ring and status mapped into userspace, 5 s capture exact, recovery works. Next: 6.1c daemon + AudioServerPlugIn.
