# Raptor Lake system input on the Lenovo reference machine

The experimental IPC4 session was validated on a Lenovo 21JK004DCD with controller `8086:51ca` and subsystem `17aa:50d1`. The preference pane shows this model's codec and DMIC details when both IDs match. Its status view reports firmware and graph hashes, DSP readiness, daemon and HAL availability, and the startup receipt for the current boot.

## Session build and assets

From the repository root, build the session kext, daemon, HAL and CoreAudio test client:

```sh
make -C ohmydsp LILU=0 CORE=1 RPL=1 LIVE=1 MANUAL=1 SESSION=1
make -C ohmydsp/userspace
./tools/build-coreaudio-stability.sh
```

The session needs the signed RPL firmware and an IPC4 capture graph generated from the matching topology and the machine's NHLT table. [The asset installer](../tools/install-rpl-assets.sh) checks the release archive and firmware hashes. [The graph installer](../tools/install-rpl-ipc4-module-plan.sh) accepts the table path through `NHLT`; the tested graph uses `--eq-config`. Firmware, extracted vendor data, generated IPC4 plans and recording files stay local.

On the reference machine, [the manual session loader](../tools/load-rpl-ipc4-session-manual.sh) stages the built kext. Complete macOS approval if prompted and verify the loaded kext UUID before installing the daemon and HAL with [the system-input installer](../tools/install-rpl-system-input.sh). Both the userspace and startup installers print a backup path and rollback command. Recording starts when a client opens the input. The final STOP releases the DMA stream and powers down the DSP.

## Startup and reboot qualification

First qualify repeated recording and idle restoration using the [CoreAudio stability tools](coreaudio-stability-tools.md). The startup installer checks the eight [pinned assets](../tools/rpl-startup-pins.sha256), the loaded kext UUID and the stopped DSP state. A rebuilt or different binary needs its own manual and reboot qualification.

On a system matching that baseline, close microphone readers, run `sudo ./tools/install-rpl-startup.sh`, then run `python3 tools/validate-reboot-input.py before` as the logged-in user. Set `BASELINE` to the output directory reported by the before check.

After a normal reboot, before opening any microphone reader or Sound input settings, run:

```sh
python3 tools/validate-reboot-input.py after --baseline "$BASELINE"
```

The one-shot loader records `loaded` when it loads the pinned kext and `already_loaded` when it finds that UUID running. Reboot qualification requires a new boot UUID, matching asset and startup configuration hashes, a successful `loaded` receipt for that boot, and a ready, idle DSP at session 0. The verifier then opens the input for the first recording.

## Verified baseline after the snapshot fix

On 2026-11-24 the startup loader loaded IPC4Session `74FA5895-CC4F-3FAD-8488-3D7C0F770DDD` and exited successfully. The eight installed assets and three startup configuration files matched the pre-reboot baseline. The first CoreAudio recording delivered 483840 frames in 945 callbacks over 10.054 seconds. The timestamp period was 131072 frames before and after capture. HAL snapshot-failure frames, timestamp discontinuities and daemon overruns were all zero. Session 1 stopped and restored idle.

The previous userspace pair lost 512 frames when a HAL read overlapped a position update. With the snapshot fix, six manual sessions restored idle with zero snapshot-failure frames. Two of those sessions carried warnings: one recorded five full-scale samples per channel and another skipped one unstable kernel status read. The final fresh-boot recording passed all checks. Its initial broad log query timed out; the counters were recovered from the same recording using the kernel session number and diagnostic ID. No PCM was saved during these qualification runs.

## November 26 candidate

IPC4Session `2C2DF9E1-4F38-32B3-B31B-54D151936299` completed four ten-second CoreAudio sessions on the RPL reference machine. All four restored DSP idle with zero timestamp discontinuities, daemon overruns and HAL snapshot-failure frames. Sessions 2 and 4 reported a diagnostic warning because the daemon skipped an unstable kernel position snapshot. The reboot check matched the pre-reboot hashes and kext UUID, recorded a successful `loaded` receipt, and found the DSP ready at session 0. The first recording delivered 482304 frames with 0 timestamp discontinuities and passed the strict diagnostics checks.
