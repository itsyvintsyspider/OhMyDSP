# DMIC bring-up recipe — ThinkPad P1 Gen 3 (CML-H, cAVS 1.8, SOF IPC3)

Date: 2026-11-05 · Phase 0 deliverable · Source: `sof-linux/` (kernel tree used for study) + `phase0-out/` + the installed
Arch firmware/topology. **Nothing in this document was executed against hardware by the macOS side.** No kext code exists yet.

Conventions
- `file:function` paths are relative to `sof-linux/sound/soc/sof/` unless they start with `include/`.
- **[V]** = read in source or decoded from a real file on this machine. **[I]** = inferred from code reading, must be confirmed by a
  Linux runtime trace (see §13). **[?]** = not verified at all.
- Line numbers are given only where I read them; otherwise cite the function name.

---------------------------------------------------------------------------------------------------

## 0. Scope, safety rules, macOS hazards

Rules from `CLAUDE.md`, restated because they shape every step below:
- Never modify or hook AppleALC / AppleHDA / AppleHDAController. Own bundle, own `IOMatchCategory`.
- Never write HDA codec verbs / CORB / RIRB, or any HDA stream AppleHDA uses. Pick a stream tag/index AppleHDA does not use.
- Phase 1 is read-only. **Stop and ask before any write to hardware** (every "WRITE" below is gated on that).
- Test builds: `kmutil load -p` only. No OpenCore injection until stable. Keep a spare OpenCore USB.
- `os_log` everything; a dated note in `docs/` per experiment.
- Firmware: load the `.ri` from `/Library`, never commit it (licence in §5.1).

**Hazards specific to this machine (the Linux driver does things we must NOT copy):**
1. Linux owns the whole controller. `hda_dsp_ctrl_init_chip()` / `hda_dsp_ctrl_get_caps()` (`intel/hda-ctrl.c`) reset the HDA
   controller via `GCTL` and bring up CORB/RIRB. On macOS that would kill AppleHDA/ALC285 output. **Do not replicate.** We must
   attach to a controller that AppleHDA already initialised and only touch the DSP BAR plus an unused stream descriptor.
2. BAR0 (HDA regs), `PPCTL` (BAR1) and PCI config `PGCTL/CGCTL` are shared with AppleHDA. Phase 1 = reads only.
3. The code-loader DMA uses a *playback* stream descriptor (`hda_data_stream_prepare`, `intel/hda-stream.c:1309`) and DMIC capture
   uses a *capture* stream descriptor. We need one free stream of each kind that AppleHDA never programs. AppleHDA's usage must be
   measured in Phase 1 (read `SD_CTL`/`SD_BDLPL` of every descriptor) before choosing. [?]
4. The device was in D3hot when `lspci` ran (`phase0-out/lspci-1f3.txt`). Do not assume registers are readable until powered.
5. DSP power gating is controlled through *PCI config* (PGCTL 0x44, CGCTL 0x48) of the same function AppleHDA drives. [V]

---------------------------------------------------------------------------------------------------

## 1. Target facts (from `phase0-out/`)

| Item | Value | Source |
|---|---|---|
| PCI id | 8086:06c8 (CML-H HD Audio), subsys 17aa:22c0, class 0x040100 (DSP present) | `lspci-1f3.txt` [V] |
| BAR0 (HDA regs) | 16 KiB @ 0x6041118000 | `lspci-1f3.txt` [V] |
| BAR4 (DSP regs) | 1 MiB @ 0x6041000000 | `lspci-1f3.txt` [V] |
| Interrupt | MSI, 1 vector | `lspci-1f3.txt` [V] |
| PCI cfg observed | 0x44 = 0x00000010, 0x48 = 0x007b09ff | `lspci-1f3-config.txt` [V] |
| Linux driver / desc | `sof-audio-pci-intel-cnl`, `cml_desc` (`intel/pci-cnl.c:88-119`, id table 122-128: `HDA_CML_H → cml_desc`) | `dmesg-sof.txt`, source [V] |
| IPC | type 0 = IPC3 | `dmesg-sof.txt` [V] |
| Firmware | `sof-cml.ri` → symlink to `intel-signed/sof-cnl.ri`, version 2.2.0 tag 57864, ABI 3:22:1 | dmesg + ext manifest decode [V] |
| Topology | `intel/sof-tplg/sof-hda-generic-2ch.tplg` | dmesg [V] |
| NHLT | "DMICs detected in NHLT tables: 2" ; `hda codecs found, mask 5` | dmesg [V] |
| Codec | ALC285 on HDA link (AppleALC alcid=31 for output/jack) | CLAUDE.md |

Firmware file (local, **not to be committed**): `/usr/lib/firmware/intel/sof/intel-signed/sof-cnl.ri`
- size 557824 bytes, sha256 `c5119cfca9c4bdf5ab87a87af08fb8fb2bf92767e0f070ea4eb52a877f10359e` [V]
- Extended manifest: magic `XMan` (0x6e614d58), `full_size` = 0x300 (768) → **payload_offset = 0x300**; byte at 0x300 begins `$CPD`
  (CSE code partition directory) [V]. The ROM validates the signed `$CPD` image; the host only DMAs bytes 0x300..EOF.

Kernel/firmware ABI: host tree has `SOF_ABI 3:23:1` (`include/uapi/sound/sof/abi.h`), firmware is 3:22:1 (compatible minor, [V]).

---------------------------------------------------------------------------------------------------

## 2. Register map

All offsets from the BAR named. Source = `intel/hda.h` unless stated.

### 2.1 BAR indices (`intel/hda.h:156-160`)
`HDA_DSP_HDA_BAR=0`, `HDA_DSP_PP_BAR=1`, `HDA_DSP_SPIB_BAR=2`, `HDA_DSP_DRSM_BAR=3`, `HDA_DSP_BAR=4`. BAR1–3 are *not* PCI BARs: they are
sub-regions of BAR0 found by walking the HDA capability list (`hda_dsp_ctrl_get_caps`, `intel/hda-ctrl.c`) — on macOS we must walk
the same list read-only. [V for the Linux behaviour; macOS layout = Phase 1]

### 2.2 DSP BAR (BAR4)
| Reg | Offset | Bits / notes |
|---|---|---|
| ADSPCS | 0x04 | per-core fields: CRST bit0, CSTALL bit8, SPA bit16, CPA bit24 (each shifted left by core index; masks built from `host_managed_cores_mask`) |
| ADSPIC | 0x08 | IPC = BIT(0), CL_DMA = BIT(1) |
| ADSPIS | 0x0C | same bit meaning (`hda_dsp_check_ipc_irq`, `intel/hda-ipc.c`) |
| HIPCTDR | 0xC0 | DSP→host message register, BUSY = BIT(31) (`CNL_DSP_REG_HIPCTDR`, hda.h:384) |
| HIPCTDA | 0xC4 | host's ack of DSP message, DONE bit |
| HIPCTDD | 0xC8 | extension data |
| HIPCIDR | 0xD0 | host→DSP message, BUSY = BIT(31) (`CNL_DSP_REG_HIPCIDR_BUSY`, hda.h:393) |
| HIPCIDA | 0xD4 | DSP's ack, DONE = BIT(31) (`CNL_DSP_REG_HIPCIDA_DONE`, hda.h:397) |
| HIPCIDD | 0xD8 | extension data |
| HIPCCTL | 0xE8 | DONE bit1, BUSY bit0 interrupt enables |
| SRAM window n | `0x80000 + n*0x20000` | `SRAM_WINDOW_OFFSET(x)`, hda.h:162; `hda_dsp_ipc_get_window_offset` (`intel/hda-ipc.c:396`) |
| ROM_STATUS | 0x80000 | `HDA_DSP_SRAM_REG_ROM_STATUS`, hda.h:170 |
| ROM_ERROR / FW_STATUS | 0x80004 | hda.h:171,173 |
| FW_TRACEP / FW_END | 0x80008 / 0x8000C | hda.h:174-175 |
| Uplink mailbox (FW_READY) | 0x81000 | `HDA_DSP_MBOX_UPLINK_OFFSET`, hda.h:177 |
| SSP base | 0x10000, stride 0x1000 | `cnl_chip_info.ssp_base_offset`; not needed for DMIC |

ROM state values (`FSR_STATE_MASK = GENMASK(23,0)`): INIT_DONE = 1, FW_ENTERED = 5 [V]. ROM error codes: hda.h:259-274 (e.g. 43
`BASE_FW_NOT_FOUND`, 44 `CSE_VALIDATION_FAILED`, 51 `BASEFW_INCOMPAT`) — log these in Phase 2.

`cnl_chip_info` for this device (`intel/cnl.c:449`) [V]: `cores_num=4`, `init_core_mask=1`, `host_managed_cores_mask=GENMASK(3,0)`,
`ipc_req=HIPCIDR`, `ipc_ack=HIPCIDA`, `ipc_ctl=HIPCCTL`, `rom_status_reg=0x80000`, `rom_init_timeout=300 ms`, `ssp_count=3`,
`d0i3_offset=0x104A`, `hw_ip_version=SOF_INTEL_CAVS_1_8`, `cl_init=cl_dsp_init`, `power_down_dsp=hda_power_down_dsp`.

### 2.3 HDA BAR (BAR0)
GCAP 0x00 (stream counts), GCTL 0x08 (bit0 CRST — **never write**, bit8 UNSOL), WAKESTS 0x0E, LLCH 0x14, INTCTL 0x20 (CIE bit30,
GIE bit31, one enable bit per stream), INTSTS 0x24, VS EM2 0x1030 (L1SEN bit13), LTRP 0x1048, D0I3C 0x104A.
Stream descriptors: base 0x80, stride 0x20 (`SOF_STREAM_SD_OFFSET`). Per-descriptor: CTL 0x00 (SRST bit0, RUN/DMA_START bit1, int enables,
**stream tag bits 20-23**), STS 0x03, LPIB 0x04, CBL 0x08, LVI 0x0C, FIFOW 0x0E, FIFOS 0x10, FORMAT 0x12, BDLPL 0x18, BDLPU 0x1C.
DPLBASE 0x70 / DPUBASE 0x74 (enable bit0). `sd_int_sta_mask = 1 << index`. Capture descriptors come first: index
0..num_capture-1 get tag `i+1`; playback descriptors get tag `i - num_capture + 1` (`hda_dsp_stream_init`, `intel/hda-stream.c`). Counts come from GCAP.

### 2.4 Other regions
- PP capability (BAR1): PPCTL +0x04 — GPROCEN bit30, PIE bit31, one "decouple" bit per stream.
- SPIB capability (BAR2): SPBFCCTL +0x04; SPIB[i] at +0x08 + 8*i.
- PCI config: PGCTL 0x44 (ADSPPGD bit2), CGCTL 0x48 (ADSPDCGE bit1, MISCBDCGE bit6). Observed 0x44=0x10, 0x48=0x007b09ff.

---------------------------------------------------------------------------------------------------

## 3. Phase 1 read-only discovery (no writes)

Goal: prove what a macOS driver may touch. Reads only:
1. PCI cfg 0x44, 0x48, vendor/device/subsystem, PMCSR (power state), BAR0/BAR4 sizes.
2. BAR0: GCAP, GCTL, INTCTL, INTSTS, WAKESTS, LLCH and the capability chain (to locate PP/SPIB/DRSM). For each stream descriptor:
   CTL, STS, CBL, LVI, FORMAT, BDLPL/U → record which ones AppleHDA has programmed (tag, running).
3. BAR4: ADSPCS, ADSPIC, ADSPIS, HIPCTDR/IDR/IDA, HIPCCTL, ROM_STATUS (0x80000), ROM_ERROR (0x80004). Expect the DSP unpowered;
   reads of a powered-down DSP BAR may return 0xFFFFFFFF — log it, do not retry in a loop.
4. Compare against Linux values from the supplementary captures in §13.

Exit criteria: documented table of free playback + capture descriptors and the DSP's idle register values. No conflict with AppleHDA observed.

---------------------------------------------------------------------------------------------------

## 4. DSP power-up and ROM boot — "WRITE" steps (Phase 2, needs explicit approval)

Linux call chain [V]: `sof_probe_continue` (`core.c:454`) → `snd_sof_load_firmware_raw` (`loader.c:17`) → `snd_sof_run_firmware`
(`loader.c:113`) → `pre_fw_run = hda_dsp_pre_fw_run` (`intel/hda.c:422`) → `run = hda_dsp_cl_boot_firmware` (`intel/hda-loader.c:340`) →
wait for FW_READY (`boot_wait`, timeout `TIMEOUT_DEFAULT_BOOT_MS` = 2000, `core.c:65`) → `post_fw_run = hda_dsp_post_fw_run` (`intel/hda.c:443`).

### 4.1 Pre-run (`hda_dsp_pre_fw_run`, `intel/hda.c:422`)
1. If the DSP is already enabled, power it down (`power_down_dsp`).
2. `hda_dsp_ctrl_clock_power_gating(false)` (`intel/hda-ctrl.c`): CGCTL clear `ADSPDCGE`, `VS EM2` clear `L1SEN`, PGCTL set `ADSPPGD`.
   → PCI-config writes in a function AppleHDA also owns (§0.4). Needs approval and a plan to restore.

### 4.2 `cl_dsp_init` (`intel/hda-loader.c:66`) — up to 3 attempts
Order is exactly:
1. `hda_dsp_core_power_up(host_managed_cores_mask)` (`intel/hda-dsp.c:280`): set ADSPCS.SPA for cores 0-3, poll CPA (timeout 50 ms, poll 500 µs).
2. `hda_ssp_set_cbp_cfp` — SSP-only; skip for DMIC. [V]
3. Write **ROM_CONTROL** to HIPCIDR: `BUSY | HDA_DSP_ROM_IPC_CONTROL (0x01000000) | HDA_DSP_ROM_IPC_PURGE_FW (0x00004000) | ((stream_tag-1) << 9)`.
   Constants hda.h:276-277 [V]; the `<<9` shift is `HDA_DSP_ROM_STREAM_SHIFT` per my notes of `hda-loader.c` [verify before use].
4. `hda_dsp_core_run(core 0)` (`intel/hda-dsp.c:248`): clear CRST (take core out of reset), poll, clear CSTALL.
5. Poll HIPCIDA.DONE (500 ms) then write DONE to clear it.
6. `hda_dsp_core_reset_power_down(cores 1-3)` (`intel/hda-dsp.c:376`).
7. `hda_dsp_ipc_int_enable` (`intel/hda-dsp.c:417`): HIPCCTL DONE|BUSY, ADSPIC.IPC.
8. Poll ROM_STATUS state == INIT_DONE (1), `rom_init_timeout` 300 ms.

Core helpers (`intel/hda-dsp.c`): `hda_dsp_core_reset_enter` (130), `reset_leave` (169), `stall_reset` (209), `is_enabled` (222),
`core_run` (248), `core_power_up` (280), `core_power_down` (328), `enable_core` (351), `reset_power_down` (376), `ipc_int_enable` (417).
Timeouts: `HDA_DSP_RESET_TIMEOUT_US` 50000, poll 500, `INIT_TIMEOUT_US` 500000, `BASEFW_TIMEOUT_US` 3000000 [V].

---------------------------------------------------------------------------------------------------

## 5. Firmware download (code-loader DMA)

### 5.1 Licence (check before bundling anything)
`/usr/share/licenses/sof-firmware/LICENCE.Intel` exists on this machine; the Arch package metadata lists `BSD-3-Clause AND ISC`.
The file is Intel's "SOF FIRMWARE RELEASE LICENCE" (© 2015-2019) with a limited patent grant. **Read the full text and the
redistribution clause before any distribution.** Project policy anyway: do not commit `.ri`; load from `/Library/...` at runtime.

### 5.2 Host-side buffer prep
`hda_cl_prepare` → `hda_data_stream_prepare` (`intel/hda-stream.c:1309`) [V]:
1. Take a free **playback** stream (see §0.3 — on macOS pick one AppleHDA never uses).
2. Allocate a DMA buffer of `fw->size - payload_offset` = 557824 − 768 = **557056 bytes**. Must be physically addressable by the
   controller (IOMMU/VT-d on macOS: use `IODMACommand`/DART-style mapping of the device — [?] Phase 2 question).
3. Format register `HDA_CL_STREAM_FORMAT = 0x40` [V].
4. `hda_dsp_stream_hw_params` programs the BDL and descriptor (§11.2). SPIB is enabled with the buffer size.
5. `memcpy` payload (from offset 0x300) into the buffer.

### 5.3 Transfer (`hda_cl_copy_fw`, `intel/hda-loader.c`)
1. `hda_cl_trigger START`: INTCTL stream bit + `SD_CTL` DMA_START|int enables.
2. Poll ROM_STATUS == FW_ENTERED (5), timeout 3 s.
3. Trigger STOP, then `hda_cl_cleanup` (stream reset, free).

### 5.4 After run
`hda_dsp_post_fw_run` (`intel/hda.c:443`) re-enables clock/power gating (reverse of §4.1). Firmware then posts FW_READY (§6).

---------------------------------------------------------------------------------------------------

## 6. FW_READY and mailbox discovery

1. Firmware writes `struct sof_ipc_fw_ready` (108 bytes, compiled [V]) at the **uplink mailbox 0x81000** and sets HIPCTDR.BUSY with
   `cmd = SOF_IPC_FW_READY = 0x70000000` [V].
2. IRQ path: `cnl_ipc_irq_thread` (`intel/cnl.c:112`) → `sof_ipc3_rx_msg` → `sof_ipc3_do_rx_work` (`ipc3.c`) → `ipc3_fw_ready`
   (`ipc3.c:817`): read struct from mailbox, check ABI (`sof_ipc3_validate_fw_version`), parse extended data windows
   (`ipc3_get_windows`, `ipc3.c` ~700-760).
3. Ack: `cnl_ipc_host_done` (BUSY write-1-clear on HIPCTDR, set DONE in HIPCTDA).

**Window layout in this firmware** — decoded from the ext manifest of `sof-cnl.ri` (7 windows). The runtime FW_READY ext data should
repeat it; confirm in the §13 trace. `BAR4 offset = 0x80000 + id*0x20000 + offset` [V decode, I that FW_READY matches]:

| Region | window id | size | offset in window | BAR4 offset | Linux name |
|---|---|---|---|---|---|
| REGS | 0 | 0x1000 | 0x0 | 0x80000 | ("regs") |
| UPBOX (DSP→host) | 0 | 0x1000 | 0x1000 | **0x81000** | `dsp_box` |
| DOWNBOX (host→DSP) | 1 | 0x2000 | 0x0 | **0xA0000** | `host_box` |
| DEBUG | 2 | 0x800 | 0x0 | 0xC0000 | `debug_box` |
| EXCEPTION | 2 | 0x800 | 0x800 | 0xC0800 | `dsp_oops_offset` |
| STREAM | 2 | 0x1000 | 0x1000 | 0xC1000 | `stream_box` |
| TRACE | 3 | 0x2000 | 0x0 | 0xE0000 | (trace) |

`ipc3_get_windows` assigns `dsp_box = inbox` (UPBOX), `host_box = outbox` (DOWNBOX), stream/debug as above [V].
`sof_mailbox_write/read` are plain `memcpy_toio/fromio` on BAR4 + offset (`iomem-utils.c:54-70`) [V].
Note the ROM_STATUS word at 0x80000 shares window 0 with REGS; the host_box at 0xA0000 is *not* in window 0 [V].

---------------------------------------------------------------------------------------------------

## 7. IPC3 transport

Header: `struct sof_ipc_cmd_hdr { u32 size; u32 cmd; }` (8 bytes). `cmd = (GLB type << 28) | (cmd type << 16)`. Max message
`SOF_IPC_MSG_MAX_SIZE` = 384. Timeouts: IPC 500 ms (`TIMEOUT_DEFAULT_IPC_MS`, `core.c:64`). Replies are `struct sof_ipc_reply`
(12 bytes: hdr + `s32 error`). Panic detection: `(word & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC` with magic 0x0dead000, mask
0x0ffff000 (`include/sound/sof/trace.h:81-82`) [V].

**Host → DSP** (`cnl_ipc_send_msg`, `intel/cnl.c:284`) [V]:
1. `sof_mailbox_write(host_box, msg, size)`.
2. Write `HIPCIDR = BUSY` (BIT 31). (Compact PM_GATE variant puts payload in HIPCIDD/HIPCIDR.)
3. Wait for DSP to set HIPCIDA.DONE → IRQ (`cnl_ipc_irq_thread`): mask DONE interrupt, read reply from the mailbox
   (`sof_ipc3_get_reply`, `ipc3.c:234`), `cnl_ipc_dsp_done` (set DONE ack, unmask). Wait side: `ipc3_wait_tx_done` (`ipc3.c:289`).
   Tx path: `ipc3.c:332-383`.

**DSP → host**: HIPCTDR.BUSY → read from `dsp_box` (0x81000) → handle → `cnl_ipc_host_done`.

Global message types: REPLY 1, COMPOUND 2, TPLG 3, PM 4, COMP 5, STREAM 6, FW_READY 7, DAI 8, TRACE 9.
Struct sizes compiled from the kernel headers with gcc (`scratchpad/sizes.c`) [V]:

| Struct | bytes | | Struct | bytes |
|---|---|---|---|---|
| sof_ipc_cmd_hdr | 8 | | sof_ipc_buffer | 44 |
| sof_ipc_reply | 12 | | sof_ipc_pipe_new | 48 |
| sof_ipc_comp | 28 | | sof_ipc_pipe_ready | 12 |
| sof_ipc_comp_config | 36 | | sof_ipc_pipe_comp_connect | 16 |
| sof_ipc_comp_host | 76 | | sof_ipc_dai_config | 216 |
| sof_ipc_comp_dai | 80 | | sof_ipc_dai_dmic_params | 164 |
| sof_ipc_pcm_params | 108 | | sof_ipc_dai_dmic_pdm_ctrl | 24 |
| sof_ipc_pcm_params_reply | 20 | | sof_ipc_stream | 12 |
| sof_ipc_fw_ready | 108 | | sof_ipc_stream_posn | 76 |

Command words (compiled) [V]:
`PIPE_NEW 0x30100000`, `COMP_NEW 0x30010000`, `BUFFER_NEW 0x30200000`, `COMP_CONNECT 0x30030000`, `PIPE_COMPLETE 0x30130000`,
`DAI_CONFIG 0x80010000`, `STREAM_PCM_PARAMS 0x60010000`, `STREAM_PCM_FREE 0x60030000`, `TRIG_START 0x60040000`, `TRIG_STOP 0x60050000`.
Additional: `TRIG_PAUSE 0x...0006`, `TRIG_RELEASE 0x...0007`, `PM_CORE_ENABLE 0x007`, `PM_GATE 0x008`, `CTX_SAVE 0x001` (cmd-type part only).

`struct sof_ipc_pipe_new` (`include/sound/sof/topology.h`): hdr, comp_id, pipeline_id, sched_id, core, period, priority, period_mips,
frames_per_sched, xrun_limit_usecs, time_domain. `sof_ipc_pipe_comp_connect`: hdr, source_id, sink_id.
`sof_ipc_dai_config` (`include/sound/sof/dai.h`): hdr, type, dai_index, `format(u16)@16`, `group_id(u8)@18`, `flags(u8)@19`,
`reserved[8]@20`, union `dmic@52`. `DAI type` enum: NONE 0, SSP 1, **DMIC 2**, HDA 3, ALH 4 ...

---------------------------------------------------------------------------------------------------

## 8. What the DSP firmware needs to start a DMIC capture (IPC3 sequence)

The widgets are created when the PCM is prepared because the DMIC pipelines are *dynamic* (scheduler widget has dynamic-pipeline=1,
decoded from the topology). Linux path [V by code reading, ordering = I]:

`sof_pcm_prepare` (`pcm.c` ~316) → `sof_pcm_hw_params` (`pcm.c:116`) → `sof_widget_list_setup` (`sof-audio.c:765`):
walk widgets from the DAI-out widget; `sof_widget_setup_unlocked` sends each widget via `sof_ipc3_widget_setup` (`ipc3-topology.c`);
DAI widgets additionally get `DAI_CONFIG` via `sof_ipc3_dai_config` (`ipc3-topology.c:2186`, flags = HW_PARAMS);
`sof_setup_pipeline_connections` → `sof_ipc3_route_setup` (COMP_CONNECT per route); `sof_ipc3_complete_pipeline` (PIPE_COMPLETE);
then `sof_ipc3_pcm_hw_params` (`ipc3-pcm.c:34`) sends PCM_PARAMS to the host component.

**Expected message order for the 48 kHz capture pipeline (pipeline 10)** [I — confirm with trace]:

| # | IPC | Object | Notes |
|---|---|---|---|
| 1 | PIPE_NEW | scheduler `PIPELINE.10.DMIC0.IN` | period 1000 µs, priority 0, core 0, time_domain 1, MIPS 5000 |
| 2 | COMP_NEW | DAI `DMIC0.IN` (type DMIC, dai_index 0, dir capture, s32le) | then DAI_CONFIG (§9) |
| 3 | BUFFER_NEW | BUF10.2, 768 B | caps 113 |
| 4 | COMP_NEW | effect EQIIR10.0 (highpass) | |
| 5 | BUFFER_NEW | BUF10.1, 768 B | |
| 6 | COMP_NEW | volume `Dmic0` (ramp 250 ms), s32le | |
| 7 | BUFFER_NEW | BUF10.0, 768 B | |
| 8 | COMP_NEW | host `PCM6C` ("Highpass Capture 6", PCM "DMIC" id 6), dir capture | |
| 9 | COMP_CONNECT ×6 | DMIC0.IN→BUF10.2→EQIIR→BUF10.1→Dmic0→BUF10.0→PCM6C | |
| 10 | PIPE_COMPLETE | pipeline 10 | |
| 11 | PCM_PARAMS | host comp PCM6C | stream_tag, rate, ch, buffer page table; reply gives `posn_offset` |
| 12 | TRIG_START | host comp PCM6C | after/around host DMA start, see §11 |

The 16 kHz variant is pipeline 11 (DMIC1.IN, DAI_INDEX 1, BUF11.x 256 B, PCM7C "DMIC16kHz", PCM id 7) [V topology decode].

Simplest first target: **pipeline 10 as shipped**. Do not hand-build a reduced graph in Phase 3 until the shipped sequence works;
payload bytes for every message must come from the Linux trace (§13), because field values (core, IDs, caps, UUID extension) are
topology-derived and I have only decoded part of them. The PCM_PARAMS page table is the compressed 20-bit PFN format of
`snd_sof_create_page_table` (`sof-utils.c:25`: 2.5 bytes per page, `idx = (5*i)>>1`) [V].

---------------------------------------------------------------------------------------------------

## 9. DMIC controller configuration (from topology, not NHLT)

On IPC3 the DMIC hardware config is sent as `DAI_CONFIG` built from topology tokens in `sof_link_dmic_load`
(`ipc3-topology.c:1502`) into `struct sof_ipc_dai_dmic_params` (`include/sound/sof/dai-intel.h:176`). Decoded from the installed
`/usr/lib/firmware/intel/sof-tplg/sof-hda-generic-2ch.tplg` (44247 B) [V]:

| Field | `dmic01` (link id 6, DAI_INDEX 0) | `dmic16k` (link id 7, DAI_INDEX 1) | Token |
|---|---|---|---|
| driver_ipc_version | 1 | 1 | `INTEL_DMIC_DRIVER_VERSION` (600) |
| pdmclk_min / max | 2 400 000 / 4 800 000 Hz | same | `CLK_MIN/MAX` (601/602) |
| duty_min / max | 40 / 60 % | same | 603/604 |
| fifo_fs | 48000 | 16000 | `SAMPLE_RATE` (608) |
| fifo_bits | 32 | 32 | `FIFO_WORD_LENGTH` (609) |
| unmute_ramp_time_ms | 200 | 400 | 610 |
| num_pdm_active | 1 | 1 | 605 |
| pdm[0].id | 0 | 0 | `PDM_CTRL_ID` (700) |
| pdm[0].enable_mic_a / b | 1 / 1 | 1 / 1 | `MIC_A/B_Enable` |
| polarity_a / b, clk_edge, skew | 0 / 0, 0, 0 | same | 703-706 |

`SOF_DAI_INTEL_DMIC_NUM_CTRL = 4` in this tree. The firmware (not the host) derives clock dividers, FIR/decimation and programs
the DMIC IP registers from these parameters — **the host never writes DMIC registers directly on IPC3.** [I: by absence in
`sof-linux/sound/soc/sof/intel`; the DMIC register block lives in the DSP address space and is firmware-owned]

Resulting wire message: `sof_ipc_dai_config{ hdr{size=216, cmd=0x80010000}, type=2 (DMIC), dai_index=0, flags=HW_PARAMS(1), dmic{...} }` [V
struct / I flags value].

Both the 48 kHz and the 16 kHz DAI configs are in the topology; the 2-mic stereo capture on this laptop uses the same single PDM
controller (id 0) with mic A and mic B enabled (i.e. one data line carrying two mics on opposite clock edges).

---------------------------------------------------------------------------------------------------

## 10. NHLT (ACPI) — what it says and what it is used for

File: `phase0-out/NHLT.bin`, 6350 bytes, checksum OK. OEM LENOVO, table id `TP-N2V`, OEM rev 0x10400, creator PTEC rev 2 [V].
- **1 endpoint** @0x25, length 0x18a1: link type **2 = DMIC (PDM)**, instance 0, vendor 0x8086, device 0xae20, revision 1, subsys 1,
  device_type 0, **direction 1 = capture**, virtual_bus_id 0.
- Endpoint specific config (48 B): mic-array descriptor: `nb_mics = 2`, array_type 0xF (vendor defined), config_type 1 (MIC_ARRAY);
  mic0 horizontal offset −42, mic1 +42 (units per `include/sound/intel-nhlt.h`), other fields identical.
- 2 formats (each `WAVE_FORMAT_EXTENSIBLE`, 48 kHz, 2 ch): fmt0 16-bit container (192000 B/s); fmt1 32-bit container with 24 valid bits (288000 B/s).
  Each carries a 3072-byte vendor blob starting `01 00 00 00 10 ff ff ff 10 ff ff ff ...` — the DMIC controller register blob used by IPC4.
- One unexplained trailing 0 byte inside the endpoint after the last format [V observed, meaning ?]. Table trailer: size 4, 0xDEADBEEF (OED).

Role in this setup:
- Linux counts mics only: `check_dmic_num` (`intel/hda.c:560`) → `intel_nhlt_get_dmic_geo` (`include/sound/hda/core/intel-nhlt.c:29`) → `dmic_num = 2`
  → machine/topology selection (`sof-hda-generic-2ch`). `hda_dsp_probe` calls `intel_nhlt_init` (`intel/hda.c:932`).
- The blobs are **sent to the firmware only for IPC4** (`intel/hda-dai.c:808`: `ipc_type == SOF_IPC_TYPE_4 && !hda_use_tplg_nhlt`).
  On IPC3 they are ignored by the DMIC setup. So for a first macOS bring-up on IPC3 the NHLT only confirms: 2 mics, one PDM controller, 48 kHz stereo, capture.

Open question: whether IPC4 + NHLT (newer firmware) is a better long-term path on macOS. Out of scope for Phase 0; the shipped
distro firmware here is IPC3.

---------------------------------------------------------------------------------------------------

## 11. Host DMA capture programming

### 11.1 Stream selection
Capture streams are descriptors `0 .. num_capture-1` with tag = index+1 (§2.3). Use one AppleHDA does not use; the ROM and the host
DMA path both identify the stream by tag, and the tag also goes into PCM_PARAMS (`stream_tag` field of `sof_ipc_pcm_params`).

### 11.2 `hda_dsp_stream_hw_params` (`intel/hda-stream.c:581`) [V]
1. PPCTL: set the decouple bit for this stream (host/link decoupling).
2. Clear `SD_CTL` RUN + interrupt enables; poll; clear `SD_STS`.
3. `hda_dsp_stream_reset`: set SRST, wait, clear SRST, wait.
4. Clear BDLPL/BDLPU.
5. `hda_dsp_stream_setup_bdl`: BDL entries `{addr_lo, addr_hi, size, ioc}`; split on 4 KiB boundaries.
6. `SD_CTL` stream tag (bits 20-23) ← tag; `CBL` = buffer bytes; `FORMAT`; `LVI` = nfrags−1; `BDLPL/BDLPU` ← BDL address.
7. DPLBASE/DPUBASE enable (position buffer); enable stream interrupts.

### 11.3 Format word (`hda_dsp_pcm_hw_params`, `intel/hda-pcm.c`)
`format = hda_dsp_get_mult_div(rate) | hda_dsp_get_bits(width) | (channels−1)`; then `hda_dsp_stream_hw_params`, SPIB disabled, and
`platform_params.stream_tag` filled for the PCM_PARAMS IPC. For 48 kHz / S32_LE / 2 ch this resolves with the usual HDA
encoding (base 48 kHz, mult 1, div 1, bits per `hda_dsp_get_bits`). Exact value to be captured from Linux (§13).

### 11.4 Start/stop order (`sof_pcm_trigger`, `pcm.c:386`)
- START: `ipc3_pcm_ops` does **not** set `ipc_first_on_start` (only `ipc4-pcm.c:1326` does) [V], so Linux triggers the **platform DMA first**
  (`hda_dsp_stream_trigger`: INTCTL stream bit, `SD_CTL` RUN|int enables, poll RUN), **then** sends `STREAM_TRIG_START` to the host comp.
- STOP: IPC `STREAM_TRIG_STOP` first, then stop the DMA.
- `reset_hw_params_during_stop = true` for IPC3 (`ipc3-pcm.c:428`), so the next start redoes hw_params.
- Position: `DPIB`/LPIB or `posn_offset` from the PCM_PARAMS reply into the stream box (`hda_set_stream_data_offset`).

### 11.5 Interrupts
`hda_dsp_check_ipc_irq` (`intel/hda-ipc.c`): ADSPIS.IPC → IPC thread; ADSPIS.CL_DMA clears `code_loading` and wakes the loader (and
returns false). Stream IRQs via INTSTS (`hda_dsp_check_stream_irq`, `intel/hda-stream.c`). On macOS use one MSI/legacy vector with a
filter that only claims bits the companion owns; AppleHDA owns the rest of INTSTS. [?]

---------------------------------------------------------------------------------------------------

## 12. Teardown

`sof_pcm_hw_free` → `STREAM_PCM_FREE` (0x60030000), then `_hda_dsp_stream_hw_free` (`intel/hda-stream.c:775`): stream reset, clear
PPCTL decouple bit, SPIB disable. Pipeline free: topology widgets freed (IPC `TPLG` free ops). DSP power-down: `hda_power_down_dsp`
(core power down, `power_down_dsp` in `cnl_chip_info`) and restore gating bits.

---------------------------------------------------------------------------------------------------

## 13. Gaps, and what to capture on Linux next

`phase0-out/` has the facts above but **no runtime evidence**. Missing:
1. Register dumps: `/sys/kernel/debug/sof/{hda,pp,dsp,regs}` or equivalent while capturing (and idle).
2. A full IPC3 trace with payload bytes (kernel param `sof_debug` flags `SOF_DBG_PRINT_IPC_SUCCESS_LOGS | SOF_DBG_DUMP_IPC_MESSAGE_PAYLOAD`
   + dynamic debug for `snd_sof*`) during one `arecord` session on the DMIC PCM → this turns §8 from [I] into [V].
3. `ftrace` `sof_intel` events (stream IRQs, stream trigger) during capture.
4. `arecord -l` / `/proc/asound/cards` — `arecord` was missing so DMIC capture was never confirmed working on Linux.
5. Contents of `fw_version` / `fw_state` debugfs.
6. A recorded WAV from Linux (reference signal for Phase 4 comparison).
7. For the Phase 1 comparison: the Linux value of every register read in §3, idle and running.

These need `tools/phase0-collect.sh` extended (or a second script); that is a read-only collection on Linux, no hardware risk.

**Update 2026-11-05: Arch Linux is no longer installed on this laptop, so items 1-7 above can no longer be captured.**
Consequences for the plan:
- Every **[I]** (inferred) item in §8 (IPC message order, payload bytes) stays unconfirmed. There is no reference trace to diff against.
  Phase 3 must therefore be built from the SOF source + the decoded topology in this document, and validated incrementally against
  what the firmware actually replies on the Mac (log every message and reply, abort on the first non-zero `error`).
- Phase 1 has no Linux register values to compare with. The only reference is the DSP's own idle state as read on macOS, plus the
  constants in §2 (taken from source, not from hardware).
- Phase 4 has no reference WAV. Use a known sound source against the recorded stream instead.
- A Linux live USB (booted without OpenCore, e.g. an Arch or Ubuntu installer) would restore items 1-7 without installing anything.
  This is the cheapest way to turn §8 from [I] into [V] if Phase 3 stalls.

---------------------------------------------------------------------------------------------------

## 14. Mapping to project phases

| Phase | Uses sections | Writes to HW? |
|---|---|---|
| 1 probe | §2, §3 | no |
| 2 power-up + FW | §4, §5, §6 | **yes — ask first** |
| 3 IPC + topology | §7, §8, §9 | yes — ask first |
| 4 host DMA capture | §11 | yes — ask first |
| 5 audio device | n/a to SOF | — |

Open questions for the user / later phases
1. Which HDA stream descriptors does AppleHDA actually program on this machine? (Phase 1.)
2. How does the companion coexist with AppleHDA's ownership of GCTL, INTCTL and the single MSI vector? (Phase 1/2.)
3. DMA mapping/IOMMU on Tahoe for a driver that is not the controller's primary driver. (Phase 2.)
4. Who raises `PGCTL/CGCTL` and the D0 power state of the shared function, and will AppleHDA fight it? (Phase 2.)
5. Are the DMIC IP registers firmware-private on IPC3? (I — confirmed only by absence in the host driver.)
6. IPC3 firmware is old; is a newer IPC4 firmware + NHLT path preferable? (Later.)
7. Final macOS audio class on Tahoe: IOAudioFamily vs AudioDriverKit. (Phase 5.)

---------------------------------------------------------------------------------------------------

## Appendix A — decode artefacts (reproducibility)
Scratchpad scripts (`python3 -I`): `nhlt_parse.py`, `tplg_dmic.py`, `tplg_dmic2.py`, `tplg_w.py`, `tplg_w2.py`, `sizes.c` (struct sizes and
command words, built with gcc against `sof-linux/include` + stubs), ext-manifest decoder (inline). Outputs summarised above; re-run
against `phase0-out/NHLT.bin` and the installed topology/firmware to verify.


## Correction 2026-11-06 (Phase 3 step 3.0/3.1 work)
- §7: the IPC3 **reply is read from the host mailbox (DOWNBOX, 0xA0000)**, not the DSP->host mailbox: `sof_ipc3_get_reply` reads `sdev->host_box.offset` (`ipc3.c:242`).
- §6: FW_READY carries no extended data on this firmware; the window table comes from the .ri extended manifest (`ipc3-loader.c:190`). Confirmed on hardware, see `2026-11-06-phase3-step0.md`.
