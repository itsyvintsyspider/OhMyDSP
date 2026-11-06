# 2026-11-06 — Phase 1 results (read-only probe, USB OpenCore, macOS 15.8.2 / Darwin 24.6)

Status: **Phase 1 succeeded.** Probe loaded via OpenCore (USB EFI), ran 30 s after boot, no panic, no writes. Raw log: `logs/phase1.txt`
(extracted from the IORegistry property `phase1-log`; raw `ioreg` in `logs/phase1-ioreg.txt`). Process history: `docs/2026-11-05-phase1-probe-boot-hang.md`.
Not checked in this run: whether AppleHDA output / headset jack still work (ask the user), nor a second boot for repeatability.

## What was observed
**PCI function 00:1f.3 (HDEF, 8086:06c8, subsys 17aa:22c0)** — command 0x0406 (mem+busmaster on), PM state D0, MSI enabled.
- BAR0 (HDA) phys 0x4011118000 len 0x4000; BAR4 (DSP) phys 0x4011000000 len 0x100000 (sizes match the Linux capture; bases differ, as expected).
- Config 0x44 (PGCTL) = 0x0000001b, 0x48 (CGCTL) = 0x00000000. (Linux capture earlier: 0x44=0x10, 0x48=0x007b09ff — i.e. macOS/AppleHDA leaves these differently.)
  ADSPPGD (bit 2 of PGCTL) = 0.

**HDA controller (BAR0)** — GCAP 0x9701: 7 input + 9 output streams, 0 bidirectional. GCTL 0x101 (out of reset, unsol on),
INTCTL 0xC0000000 (GIE+CIE, no stream irq enabled), INTSTS 0, WAKESTS 0, LLCH 0xC00.
- Capability chain: 0xC00 (id 2) → 0x800 PP (id 3) → 0x500 GTS (id 1) → 0x1F00 DRSM (id 5) → 0x700 SPIB (id 4) → end.
- PPCTL (0x804) = 0x40000000: GPROCEN set, no per-stream decouple bits set. SPBFCCTL (0x704) = 0.
- VS EM2 (0x1030) = 0x84806000 → L1SEN (bit 13) set. LTRP 0x280800A9, D0I3C 0x2808.

**Stream descriptors AppleHDA has programmed (all stopped, RUN=0):**
| SD | dir | tag | CBL | BDL base |
|---|---|---|---|---|
| 0 | in | 1 | 0x30000 | 0x144c3000 |
| 1 | in | 2 | 0x30000 | 0x144c2000 |
| 7 | out | 1 | 0x60000 | 0x144c1000 |
All other descriptors (in: 2–6, out: 8–15) are untouched (CTL 0x00040000, everything else 0).
→ Candidate free descriptors: **capture SD2–SD6**, **playback SD8–SD15** (code-loader DMA). Tags AppleHDA already uses: in 1,2; out 1.

**DSP (BAR4)** — ADSPCS = 0x00000F0F: all four cores in reset (CRST bits 0–3) and stalled (CSTALL bits 8–11); SPA = 0, CPA = 0
(powered down). ADSPIC = 0, ADSPIS = 0, all HIPC registers = 0, HIPCCTL = 0. Reads were safe. The SRAM window (ROM_STATUS 0x80000) was
deliberately **not** read because CPA = 0 (probe guard); it will become readable only after power-up.
→ DSP is idle and untouched, exactly the starting state the recipe (§4) assumes.

## Conclusions
1. The probe can attach next to AppleHDAController with its own match category and read config/BAR0/BAR4 without disturbing it (no panic;
   AppleHDAController still shows as the active controller driver in IORegistry).
2. The DSP BAR is mapped and readable while the DSP is off. Phase 2 starts from a clean "everything in reset, powered down" state.
3. AppleHDA uses only 3 of 16 stream descriptors, so a free playback (code loader) and capture (DMIC) descriptor exist.
4. Shared state Phase 2 would have to touch: PGCTL/CGCTL (config space), EM2.L1SEN, PPCTL decouple bit and INTCTL stream bit for our descriptor.
   All of these live in registers AppleHDA also uses — read-modify-write only, never whole-register writes.

## Differences from the recipe to remember
- Recipe §2.3 assumed capture streams first with tag = index+1; AppleHDA's actual tags are in 1,2 / out 1 — choose tags that do not collide.
- Recipe §4.1 says pre-run clears CGCTL.ADSPDCGE and sets PGCTL.ADSPPGD: currently ADSPPGD=0, CGCTL=0.

## Open questions before Phase 2 (nothing written to hardware yet)
- DMA mapping: does Tahoe/15.x with this OpenCore setup (DMAR/VT-d handling) let a non-primary driver DMA to an `IOBufferMemoryDescriptor`
  from the PCH audio function? Needs a design note.
- Interrupts: the DSP interrupt is shared with AppleHDAController's MSI; need a non-intrusive way to receive IPC (polling first).
- How to open/own the provider without conflicting with AppleHDAController (currently the probe never calls `open()`).
