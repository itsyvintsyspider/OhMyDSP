# Phase 4 plan — host DMA capture of DMIC samples (raw dump, then WAV)

Date: 2026-11-06. Status: **PLAN ONLY. No Phase 4 code written, nothing enabled.** Every step that writes hardware needs its own go-ahead (CLAUDE.md).
Builds on: `bringup-recipe.md` §8, §11, §12; Phase 3 done (`2026-11-06-phase3-step4.md`: 19/19 IPC messages incl. PIPE_COMPLETE, error=0).

## Goal / done-definition
With the Phase 3 pipeline built, program one HDA **capture** stream descriptor, send `PCM_PARAMS`, start DMA, send `TRIG_START`, capture ~2 s of DMIC audio into a
host buffer, stop everything in the safe order, write the samples to a file, convert to WAV on the Mac and confirm real microphone audio (clap/speech test).
Not in Phase 4: interrupts, ring buffer with wrap, an audio device (Phase 5), anything persistent across boots.

## Design decisions
1. **Descriptor choice.** GCAP 0x9701 = 7 input + 9 output streams. AppleHDA uses input tags 1 and 2 (Phase 1). Candidate: **input SD5 (index 5, tag 6)**; `dmic_p4_sd=N`
   overrides. Before any write the kext checks SD5 is idle (CTL RUN=0, SRST state sane, tag field not equal to 1/2). CLAUDE.md: never touch a stream AppleHDA uses.
2. **Linear one-shot buffer, no wrap.** Buffer 1 MiB (> 2.7 s at 48 kHz x 2 ch x 4 B = 384 kB/s... exact format taken from the topology in 4.0). BDL = 4 KiB entries,
   `ioc=0`, stream interrupts disabled. We stop the DMA by time (2 s) long before CBL is reached, so it never wraps over data.
3. **Polling only**, as before: INTCTL, stream interrupt enables, ADSPIC and HIPCCTL stay untouched. Progress is read from SD LPIB; DSP replies are polled.
4. **Memory safety is the main new hazard.** For the first time the hardware writes into host RAM. Buffer and BDL come from `IODMACommand` (IOVA, as in the firmware
   download, IOVA above 4 GB worked). Whether the IOMMU actually confines a stray DMA is unverified, so: (a) a guard pattern is written before the run and checked after,
   (b) the buffer is freed only after SD RUN is confirmed clear **and** SRST asserted. If RUN cannot be cleared, the buffer is deliberately leaked and logged.
5. **Stop order (reverse of start):** `TRIG_STOP` (IPC) -> clear SD RUN and poll -> SD reset -> `PCM_FREE` -> clear PPCTL decouple bit -> core reset / power-down (Phase 2
   teardown) -> only then copy out and free the buffer. DMA must never outlive its buffer or the DSP.
6. **Output is a raw file, not ioreg.** ~768 kB is too large for a registry property. The kext writes `/Library/Application Support/dmic-macos/capture-raw.bin` (root-only, mode 0600)
   via `vn_rdwr` after teardown; a tool in `tools/` wraps it into a WAV. Recorded audio is private: never committed, delete after review.
7. **Start order follows Linux** (`pcm.c:386`, IPC3 has no `ipc_first_on_start`): DMA RUN first, then `TRIG_START`.
8. **One variable per boot**, gated by one boot-arg `dmic_p4=L` (default 0 = off; needs `dmic_p2=1 dmic_p3_n=19`; `dmic_off=1` still kills everything).

## Step ladder (each = one reboot, a log, and a dated note before the next)
| Step | `dmic_p4` | What | Hardware writes | Pass criteria |
|---|---|---|---|---|
| 4.0 | - | **Offline only.** Read `ipc3-pcm.c`, `intel/hda-pcm.c`, `hda-stream.c`; settle: PCM_PARAMS layout and size, page-table format (does HDA IPC3 need one?), format word for 48k/2ch/frame_fmt from the topology, `posn_offset`, whether the DPIB position buffer is needed, `no_stream_position`. Assert struct sizes with a compile check. | none | Written spec at the end of this file, reviewed by the user |
| 4.1 | 1 | Read-only precheck: dump SD0-SD6 CTL/STS/FORMAT/BDL, PPCTL, INTCTL while AppleHDA is running; allocate + map buffer and BDL, log IOVAs, free them. | none (IOMMU mappings only) | Chosen SD idle; buffer mapped; guard pattern readable |
| 4.2 | 2 | Stream setup, **no RUN**: set PPCTL decouple bit, SD reset, program tag/CBL/LVI/FORMAT/BDL, send `PCM_PARAMS`, then `PCM_FREE`, undo, teardown. | SD5 regs, 1 PPCTL bit, 2 IPC | PCM_PARAMS error=0, `posn_offset` sane, state restored |
| 4.3 | 3 | Add DMA RUN for 100 ms with **no** TRIG_START, then stop. | + SD RUN | Clean stop, no SD error flags (FIFOE/DESE), LPIB logged, guard intact |
| 4.4 | 4 | Add `TRIG_START`, poll LPIB 500 ms, `TRIG_STOP`, stop. Log sample statistics only (min/max/non-zero count), no file. | + TRIG_START/STOP | LPIB advances ~ rate x frame size, reply error=0, guard intact |
| 4.5 | 5 | Same with a 2 s window; write the raw file; make the WAV; clap/speak during the window. | + file write | Audible, correct rate/pitch, no gaps |

**Abort rules (every step):** reply.error != 0, IPC timeout (500 ms), panic magic in the mailbox, SD error flags set, LPIB not moving 100 ms after start, guard pattern
damaged, or time cap exceeded -> `TRIG_STOP` if started, stop DMA (design decision 5), log raw mailbox + SD registers + ROM status, power down, stop. Never retry a failed message.

## Risks / unknowns
- **PCM_PARAMS bytes are inferred** until 4.0; a wrong page table or format word can make the DSP write somewhere unintended (hence the guard and IOMMU mapping) or fail the reply.
- **PPCTL is shared with AppleHDA.** We set/clear only our own bit with a single read-modify-write; a race with AppleHDA's own RMW is unlikely but not excluded.
- **No samples / all zeros** would mean the PDM pad or clock path needs something Linux does implicitly (DMIC pad mux, NHLT blob, DAI_CONFIG ordering). That is a debugging
  branch, not a reason to touch anything beyond the DSP/DMIC path.
- **AppleHDA coexistence** (AppleALC alcid=31, SimpleHDA): different descriptor, tag and DMA buffer, no shared global state except PPCTL/INTCTL as above. Check audio output and the
  headset jack after every live step.
- **Hang risk** is host-side (a stuck RUN bit). Mitigation: bounded polls everywhere and the leak-on-failure rule.
- Longer DSP-on time again (about 3 s); sleep/wake during the window is not handled and not expected.

## Approvals needed
1. This plan. 2. 4.0 offline spec review. 3. Each live step separately (4.1 ... 4.5). 4. Writing the raw capture file in 4.5.

## Step 4.0 spec (offline reading done 2026-11-06; sources: `ipc3-pcm.c`, `sof-utils.c`, `pcm.c`, `intel/hda-pcm.c`, `intel/hda-stream.c`, `intel/hda.c`)

**Capture parameters.** All pipeline-10 components are `s32le` (DAI, EQ, volume), 48 kHz, 2 ch, so: `frame_fmt = SOF_IPC_FRAME_S32_LE (2)`, valid bytes 4,
container bytes 4, interleaved, 8 B/frame = 384 kB/s. 2 s = 768 000 B. Buffer 1 MiB (256 pages), period 8192 B (1024 frames, 21.3 ms).

**Stream descriptor = input SD5, index 5, tag 6.** The tag must equal `index + 1` (Linux convention, `hda_dsp_stream_init`): the DSP locates the host DMA channel
from `stream_tag`, so a free-choice tag is not safe. AppleHDA uses tags 1 and 2 (SD0, SD1). PPCTL (BAR1 0x804) decouple bit = `1 << 5`; SPIB is left disabled
(reset default; read in 4.1, write only if found enabled).

**Host-side programming order** (`hda_dsp_stream_hw_params`, done *before* the PCM_PARAMS IPC, as in `pcm.c`):
1. PPCTL |= bit5 (single read-modify-write). 2. SD_CTL clear RUN + status bits, poll RUN clear. 3. SD_STS clear. 4. stream reset (SRST set, wait, clear, wait).
5. BDLPL/BDLPU = 0. 6. Build BDL (128 entries x 8192 B, `ioc=0`; split further if the IOVA segments are not contiguous). 7. SD_CTL tag field (bits 20-23) = 6.
8. CBL = 1 048 576. 9. FORMAT = `0x0041` (48 kHz base, mult/div 1, 32 bit = `4 << 4`, channels-1 = 1). 10. LVI = entries-1. 11. BDLPL/BDLPU = BDL IOVA.
**Not done (differs from Linux, on purpose):** no DPLBASE/DPUBASE (global position buffer shared with AppleHDA; LPIB is used instead), no INTCTL bit, no SD_CTL interrupt enables.
`SOF_INTEL_PROCEN_FMT_QUIRK` is APL-only, not CML, so no couple/decouple dance around FORMAT.

**PCM_PARAMS (`0x60010000`), 108 B, compile-checked against `include/sound/sof/stream.h`** (sizes: host_buffer 28, stream_params 84, reply 20):
| offset | field | value |
|---|---|---|
| 0 | hdr.size / hdr.cmd | 108 / `SOF_IPC_GLB_STREAM_MSG \| SOF_IPC_STREAM_PCM_PARAMS` = 0x60010000 |
| 8 | comp_id | 50 (host PCM6C) |
| 12, 16 | flags, reserved[2] | 0 |
| 24 | params.hdr.size | 84 |
| 28 | buffer.hdr (4 B) | 0 |
| 32 / 36 / 40 | buffer.phy_addr / pages / size | page-table IOVA (low 32 bits; page must be < 4 GB or see risk) / 256 / 1 048 576 |
| 56 | direction | 1 (capture) |
| 60 | frame_fmt | 2 (S32_LE) |
| 64 | buffer_fmt | 0 (interleaved) |
| 68 | rate | 48000 |
| 72 / 74 / 76 / 78 | stream_tag / channels / valid bytes / container bytes | 6 / 2 / 4 / 4 |
| 80 | host_period_bytes | 8192 |
| 84 | no_stream_position | 1 (Intel HDA has `pcm_pointer`, so `no_ipc_position = 1`; ABI 3:22 >= 3:10) |
| 86 | cont_update_posn | 0 |
| rest | ext_data_length, chmap[8] | 0 (memset, as Linux) |
Reply: `sof_ipc_pcm_params_reply` (20 B) with `posn_offset`; accept only if `error == 0` and `posn_offset % 76 == 0` and `<= stream window size` (`sof_set_stream_data_offset`).
We do not use stream position data, so `posn_offset` is only validated.

**Page table** (`snd_sof_create_page_table`): 4 KiB-aligned buffer, 20-bit PFNs of each page's **DMA address** packed 5 bytes per 2 pages, little-endian:
even page `i`: `u32 at [5i/2] = pfn`; odd page: `u32 at [5i/2] = (old byte0 & 0xf) | pfn << 4`. 256 pages -> 640 B. Linux passes it for HDA too, so we do.
`phy_addr` is 32-bit: the page-table page must have an IOVA below 4 GB (or the IOMMU mapping must be forced low); logged in 4.1, abort if not.

**Start/stop messages:** `TRIG_START 0x60040000` / `TRIG_STOP 0x60050000` / `PCM_FREE 0x60030000`, each `sof_ipc_stream` = 12 B `{size 12, cmd, comp_id 50}`, plain 12 B reply.
Start: SD_CTL |= RUN (0x02), poll RUN set, **then** TRIG_START. Stop: TRIG_STOP, SD_CTL &= ~RUN, poll RUN clear, SD_STS write-1-clear, then reset (SRST), clear PPCTL bit5.

**Position:** `LPIB` (SD +0x04) polled every ~1 ms; expected rate 384 kB/s -> about 384 B/ms. Pass = monotonic, within 20 % of expectation after the first 20 ms.

**Open points that only hardware can answer (handled by the abort rules, not guessed):** whether the DSP wants the page table non-zero for HDA; whether SD5 ignores
RUN until TRIG_START arrives; whether DMIC data is non-zero (pad mux/clock). 4.1 resolves the IOVA questions, 4.2-4.4 the rest.

## Progress log
- 2026-11-06: plan written; nothing implemented.
- 2026-11-06: **step 4.0 done** (offline, no hardware): spec above. Awaiting review and the go-ahead for 4.1 (kext code + read-only precheck boot).
- 2026-11-06: **step 4.1 done, PASS** (`2026-11-06-phase4-step1.md`): SD5 idle, 1 MiB buffer is one IOVA segment below 4 GB, page table and BDL verified in software. Buffer and page table are both constrained below 4 GB (20-bit PFNs). Next 4.2 pending approval.
- 2026-11-06: **step 4.2 done, PASS** (`2026-11-06-phase4-step2.md`): PCM_PARAMS error=0, PCM_FREE error=0, stream state restored. Next 4.3 pending approval.
- 2026-11-06: **step 4.3 done, PASS** (`2026-11-06-phase4-step3.md`): DMA ran 100 ms cleanly, nothing written without TRIG_START. Next 4.4 pending approval.
- 2026-11-06: **step 4.4 done, PASS** (`2026-11-06-phase4-step4.md`): TRIG_START ok, LPIB advances at the 384 kB/s rate, non-zero samples in host memory. Next 4.5 (2 s capture to a raw file + WAV) pending approval.
- 2026-11-06: **step 4.5 done** (`2026-11-06-phase4-step5.md`): 2 s raw capture written at exactly 384 B/ms. Left channel exact zeros, right channel live signal; content not yet confirmed as real sound. Added `dmic_delay=N` so the capture can land after login.
- 2026-11-06: **step 4.5b**: second capture with speech has a different, speech-like spectrum vs the quiet-room capture, so the right channel is real microphone audio. Left channel still silent. Phase 4 goal met (host DMA capture of DMIC samples, WAV). Next: Phase 5 plan; side experiment for mic A (clk_edge/skew).
