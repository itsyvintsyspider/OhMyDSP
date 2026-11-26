# CoreAudio stability and IPC4 diagnostics

These tools exercise the installed **Internal Microphone (DMIC)** through real
CoreAudio IOProcs. They do not install/reload drivers, change defaults or gain,
access the exclusive kernel user client, or modify firmware/EFI. No new kext
approval or reboot is required. Build and run as the logged-in user, without
`sudo`. The terminal/tool may require microphone permission in macOS.

The daemon and HAL emit per-session counters for missing frames, clock snapshots and pump timing. Use `--require-hal-diagnostics` to require both stop summaries. The runner correlates them in `audio-diagnostics.json`. IORegistry-only exports cover the kernel session.

## Start with a short recording

Close recording apps and Sound > Input (the level meter can keep input active).
From the repository root:

```sh
./tools/build-coreaudio-stability.sh
python3 tools/run-coreaudio-stability.py smoke --save-sample
```

Speak during the 10-second test. If macOS asks for microphone access, allow the
terminal/tool. A timeout while approving is a failed test; rerun after approval.
Do not use sudo to work around microphone permission. `--save-sample` saves at
most the first 10 seconds per client as stereo Float32 WAV, including startup.
Omit it to save statistics without audio. Reports/samples belong to the invoking
user, in a new mode-0700 directory under `diagnostics/coreaudio-*`; files use 0600.
Output directories and individual reports cannot overwrite prior runs.

The tool prints the exact output directory. `summary.json` contains each round's
result and client report. Each round preserves IORegistry snapshots before and
after, including the four bounded host/firmware trace slots. The final unified
log export is best effort; errors/timeouts are recorded. Reports use UTC timestamps.
Recorded speech and machine logs stay in the ignored diagnostics directory.

## Qualification modes

Run separately, reviewing the previous result first:

```sh
# Twenty independent 5-second start/stop cycles, stopping on the first FAIL.
python3 tools/run-coreaudio-stability.py cycles

# Continuous 10-minute input; bounded memory, no recording file by default.
python3 tools/run-coreaudio-stability.py long

# Two independent processes reading the same device; B stops while A continues.
python3 tools/run-coreaudio-stability.py overlap
```

Options: `cycles --cycles 2..50 --seconds 2..60`, `long --seconds 60..1800`,
`smoke --seconds 2..60`, `--output NEW_DIRECTORY`, `--require-hal-diagnostics`, and `--save-sample`.
Overlap uses fixed timings: A records 12 seconds, B starts about 2 seconds later
and records 4 seconds. It requires the DSP to remain running after B exits,
then stop cleanly after A exits; normally this produces one kernel session.
It checks shared CoreAudio ownership, not the UI behavior of every recording app.

Other readers can invalidate an isolated test. The runner refuses to begin when
the IPC4 session is already running; if another app starts during a test, it may
report a session-count or final-state mismatch. It never stops that other app.
Normal speaker playback can be used concurrently for a later coexistence test;
the tool does not itself play sound or prove speaker correctness.

Ctrl-C requests child shutdown, final snapshots and report saving. A client with
no frame progress for 5 seconds stops; the runner also enforces an external
process deadline, terminates/kills stuck children and preserves incomplete
reports. A timeout/failure is never converted into PASS. For long tests, the
terminal remains occupied until completion; no automation/background job is added.

## What results establish

The native client selects the exact UID `com.ohmydsp.InternalMicrophone`, checks
48 kHz interleaved stereo Float32, and refuses other formats. The realtime
callback does no file IO, allocation or logging. Storage is bounded to statistics
and an optional 3.84 MB sample buffer. Statistics are read after callback teardown.

- **FAIL**: device/setup/start/stop failure, interrupted/stalled process, malformed
  buffers, invalid or discontinuous input timestamps after the first 0.5 seconds,
  nonfinite samples, frame/wall ratio outside 0.95–1.05, missing report, incorrect
  kernel-session transition, missing cleanup evidence, firmware error/xrun or
  nonzero scheduler overruns.
- **WARN**: a continuous exactly-zero segment of at least 1 ms after warmup, full-scale samples, callback gap
  above 100 ms, unknown firmware warning, or bounded trace/log coverage limits.
  A WARN exits zero but is not an unqualified stability pass. Review JSON and
  listen to an optional sample before continuing.
- **PASS**: all observed checks pass within this test's limits.

The successful baseline firmware warning ending in
`ipc_pipeline_complete(): no scheduling component specified, use comp 0x20004`
is retained as `expected_scheduler_fallback`, not treated as a new error.

Frame/timestamp continuity is measured at the CoreAudio client. Zero audio may
mean natural digital silence, mute or upstream missing samples; it cannot be
classified conclusively from samples alone. Likewise, continuous timestamps do
not prove the absence of upstream duplicated/missing audio. The original build
has no public HAL missing-frame/daemon-copy counters; the optional diagnostic
build measures availability bounds and supply positions, not PCM content identity. Start milliseconds are
setup latency, not measured acoustic latency. Peak/RMS/clipping statistics after
warmup describe the HAL output, including its existing gain/channel policy.
The report retains up to 64 timestamp-jump/zero-run events with recorded frame positions and signed jump sizes; additional events are counted as omitted. Single isolated zero samples remain statistics, without a warning, because they can occur naturally.
The 5% frame/wall tolerance is a gross transport check, not clock-drift certification.

Trace archives remain bounded at 64 KiB and four sessions in the loaded build.
Per-round snapshots preserve each completed session before later rounds replace
it, but long sessions can fill an archive; this is reported as limited coverage.
These tests do not qualify sleep/wake, forced process crash recovery or startup
persistence. Those require a separate lifecycle implementation/test phase.

## Read-only diagnostic export

No microphone opens and no administrator password is needed:

```sh
python3 tools/session-diagnostics.py diagnostics/session-snapshot-NEW
# Set COLLECTION to a saved diagnostic directory to parse its IORegistry snapshot:
python3 tools/session-diagnostics.py diagnostics/session-analysis-NEW \
  --source "$COLLECTION/ohmydsp-ioreg"
```

Exports include state/number/bundle, start and stop evidence, idle restoration,
firmware trace validity/coverage, firmware errors/warnings and scheduler overruns.
They cannot read a root-only source directory without the owner granting access.

## Offline verification

```sh
./tools/build-coreaudio-stability.sh
python3 tools/test-coreaudio-stability.py
local/coreaudio-stability --list
```

The build runs synthetic callback tests (continuity, silence, NaN, clipping,
timestamp/format failures) without opening audio. Python tests cover diagnostics,
expected/unknown warnings, corrupt trace, missing cleanup, report failures,
exclusive output creation, busy-device refusal and overlapping-reader lifecycle
using mocked snapshots/processes. `--list` only enumerates and checks the format.
Live qualification must still be performed on the installed device.

When HAL diagnostics are required, matching daemon/HAL summaries must be present.
Steady missing frames or state/snapshot/clock failures cause FAIL. Startup missing
frames and bounded evidence gaps cause WARN. Per-round result.json is the immediate
pre-log verdict; final summary.json includes structured diagnostics collected after
the run. Diagnostic-only failures discovered at this final step do not retroactively
stop earlier cycles, so qualify the short test before a batch.

For RPL qualification, run the installed build with both checks enabled:

```sh
python3 tools/run-coreaudio-stability.py smoke --require-clock-contract --require-deadline-pump
```

Check that the final `summary.json` reports `PASS` before proceeding to cycles,
overlap or continuous recording with the same flags. The checks require matched
HAL and daemon summaries, a valid clock contract (v3 by default), and a capture
thread with the expected scheduling policy, period, budget and constraint.
