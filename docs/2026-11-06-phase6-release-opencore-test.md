# 2026-11-06 — Phase 6: 6.1 checks, firmware download, OpenCore-only test, 0.3.0 release package

Machine: ThinkPad P1 Gen 3 (Comet Lake-H, PCI `8086:06c8`), macOS 15.8.2, OpenCore, AppleALC `alcid=31`. Status: **working fine so far**
(one OpenCore boot of the release build, observed over the test session; no long-run, sleep/wake or multi-boot data for this build yet).

## 1. 6.1 verification (`tools/verify-6.1.sh`, manual `kmutil load -p` build)

| Check | Result |
|---|---|
| Baseline, 5 s recording through daemon + driver | PASS: 239104 frames (expected ~240000), 2 channels, audio present |
| Daemon killed during a 12 s recording | PASS: recording ended on its own (no hang), DSP stopped (`DMA stop OK, stream free clean, DSP down, 2 ms`), launchd restarted the daemon, a new recording worked |
| Kext unload / reload (40 s wait for the DSP) | PASS: recording works after the reload |
| Idle sleep / wake (`pmset sleepnow`, run by the script) | **FAIL, machine did not wake** from the power button; hard power cycle needed. See section 2 |

## 2. Failed wake after `pmset sleepnow`

- macOS logged `Sleep Wake failure in EFI`, failure code `0x53425348 0x0000001f` (report `Sleep Wake Failure_2026-11-06-132230_*.diag`), i.e. the firmware/EFI
  stage of resume, before any driver runs. The report's own stackshot is to be ignored (EFI failure).
- Last OhMyDSP activity before the sleep was a clean `engine stop`. Boot args at the time: `-v debug=0x100 keepsyms=1 alcid=31 -igfxblt -igfxbls -igfxmlr -ohmydspdbg`.
- **Cause not established.** OhMyDSP is not ruled out and not implicated. A control (several sleep/wake cycles without the kext, then with it) was not run.
- Earlier sleep/wake results (Phase 5.5, and a Voice Memos recording across `pmset sleepnow`) were fine, so this is one failure against several passes.
- Change made: the sleep step of the script is now opt-in (`SLEEP=1`); sleep tests are never run unannounced.

## 3. Pane additions

- **Misc > Collect Logs…**: plain-text report (versions, CPU, PCI ID/verdict, boot-args, kext state, installed files, last 2 h of OhMyDSP log lines; no audio) saved where the user chooses.
- **Hardware row**: IORegistry read of the Intel audio controller (vendor `8086`, device ID, class). Class `0x0401` = DSP enabled, `0x0403` = DSP hidden (HDA mode).
  Verdicts: supported (only `06c8`), not supported yet, no SOF firmware (Skylake/Kaby Lake IDs), DSP off, unknown. Only `06c8` is verified on hardware; the other IDs come from public lists.
- **Firmware download with progress bar** (Install… downloads first if the firmware is missing), see section 4. Installed to `/Library/Application Support/OhMyDSP/`
  (kext paths moved from `dmic-macos/`). The pipeline blob `dmic-p10.ipc` (1584 bytes) now ships inside the pane.
- Uninstall removes the daemon, driver, firmware and blob.

## 4. sof-bin findings

- The firmware used so far (`sof-cnl.ri`, 557824 bytes, SHA-256 `c5119cfc…359e`) is byte-identical to `sof/intel-signed/sof-cnl.ri` in sof-bin releases 2025.05, 2025.05.1,
  2025.12, 2025.12.1, 2025.12.2, 2026.09, 2026.09.1 and 2026.09.2. The `community/` build has the same size but a different hash and is not what we use.
  `sof-cml.ri` is a symlink to `sof-cnl.ri`.
- Pinned download: release `v2026.09.2`, `sof-bin-2026.09.2.tar.gz`, 17550180 bytes, SHA-256 `5f7bc30b…43e1`. Both the tarball and the extracted image are checked by the pane; the installer re-checks the installed copy.
- `intel-signed/` has apl, glk, cnl/cml, icl, tgl, tgl-h (IPC3) and IPC4 variants for tgl/tgl-h; no Skylake/Kaby Lake image. SOF documentation says those use `snd-soc-skl`.
- A headless test of detection and download on this machine: platform `Comet Lake-H 8086:06c8 class 0x401 verdict supported`, download 0 to 100 %, both checksums OK.
- The kext reads firmware and blob once, at start (boot + `ohmydsp_delay`), so the first install needs one restart. The kext still hard-codes the firmware size (`FW_SIZE_EXPECTED = 557824`).

## 5. NHLT access (research, nothing implemented)

- macOS exposes no ACPI tables in the IORegistry (`NHLT`, `DSDT`, `SSDT` not found as properties), so the pane cannot read the NHLT.
- `IODeviceTree:/efi/configuration-table` is present; a kext can map the ACPI root from it and walk `RSDP → XSDT → NHLT`. Planned for the next phase.
- Linux capture of this machine (`NHLT.bin`, 6350 bytes): 2 DMICs. On IPC3 the NHLT mainly confirms mic count and format; the pipeline comes from the topology.

## 6. OpenCore-only test (release 0.3.0, USB EFI, no `kmutil load`)

Setup: macOS-side traces removed first (daemon, plist, driver, old pane, staged kext copy); kext `OhMyDSP.kext` (Lilu-plugin build, version 0.3.0) copied to the USB's `EFI/OC/Kexts`,
one `Kernel > Add` entry after Lilu (`config.plist` backed up as `config.plist.bak-before-ohmydsp`). A first PlistBuddy edit added empty junk entries; it was restored from the backup and redone with Python's plist module.
OpenCore settings of this machine: `DisableLinkeditJettison` and `DisableIoMapper` true, `SecureBootModel` Disabled (none of these is shown to be required).

Observed:
- `kmutil showloaded`: `com.ohmydsp.OhMyDSP (0.3.0)`, kext index 95, linked against Lilu and the usual kpi set: injected by OpenCore, not loaded by macOS.
- `kernelmanagerd` logged `Refusing new kext com.ohmydsp.OhMyDSP … a loaded copy with a different executable UUID is already present` and `missing bundle … /Library/Application Support/dmic-macos/kext/…`:
  benign, macOS remembering the earlier manual `kmutil load -p`. No double load.
- The kext's start-up lines are not reliably visible in `log show` (early boot); they are in the IORegistry property `phase1-log`: BAR0/BAR4 mapped, `DSP core0 not powered` while idle,
  `Phase 3 blob: 19 messages, 1584 bytes -> OK`, `firmware: magic=0x6e614d58 ext_man_size=0x300 … -> OK`, DMA ring 131072 bytes with all IOVAs below 4 GB, `live capture ready: DSP idle, powers up when the user client starts the stream`.
- Fresh install through the pane (admin password, no terminal): Hardware `Comet Lake-H`, firmware downloaded with progress bar and installed, service running, driver active, microphone ready.
  Firmware folder: `sof-cnl.ri` 557824, `LICENCE.Intel`, `dmic-p10.ipc` 1584, `ohmydspd`. The old `dmic-macos` folder is gone.
- System Settings > Sound > Input lists *Internal Microphone (DMIC)* with a moving level meter; pane input slider at +13.1 dB. Screenshots in `docs/images/`.
- The device name is truncated in the Sound list column ("Internal Microphone (DMI…"); cosmetic.

## 7. Release package

`tools/make-release.sh` builds kext (CORE, Lilu), userspace and pane, checks that kext and pane versions match (both 0.3.0), and writes `dist/OhMyDSP-0.3.0.zip` (370 KB:
kext, pane with daemon + driver + blob, LICENSE, INSTALL.txt) and `dist/SHA256SUMS`. The zip tested is the one whose kext was on the USB.

## Open items

- Push and GitHub release (tag `v0.3.0`) are not done.
- Not yet tested for the OpenCore build: sleep/wake, several consecutive boots, recording longer than a few minutes, Voice Memos recording of this run.
- The failed wake in section 2 is unexplained; a with/without-kext control run is still missing.
- Pane and ad-hoc signed driver/daemon are not notarized (Gatekeeper workaround in INSTALL.txt).
- Next: kext NHLT reader (read-only), pane check "do the mics use the DSP", per-machine profile file, pipeline chosen from NHLT values.
