# 2026-11-06 — Phase 2 dry run (dmic_p2=2), USB OpenCore, macOS 15.8.2

Status: **dry run OK, no panic, zero hardware writes.** Raw: `logs/phase2-dry-ioreg.txt`. Plan: `docs/phase2-plan.md`.
Setup: Phase 2 build on USB, boot-args `... dmic_p2=2`; firmware `/Library/Application Support/dmic-macos/sof-cnl.ri`
(sof-bin v2026.09.2 `intel-signed`, 557824 B, sha256 c5119cfc…359e, matches Phase 0; LICENCE.Intel alongside).

## Verified by this run
- Preconditions pass: cmd 0x0406, ADSPCS=0x00000f0f, GCTL=0x101, SD8 CTL=0x00040000 CBL=0, PP cap @0x800, SPIB cap @0x700.
- Firmware file check passes (magic XMan, ext manifest 0x300, `$CPD` at payload).
- DMA mapping via IODMACommand works on this OpenCore setup (no IOMMU fault or panic): 557056 B in 34 x 16 KiB BDL entries,
  BDL IOVA 0xa2561b000 (above 4 GiB; BDLPL/BDLPU split 0x2561b000 / 0xa).
- Write list matches the plan (SD8 offset 0x180, tag 2 -> CTL 0x00240000, SPIB[8] @0x748, ROM_CONTROL 0x81004200 = BIT31|0x01000000|0x4000|(1<<9)).

## Dry-run artefacts (NOT bugs)
- ADSPCS values after the first are computed from the unchanged register (writes are skipped), so e.g. "core0 leave reset = 0x0f0e"
  lacks the SPA bits. Live uses read-modify-write with masks, so only the named bits change.
- "firmware reports READY: YES", all polls "ok", ROM_STATUS/ROM_ERROR/LPIB = 0 are placeholders, nothing was read from the SRAM window.

## Not yet proven
Everything hardware-side: ROM handshake, code-loader DMA from a non-AppleHDA driver, polling without interrupts, FW_READY.
