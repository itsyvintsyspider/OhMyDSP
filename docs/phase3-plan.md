# Phase 3 plan — IPC mailbox + DMIC pipeline setup (up to PIPE_COMPLETE; no host DMA, no TRIG_START)

Date: 2026-11-06. Status: **PLAN ONLY. No Phase 3 code written, nothing enabled.** Every IPC send is a hardware write: stop and ask first (CLAUDE.md).
Builds on: `bringup-recipe.md` §6–§9, `2026-11-06-phase2-live.md` (DSP boots to FW_READY, FW 2.2.0, ABI 3:22:1).

## Goal / done-definition
With the DSP held up for a bounded time, send the IPC3 messages Linux sends to build **pipeline 10** (DMIC0.IN -> BUF -> EQIIR -> BUF -> volume -> BUF -> host PCM6C)
plus `DAI_CONFIG` for the DMIC DAI, and get `error = 0` for each reply up to and including `PIPE_COMPLETE`. Then tear down (core reset + power-down).
Not in Phase 3: `PCM_PARAMS`, host DMA stream for capture, `TRIG_START` (Phase 4).

## Design decisions
1. **No topology parser in the kext.** An offline tool (`tools/gen-ipc3-dmic.py`, runs on the Mac) reads `sof-hda-generic-2ch.tplg`, builds the IPC3 messages
   for pipeline 10 + DAI_CONFIG (mirroring `ipc3-topology.c`: `sof_ipc3_widget_setup`, `sof_link_dmic_load`, `sof_ipc3_dai_config`, route setup, `complete_pipeline`)
   and writes a flat script blob: `[u32 len][u32 expect_reply_len][msg bytes]...`. The kext only replays it. Blob lives in
   `/Library/Application Support/dmic-macos/dmic-p10.ipc`, **not committed** (derived from Intel-licensed topology).
2. **Polling only**, as in Phase 2: ADSPIC/HIPCCTL stay 0, so no DSP interrupt can reach AppleHDAController's vector. IPC done and DSP->host notifications
   (position/trace) are polled and acked; unacked DSP->host messages can stall the firmware.
3. **Bounded hold, never sleep with the DSP up.** DSP is kept powered only for `dmic_p3_hold` seconds (default 30), then torn down by core reset + power-down
   (restores the Phase 1 idle state, `ADSPCS == 0x00000f0f`). No DSP state survives; no sleep/wake handling needed in this phase.
4. **Incremental, one variable per boot**, gated by boot-args (all default off; `dmic_off=1` still kills everything):
   `dmic_p3=2` dry run (prints message list from blob, no hardware), `dmic_p3=1` live; `dmic_p3_n=N` = send at most N messages; `dmic_p3_hold=S`.
   Phase 3 live requires `dmic_p2=1` (it reuses the Phase 2 boot path, then continues instead of powering down).

## Step ladder (each = one reboot, each ends with log + dated note before the next)
| Step | What | Hardware effect | Pass criteria |
|---|---|---|---|
| 3.0 | Boot FW as Phase 2, **hold**, dump raw mailbox 0x81000..0x81400 (FW_READY + ext data at +108, `ipc3.c:854` `ipc3_fw_parse_ext_data`, windows `ipc3_get_windows`), dump DOWNBOX 0xA0000 head, then teardown | no IPC sent | Window table (dsp_box, host_box, stream, debug offsets/sizes) read from the firmware itself and compared with the ext-manifest values in recipe §6 |
| 3.1 | Transport proof: send **one** message, poll HIPCIDA.DONE, read reply from dsp_box, ack. Candidate = `PM_CORE_ENABLE` core 0 if Linux sends it on this path (check `ipc3.c` ~1080–1100 first); else `PIPE_NEW` is the first real message. | 1 IPC | reply.error == 0, hdr.size sane, no panic magic (0x0dead000) in mailbox |
| 3.2 | `dmic_p3_n=1..k`: replay the blob prefix: PIPE_NEW, COMP_NEW (DAI), DAI_CONFIG | k IPCs | error == 0 each |
| 3.3 | Rest of pipeline 10: BUFFER_NEW/COMP_NEW x N, COMP_CONNECT x6, PIPE_COMPLETE | ~15 IPCs | PIPE_COMPLETE reply error == 0 |

**Abort rules (every step):** reply.error != 0, timeout (500 ms, `TIMEOUT_DEFAULT_IPC_MS`), reply size mismatch, or panic magic in the mailbox -> log the raw
mailbox + ROM_STATUS/ROM_ERROR, stall+reset cores, clear SPA, restore PGCTL/CGCTL/EM2, stop. Never retry a failed message.

## Offline work (no hardware, can be done and reviewed first)
- `tools/gen-ipc3-dmic.py` + offline checks: struct sizes asserted against the compiled sizes in recipe §7 (e.g. `sof_ipc_dai_config`=216, `sof_ipc_comp_dai`=80,
  `sof_ipc_pipe_new`=48), DMIC params checked against recipe §9 table (fifo_fs 48000, 2 mics, PDM clk 2.4–4.8 MHz, duty 40–60).
- Output includes a human-readable hex dump of every message for review **before** any is sent.
- Source files: `sof-hda-generic-2ch.tplg` from sof-bin v2026.09.2 `sof-tplg/` (44247 B, sha256 7896ab52…3c53). Same size as the file Linux loaded on this laptop
  (`phase0-out/dmesg-sof.txt`: "intel/sof-tplg/sof-hda-generic-2ch.tplg", topology ABI 3:22:1); byte-identity with the Arch copy is **not** proven (only size known).
  Same Intel licence as the firmware.

## Risks / unknowns
- **Payload bytes are [I]-level (inferred).** Arch is gone, so there is no reference trace. Fields like core, component UUID/extension, buffer caps, and the exact
  message order come from reading `ipc3-topology.c` + the decoded topology. A wrong field -> firmware error reply or a DSP-side panic. DSP-side failure is isolated
  to the DSP (recoverable by core reset); a host-side hang is still possible.
- **Mailbox window layout** is unverified until step 3.0. Messages go to host_box (expected 0xA0000), replies come from dsp_box (expected 0x81000).
- **What Linux sends *besides* pipeline 10** at boot/prepare (e.g. static pipelines, core power messages, `PM_CTX`, DAI_CONFIG repeats with HW_PARAMS flags) is not fully
  traced. Pipeline 10 alone may be rejected if the firmware expects something earlier; we find out incrementally.
- DMIC pad/pin muxing on the PCH side: Linux does nothing host-side on IPC3, but this has never been checked on hardware.
- Longer DSP-on time than Phase 2: PGCTL/CGCTL/EM2 stay changed during the hold; a panic in that window leaves them changed until reboot/D3.

## Strongly recommended (not blocking 3.0/3.1)
A **Linux live USB booted without OpenCore** would give a real IPC3 trace (`sof_debug` payload dump) and register dumps from this exact machine, turning §8 of the recipe
from inferred to verified before step 3.2. Cost: one USB stick and ~30 minutes; no install, read-only.

## Approvals needed
1. This plan (design + ladder). 2. Each live step separately ("go live" for 3.0, then 3.1, ...). 3. Optional: the Linux live USB capture.

## Progress log
- 2026-11-06: plan approved by user. Offline tool work started (no hardware touched).
  - `tools/tplg.py` (read-only .tplg decoder) done and verified on `local/sof-hda-generic-2ch.tplg` (sha256 7896ab52…3c53, in gitignored-by-convention `local/`):
    76 widgets, 42 routes parsed with exact byte accounting; pipeline-10 widgets and tuples match recipe §8/§9.
    Gotcha found: a mixer/enum/bytes control's `ctl_hdr.size` covers only the header; the real struct size is the u32 right after it.
  - Next: `tools/gen-ipc3-dmic.py` — pack the messages per `ipc3-topology.c` (`sof_comp_alloc` l.367 + uuid ext, `*_setup_comp_{pipeline,buffer,dai,pga,host}`,
    token tables l.73-330, `sof_ipc3_dai_config` l.2186, route l.1780, complete l.2131, EQIIR bytes control -> process data), comp_id/core assignment,
    then a human-readable hex dump for review before any kext code.
- 2026-11-06 (later): read the packing path in `ipc3-topology.c`. Findings that change the generator scope:
  - Messages are built from token tables (l.73-330) applied to the widget tuples; component id/core/pipeline_id come from `sof_comp_alloc` (l.367), UUID is appended as ext data
    (`ext_data_length`=16) when non-null. Every message is sent via `sof_ipc_tx_message_no_reply` (reply = plain `sof_ipc_reply`, 12 B).
  - **DAI_CONFIG is not built from the widget.** It comes from the BE DAI *link* block (`SND_SOC_TPLG_TYPE_BACKEND_LINK`: hw_config[] formats + DMIC/PDM tuples,
    `sof_link_dmic_load` l.1502), and is sent with the HW_PARAMS flag only after the widget exists (`sof_ipc3_dai_config` l.2186). `tplg.py` does not parse link blocks yet.
  - **Extra messages beyond the widget list:** Linux also pushes initial *kcontrol* values after widget setup (volume/switch for `Dmic0`, the EQIIR `eqiir_coef_10`
    bytes blob via ctrl-data IPC) — `sof_ipc3_control_load_*` l.1803-1900 and the bytes data in `sof_get_control_data`. These are `GLB_COMP_MSG` ctrl-data messages not yet in §8.
  - Order is set by the widget-list walk in `sof-audio.c`, not by the token order; comp_id values are assigned at topology load (need to confirm the numbering rule in `topology.c`).
  Remaining generator inputs: struct layouts (`include/uapi/sound/sof/{topology,dai,dai-intel,header}.h`), enums (comp/frame fmt/dai/process type), link-block parser,
  comp_id rule, control-data message format.

- 2026-11-06: **step 3.0 done** (see `2026-11-06-phase3-step0.md`): FW_READY carries no ext data; windows come from the .ri ext manifest. Step 3.1 should parse them from the firmware file.

- 2026-11-06: **step 3.1 done**: PIPE_NEW(pipeline 10) accepted, error=0 (see `2026-11-06-phase3-step1.md`). Next: 3.2 needs the generator (comp ids known: PCM6C 50, Dmic0 51, EQIIR10.0 52, BUF10.0 53, BUF10.1 54, BUF10.2 55, DMIC0.IN 56, PIPELINE.10 57).
- 2026-11-06: **generator written** (`tools/gen-ipc3-dmic.py`, `tools/tplg.py`): 19 messages for pipeline 10 (PIPE_NEW, DAI comp + DAI_CONFIG, BUF10.2, EQIIR10.0 + SET_DATA,
  BUF10.1, Dmic0 + vol/switch SET_VALUE, BUF10.0, PCM6C, 6x COMP_CONNECT, PIPE_COMPLETE). Message 1 is byte-identical to the PIPE_NEW accepted in 3.1.
  Output blob `local/dmic-p10.ipc` (sha256 2e37e042…b9bf at generation time; not committed, Intel-licensed topology), readable dump `local/dmic-p10.dump.txt`.
  Fixes found while building it: `sof_abi_hdr` is 32 B (no comp_id); `fifo_bits_b` stays 0 on ABI >= 3.0.1. Message order of the 6 COMP_CONNECTs is [I].
  Kext now replays the first N messages of the blob (`dmic_p3_n=N`), validating the blob before any register write. Ladder: N=3 -> through DAI_CONFIG (3.2),
  N=12 -> all components + controls (3.3), N=19 -> connects + PIPE_COMPLETE (3.4).
- 2026-11-06: **step 3.2 done** (N=3): PIPE_NEW, DAI COMP_NEW, DMIC DAI_CONFIG all error=0 (`2026-11-06-phase3-step2.md`). Next 3.3 = N=12.
- 2026-11-06: **step 3.3 attempt 1**: msgs 1–5 ok, msg 6 (EQ SET_DATA) error -22 — generator used an 80 B ctrl_data header, real size is 92 B. Fixed, blob now 63ebf9f2…bede (1584 B). See `2026-11-06-phase3-step3.md`. Retry N=12 pending approval.
- 2026-11-06: **step 3.3 done** (attempt 2, N=12): 12/12 error=0 after the ctrl_data 92 B fix (`2026-11-06-phase3-step3b.md`). Next 3.4 = N=19 (connects + PIPE_COMPLETE), pending approval.
- 2026-11-06: **step 3.4 done, Phase 3 complete**: 19/19 error=0 incl. 6 COMP_CONNECT and PIPE_COMPLETE (`2026-11-06-phase3-step4.md`). Next: Phase 4 plan.
