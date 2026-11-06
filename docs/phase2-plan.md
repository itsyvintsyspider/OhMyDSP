# Phase 2 plan — DSP power-up + firmware download (until FW_READY)

Date: 2026-11-06. Status: **code written, NOT enabled, never run.** Every write below is to hardware; CLAUDE.md says stop and ask first.
Source of truth: `docs/bringup-recipe.md` §4–6 (Linux refs re-read for this plan: `intel/hda-loader.c` `cl_dsp_init`, `hda_cl_trigger`,
`hda_cl_copy_fw`; `intel/hda-dsp.c` `hda_dsp_core_{power_up,reset_enter,reset_leave,run}`; `intel/hda-stream.c` `hda_dsp_stream_hw_params`,
`hda_dsp_stream_reset`, `hda_dsp_stream_spib_config`; `intel/hda-ctrl.c` `hda_dsp_ctrl_clock_power_gating`; `intel/hda.c` pre/post_fw_run).

## Safety gates (all must hold, else the probe stays read-only)
| Gate | How |
|---|---|
| Kill switch (total) | boot-arg `dmic_off=1` → kext's `start()` returns false, nothing runs |
| Writes off by default | boot-arg `dmic_p2` absent/0 → Phase 1 read-only behaviour only |
| Dry run | `dmic_p2=2` → logs every write it *would* do (`DRYRUN W ...`), performs none, skips polls |
| Live | `dmic_p2=1` → performs the writes |
| Keep DSP running | `dmic_p2_keep=1`; default is to power the DSP back down after FW_READY (restore the Phase 1 idle state) |
| Log before write | every register write is logged (`IOLog` → verbose console, + IORegistry `phase1-log`) *before* it happens |
| Preconditions | PCI D0 + mem decode; `ADSPCS == 0x00000f0f` (virgin); GCTL.CRST=1; target stream descriptor untouched; firmware file verified (size, `XMan` magic, `$CPD` at payload offset); PP + SPIB capabilities found |
| Rollback | any failure after the first write → stall+reset cores, clear SPA, tear down the stream, restore PGCTL/CGCTL/EM2 to the values read at start |

## Firmware (NOT in the repo, NOT on this Mac yet)
Path the kext reads: `/Library/Application Support/dmic-macos/sof-cnl.ri`.
Expected: `intel-signed/sof-cnl.ri` from the SOF release matching FW 2.2.0 (tag 57864, ABI 3:22:1), size **557824**, sha256
`c5119cfca9c4bdf5ab87a87af08fb8fb2bf92767e0f070ea4eb52a877f10359e` (from the Arch copy decoded in Phase 0). Verify with `shasum -a 256` before
use. Licence: `LICENCE.Intel` (SOF firmware release licence) — read it before obtaining/redistributing; we never bundle it.
The kext checks only size + magic + `$CPD` (no crypto in the kext); the ROM itself validates the CSE signature.

## Resources used (free per Phase 1)
Playback descriptor **SD8** (stream index 8, ROM stream tag 2). AppleHDA uses out SD7 (tag 1) and in SD0/SD1. Interrupts for SD8 are **not** enabled.

## Ordered write list (live mode). Offsets: B0 = HDA BAR0, B4 = DSP BAR4, CFG = PCI config
| # | Write | Linux ref |
|---|---|---|
| 0 | (no hw) load + verify firmware; allocate DMA buffer (557056 B) and BDL page; copy payload from offset 0x300 | `snd_sof_load_firmware_raw`, `hda_data_stream_prepare` |
| 1 | read & save CFG 0x44, 0x48, B0 0x1030 | — |
| 2 | CFG 0x48 clear bit1 (ADSPDCGE); B0 0x1030 clear bit13 (L1SEN); CFG 0x44 set bit2 (ADSPPGD) | `hda_dsp_ctrl_clock_power_gating(false)` |
| 3 | B0 PPCTL (PP cap+4) set bit 8 (decouple SD8) | `hda_dsp_stream_hw_params` |
| 4 | SD8 CTL clear RUN/int bits; stream reset (CTL.SRST set→wait→clear→wait); clear BDLPL/U; STS clear | same |
| 5 | SD8: CTL tag(23:20)=2; CBL=0x88000; FORMAT=0x0040; LVI=entries-1; BDLPL/BDLPU=BDL IOVA | same |
| 6 | SPIB cap: SPBFCCTL set bit 8; SPIB[8] = 0x88000 | `hda_dsp_stream_spib_config` |
| 7 | B4 ADSPCS set SPA(cores0-3) (bits 16-19); poll CPA (bits 24-27) all set, 50 ms | `hda_dsp_core_power_up` |
| 8 | B4 HIPCIDR = BUSY(31) \| 0x01000000 \| 0x4000 \| ((tag-1)<<9) | `cl_dsp_init` step 2 (**`<<9` verified in source**) |
| 9 | B4 ADSPCS clear CRST core0, poll; clear CSTALL core0; check core running | `hda_dsp_core_run` |
| 10 | poll B4 HIPCIDA.DONE(31), 500 ms; then write DONE to clear | `cl_dsp_init` step 4 |
| 11 | power down cores 1-3: set CRST+CSTALL, clear SPA, poll CPA | `hda_dsp_core_reset_power_down` |
| 12 | poll ROM_STATUS (B4+0x80000) state == INIT_DONE(1), 300 ms | `cl_dsp_init` step 7 |
| 13 | SD8 CTL set RUN (bit1); poll ROM_STATUS state == FW_ENTERED(5), 3 s; clear RUN, poll | `hda_cl_trigger/copy_fw` |
| 14 | teardown stream: reset, clear BDL, SPIB disable (SPIB=0, SPBFCCTL clear bit 8), PPCTL clear bit 8 | `hda_cl_cleanup` |
| 15 | poll B4 HIPCTDR.BUSY(31) up to 2 s; read `sof_ipc_fw_ready` at B4+0x81000 (cmd must be 0x70000000); log it; ack: HIPCTDR=BUSY (w1c), HIPCTDA=DONE | `cnl_ipc_irq_thread`, `ipc3_fw_ready`, `cnl_ipc_host_done` |
| 16 | restore CFG 0x44/0x48, B0 0x1030 to saved values (gating back on) | `hda_dsp_post_fw_run` |
| 17 | unless `dmic_p2_keep=1`: power DSP down (stall+reset all cores, clear SPA, poll CPA=0) | `hda_power_down_dsp` |

## Deliberate deviations from Linux (and why)
1. **No interrupts.** Linux enables ADSPIC.IPC/HIPCCTL and the stream INTCTL/IOC bits. Our MSI vector belongs to AppleHDAController; an
   unexpected DSP/stream interrupt there could storm or confuse it. We poll registers instead (HIPCIDA, ROM_STATUS, HIPCTDR). The ROM/DMA do not
   need the interrupt enables to run. Unverified on hardware.
2. **No HDA controller reset, no CORB/RIRB, no GCTL writes** (hard rule). Controller assumed up (GCTL.CRST=1, checked).
3. **No position buffer (DPLBASE)** for SD8 — code loader does not need it.
4. **No IPC4/library/ICCMAX paths** — base FW only.
5. **DMA addresses via `IODMACommand` (mapped)**, not raw physical addresses, so a VT-d/IOMMU setup still works. Untested with this OpenCore DMAR config.
6. **Power down afterwards by default**, so a Phase 2 run leaves the machine as it found it.

## Known risks / unknowns
- Whether macOS' PCI power management or AppleHDA sleep/wake interacts with a powered DSP (hence power-down default).
- PGCTL/CGCTL/EM2 are shared with the codec controller; we restore saved values, but a panic mid-run would leave them changed until reboot/D3.
- `ROM_CONTROL`/IMR/purge semantics are taken from Linux on a different OS; ROM error codes (ROM_STATUS/ROM_ERROR) are logged for diagnosis.
- Firmware file vs. kernel-side file read (`vnode_open`) availability on this macOS (checked at link time via `kmutil libraries`).

## How to run (after you approve)
1. Put the verified firmware at the path above (`sudo mkdir -p ...; sudo cp ...; shasum -a 256`).
2. Dry run first: add `dmic_p2=2` to USB boot-args, boot, read `ioreg -rc com_dmicmac_DMICProbe -w0`. Review the logged writes.
3. Live: `dmic_p2=1` (keep `-v debug=0x100`). Record the outcome in a dated note in `docs/`.
