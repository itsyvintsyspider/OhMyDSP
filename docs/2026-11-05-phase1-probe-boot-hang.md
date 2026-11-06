# 2026-11-05 — Phase 1 probe: boot hang when injected via OpenCore (USB EFI)

Status: **open**. Probe is currently DISABLED in the USB config. Internal EFI untouched throughout.

## Setup
- Probe: `dmic-probe/` (read-only; PCI config reads + BAR0/BAR4 MMIO reads). Injected via `Kernel > Add` on the USB EFI
  (`/Volumes/OC-EFI/EFI/OC`), deviating from the CLAUDE.md "manual `kmutil load -p` first" rule at the user's request.
- Machine: ThinkPad P1 Gen 3, macOS reports 15.8.2 (Darwin 24.6.0), OpenCore REL-109.

## Timeline (all 2026-11-05, local time)
| Time | Build | Result |
|---|---|---|
| ~22:16 | `kmutil load -p` from /tmp | refused: needs user approval + restart (staged); no probe output |
| 22:37 | v0 (os_log) via OC USB | booted fine; probe attached; **no log output visible** |
| 22:42 | v1 (os_log+IOLog) via OC USB | booted fine; no output in unified log |
| 22:44 | v2 (os_log + IORegistry `phase1-log`) via OC USB | **machine reset right after the Apple logo**, no panic data in NVRAM |
| ~22:50 | v2 again, USB config now `-v debug=0x100` | **hard hang** (no panic text) — see below |

## Evidence from the verbose-boot photos
Last lines on screen (both photos identical):
`ApplePS2Controller: Timed out on input stream 1.` → `[ PCI configuration begin ]` → `[ PCI configuration end, bridges 11, devices 18 ]`
→ `SMCSuperIO ssio: @ starting up SuperIO sensors` — and then nothing. No `panic(cpu …)`, no backtrace, **no `DMICProbe:` line**.
With `debug=0x100` a panic would have stayed on screen, so this is a hang, not a panic. The 22:44 reset was probably the same hang
ended by a platform watchdog.

## Facts learned
- Kernel `IOLog`/`os_log` lines from kexts do **not** reach the unified log on this system (other kexts' `IOLog` lines such as
  `ApplePS2Controller:` are on the verbose console only). So empty `log show` results never proved the probe did not run.
- USB OpenCore here writes no log files (`Misc>Debug>Target=3`, release build); `ApplePanic=false`, `PanicNoKextDump=true`.
- Touchpad SMBus/RMI test failed independently: `VoodooSMBus` 2.2 only matches PCI ids 9d23/a323 (this machine's SMBus is 06a3) and
  `RMISMBus` needs `VoodooSMBus` >= 3.0. Reverted to PS/2 trackpad (`VoodooPS2Trackpad` enabled).

## Hypotheses (unproven)
1. **DSP SRAM read while DSP is power-gated.** The probe read `BAR4+0x80000/0x80004`. SOF treats that memory as D0-only
   (`SOF_DEBUGFS_ACCESS_D0_ONLY`); reading it with the DSP off can stall the bus. Earlier boots may have returned early (before
   AppleHDAController enabled memory decode) and so never reached the read. Counter-evidence: no `DMICProbe:` console line before the hang.
2. **Race with AppleHDA initialisation** (reads while the controller is mid-reset).
3. **Not the probe at all** (e.g. an effect of `-v`/`debug=0x100`, or SMCSuperIO). Only a control boot can exclude this.

## Changes made
- Probe hardened: SRAM window is read only if `ADSPCS.CPA` (bit 24) is set and the register is not all-ones.
- USB `config.plist`: `DMICProbe.kext` **disabled** (state saved as `logs/config.plist.usb-hang-test-2026-11-05`).

## Next (one variable at a time)
1. Control: boot USB with probe disabled, `-v debug=0x100` kept. Hang => probe is innocent; investigate elsewhere (SMCSuperIO, flags).
   Boots => probe implicated.
2. If implicated: enable the hardened probe with `-v` on. The last `DMICProbe:` line on screen locates the hang.
3. If it still hangs, drop SRAM/BAR4 reads entirely (read PCI config + BAR0 only) and re-bisect.

## Update 23:10 — control boot + hardened-probe boot
- Control (probe disabled, `-v debug=0x100`): **booted to macOS fine** → `-v`/`debug=0x100` are not the cause.
- Hardened probe (SRAM read gated on ADSPCS.CPA) enabled, same flags: **hung again**, last console line `Previous shutdown cause: 5`
  (after IntelBTPatcher/AMFIPass lines), no `DMICProbe:` line. So the SRAM read was not the (only) cause; the earlier SMCSuperIO
  stop point differs, so the hang is probably timing-dependent rather than at one fixed spot.
- Probe history: v0 (os_log only) and v1 (os_log+IOLog) booted fine with the probe enabled; v2 (adds `record()` + `setProperty`
  of an OSArray) and the hardened v2 both hang. The only code delta between "boots" and "hangs" is the IORegistry-property logging
  (plus the SRAM gate). Strongest suspect: that logging path, or probing too early while AppleHDAController initialises.
- Probe disabled again in USB config.

## Update 2026-11-06 — root cause found: panic in the probe's own logging
Delayed-probe build reached the lock screen, then panicked ~32 s after boot:
`panic: Trying to change a collection in the registry @OSCollection.cpp:70`, backtrace
`OSArray::setObject <- DMICProbe::record <- logConfig <- runProbe <- probeEntry` (kext in backtrace: com.dmicmac.DMICProbe).
Cause: v2 called `setProperty("phase1-log", logBuf)` and then kept appending to `logBuf`; a collection already in the registry is
immutable. Fix: append to a private array, publish a **copy** once at the end (`publishLog()`).
Consequence: the earlier boot hangs/resets with v2 builds (probing in `start()`) are very likely the same bug (panic during early boot
with the display stuck/reset) — not proven. The register-reading code itself had not yet been shown to misbehave: the panic occurred
on the first log lines (PCI config dump), before any BAR read. Delayed probe kept (harmless, avoids the AppleHDA init race).
