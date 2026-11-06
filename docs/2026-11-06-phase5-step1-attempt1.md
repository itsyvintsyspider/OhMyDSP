# Phase 5 step 5.1, attempt 1 — 2026-11-06 — kext not loaded (OpenCore injection cannot link IOAudioFamily)

Config: `dmic_audio=1`, kext injected by OpenCore as before. Result: **no "Internal Microphone" and no meter** because `com.dmicmac.DMICProbe` was not loaded at all
(`kmutil showloaded` has no entry, no `com_dmicmac_DMICProbe` in ioreg, no log lines; no panic).

## Cause
`kmutil libraries` shows `com.apple.iokit.IOAudioFamily` lives in `SystemKernelExtensions.kc`. OpenCore injects into the **Boot KC**, and a Boot KC kext cannot link against a
System KC kext, so the injected kext is silently dropped as soon as it declares that dependency (our earlier builds only linked kpi/IOPCIFamily, which are in the Boot KC).
Not fixable with OpenCore `Kernel -> Force`: the macOS 15 on-disk `IOAudioFamily.kext` has no executable (`Contents/MacOS` missing, the code exists only inside the KC).

## Decision
From Phase 5 on the kext is loaded manually (this is also what CLAUDE.md prescribes for test builds): `sudo sh tools/load-kext.sh` stages a root-owned copy under
`/Library/Application Support/dmic-macos/kext/` and runs `kmutil load -p`; `sudo sh tools/load-kext.sh unload` removes it. No reboot is needed to iterate, only after a panic.
The OpenCore Kernel>Add entry (index 38) is disabled on the USB config (backup `logs/config.plist.usb-before-manual-load-2026-11-06`).
boot-args still carry the `dmic_*` switches (`PE_parse_boot_argn` reads them at load time too).
