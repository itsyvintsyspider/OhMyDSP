#!/usr/bin/env python3
"""Controlled CoreAudio tests. Run as the logged-in user, not sudo; no installed files change."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('diagnostics', ROOT / 'tools/session-diagnostics.py')
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)
audio_spec = importlib.util.spec_from_file_location('audio_diagnostics', ROOT / 'tools/parse-audio-diagnostics.py')
audio_diag = importlib.util.module_from_spec(audio_spec)
audio_spec.loader.exec_module(audio_diag)
children = []
cancelled = False


def cancel(signum, frame):
    global cancelled
    cancelled = True
    for proc in children:
        if proc.poll() is None:
            proc.terminate()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def inspect_report(path, returncode):
    errors, warnings = [], []
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as exc:
        return {}, [f'missing/invalid test report: {exc}'], warnings
    if returncode or data.get('status') != 'PASS':
        errors.append(f'CoreAudio test failed (exit {returncode})')
    if data.get('timestamp_discontinuities', 0):
        jumps = [e.get('delta') for e in data.get('events', []) if e.get('kind') == 'sample_jump_frames']
        errors.append(f'{data["timestamp_discontinuities"]} timestamp discontinuities; sample jumps (frames): {jumps}')
    if data.get('period_before_status')==0 and data.get('period_after_status')==0 and data.get('period_before')!=data.get('period_after'):
        errors.append(f'HAL timestamp period changed: {data["period_before"]} -> {data["period_after"]}')
    if data.get('callback_stall'):
        errors.append('no frame progress for more than 5 seconds')
    if data.get('events_dropped', 0):
        warnings.append('bounded event evidence filled; some event details omitted')
    if data.get('longest_zero_ms', 0) >= 1:
        warnings.append(f'{data["zero_frames"]} exactly zero frames; silence/mute and missing audio are not distinguishable here')
    if any(c.get('clipped_samples', 0) for c in data.get('channels', [])):
        warnings.append('samples reached full scale; review gain/clipping')
    if data.get('max_callback_gap_ms', 0) > 100:
        warnings.append('callback scheduling gap exceeded 100 ms')
    return data, errors, warnings


def spawn(binary, duration, folder, name, sample):
    report = folder / f'{name}.json'
    cmd = [str(binary), str(duration), str(report)]
    if sample:
        cmd.append(str(folder / f'{name}-first10s.wav'))
    log = open(folder / f'{name}.log', 'x')
    try:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    finally:
        log.close()
    children.append(proc)
    return proc, report


def wait(proc, seconds):
    try:
        return proc.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        return 124


def wait_idle(path):
    # Closing our client does not stop another application's recording. Observe, never force it.
    data = diag.snapshot(path)
    return data


def run_group(binary, mode, duration, folder, sample):
    before = diag.snapshot(folder / 'before')
    if before.get('ready') is not True or before.get('state') not in ('stopped', 'prepared') or before.get('bundle') != 'com.ohmydsp.OhMyDSP.IPC4Session':
        return dict(status='FAIL', errors=['IPC4Session must be ready and stopped; close other microphone readers first'], warnings=[])
    jobs = [spawn(binary, duration, folder, 'client-a', sample)]
    overlap = None
    if mode == 'overlap':
        time.sleep(2)
        if jobs[0][0].poll() is None and not cancelled:
            overlap = diag.snapshot(folder / 'first-client-running')
            if overlap.get('state') == 'running':
                jobs.append(spawn(binary, 4, folder, 'client-b', sample))
    records, errors, warnings = [], [], []
    # In overlap mode B must finish while A continues, then validate continued ownership.
    order = list(reversed(jobs)) if mode == 'overlap' else jobs
    remaining = None
    for proc, report in order:
        code = wait(proc, duration + 20)
        data, e, w = inspect_report(report, code)
        records.append(dict(file=report.name, returncode=code, report=data))
        errors.extend(e); warnings.extend(w)
        if mode == 'overlap' and len(jobs) == 2 and report == jobs[1][1]:
            remaining = diag.snapshot(folder / 'after-second-client')
            if jobs[0][0].poll() is not None or remaining.get('state') != 'running':
                errors.append('first client did not remain running after second client stopped')
    if mode == 'overlap' and len(jobs) != 2:
        errors.append('could not establish overlapping clients')
    time.sleep(1)
    after = wait_idle(folder / 'after')
    e, w = diag.validate_transition(before, after)
    errors.extend(e); warnings.extend(w)
    if cancelled:
        errors.append('test interrupted')
    return dict(status='FAIL' if errors else 'WARN' if warnings else 'PASS', errors=errors,
                warnings=warnings, clients=records, kernel_before=before.get('session_number'),
                kernel_after=after.get('session_number'))


def check_audio_diagnostics(group, parsed, args):
    match = next((s for s in parsed['sessions'] if s['kernel_session']==group.get('kernel_after')),None)
    if match:
        group['audio_diagnostics_available']=match['available']
        group['errors'].extend(match['errors'])
        group['warnings'].extend(match['warnings'])
    if (args.require_hal_diagnostics or args.require_clock_contract or args.require_deadline_pump) and (not match or not match['available']):
        group['errors'].append('required HAL/daemon diagnostic summaries missing; check installed build and log access')
    if args.require_clock_contract and (not match or match.get('clock',{}).get('contract')!=args.require_clock_contract):
        group['errors'].append(f'required clock contract v{args.require_clock_contract} evidence missing')
    if args.require_deadline_pump and (not match or match.get('pump',{}).get('mode')!='deadline_thread'):
        group['errors'].append('required dedicated capture pump evidence missing')
    if parsed['malformed'] or parsed['conflicts']:
        group['warnings'].append('malformed/conflicting diagnostic log records; inspect audio-diagnostics.json')
    group['status']='FAIL' if group['errors'] else 'WARN' if group['warnings'] else group['status']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['smoke', 'cycles', 'long', 'overlap'])
    p.add_argument('--seconds', type=int, help='seconds per session (long: 60..1800; others: 2..60)')
    p.add_argument('--cycles', type=int, help='cycles mode only, 2..50 (default 20)')
    p.add_argument('--require-clock-contract', nargs='?', const=3, type=int, choices=[2,3], help='require clock contract version (default 3: fixed RPL lap period)')
    p.add_argument('--require-deadline-pump', action='store_true', help='require dedicated capture thread policy evidence')
    p.add_argument('--require-hal-diagnostics', action='store_true', help='require matching HAL and daemon counters (install diagnostic userspace build first)')
    p.add_argument('--save-sample', action='store_true', help='save first 10 seconds per client; default saves no audio')
    p.add_argument('--output', type=Path, help='new directory; defaults to diagnostics/coreaudio-MODE-TIMESTAMP')
    args = p.parse_args()
    if os.geteuid() == 0:
        p.error('run without sudo so microphone permission belongs to your logged-in terminal')
    if args.cycles is not None and (args.mode != 'cycles' or not 2 <= args.cycles <= 50):
        p.error('--cycles is 2..50 and only valid for cycles mode')
    if args.mode == 'overlap' and args.seconds is not None:
        p.error('overlap uses fixed 12-second and 4-second clients')
    duration = args.seconds if args.seconds is not None else dict(smoke=10, cycles=5, long=600, overlap=12)[args.mode]
    lo, hi = (60, 1800) if args.mode == 'long' else (2, 60)
    if not lo <= duration <= hi:
        p.error(f'--seconds must be {lo}..{hi}')
    binary = ROOT / 'local/coreaudio-stability'
    if not binary.is_file():
        p.error('run tools/build-coreaudio-stability.sh first')
    os.umask(0o077)
    out = (args.output or ROOT / 'diagnostics' / ('coreaudio-' + args.mode + '-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))).resolve()
    out.mkdir(parents=True, exist_ok=False, mode=0o700)
    count = (args.cycles or 20) if args.mode == 'cycles' else 1
    summary = dict(schema=1, mode=args.mode, seconds=duration, requested_cycles=count,
                   saves_audio=args.save_sample, started_utc=datetime.now(timezone.utc).isoformat(),
                   binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), groups=[], status='INCOMPLETE')
    write(out / 'summary.json', summary)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, cancel)
    print(f'Diagnostics: {out}\nRecording from DMIC via CoreAudio; microphone permission may be requested.', flush=True)
    try:
        # Enumeration does not start recording or change the system default device.
        listed = subprocess.run([str(binary), '--list'], capture_output=True, text=True, timeout=15)
        (out / 'device.json').write_text(listed.stdout)
        (out / 'device.stderr').write_text(listed.stderr)
        if listed.returncode:
            raise RuntimeError('expected DMIC device/format unavailable')
        for index in range(count):
            if cancelled:
                break
            folder = out / f'round-{index+1:03d}'
            folder.mkdir(mode=0o700)
            print(f'Round {index+1}/{count}: {duration}s', flush=True)
            result = run_group(binary, args.mode, duration, folder, args.save_sample)
            summary['groups'].append(result)
            write(folder / 'result.json', result)
            write(out / 'summary.json', summary)
            print(result['status'], '; '.join(result['errors'] + result['warnings']), flush=True)
            if result['status'] == 'FAIL':
                break
        statuses = [g['status'] for g in summary['groups']]
        summary['status'] = 'FAIL' if cancelled or len(statuses) != count or 'FAIL' in statuses else 'WARN' if 'WARN' in statuses else 'PASS'
    except Exception as exc:
        summary['status'] = 'FAIL'
        summary['error'] = str(exc)
        print('FAIL:',str(exc),flush=True)
    finally:
        for proc in children:
            if proc.poll() is None:
                proc.terminate()
                wait(proc, 5)
        try:
            diag.snapshot(out / 'final')
        except Exception as exc:
            summary['final_snapshot_error'] = str(exc)
            summary['status'] = 'FAIL'
        # Best effort. No sudo; lack of unified-log access is reported, not concealed.
        try:
            start = datetime.fromisoformat(summary['started_utc']).astimezone()
            result = subprocess.run(['log', 'show', '--start', start.strftime('%Y-%m-%d %H:%M:%S'),
                                     '--style', 'syslog', '--predicate',
                                     'process == "ohmydspd" OR sender == "OhMyDSPDriver" OR '
                                     '(process == "kernel" AND eventMessage BEGINSWITH "OhMyDSP")'],
                                    capture_output=True, text=True, timeout=30)
            (out / 'unified.log').write_text(result.stdout)
            (out / 'unified.stderr').write_text(result.stderr)
            summary['log_collection_status'] = result.returncode
        except Exception as exc:
            summary['log_collection_error'] = str(exc)
        requested_sessions = [g['kernel_after'] for g in summary['groups'] if isinstance(g.get('kernel_after'),int) and g.get('kernel_after') != g.get('kernel_before')]
        logfile = out / 'unified.log'
        parsed = audio_diag.parse(logfile.read_text() if logfile.exists() else '', requested_sessions)
        write(out / 'audio-diagnostics.json', parsed)
        for group in summary['groups']:
            check_audio_diagnostics(group, parsed, args)
        if any(g['status']=='FAIL' for g in summary['groups']): summary['status']='FAIL'
        elif summary['status']=='PASS' and any(g['status']=='WARN' for g in summary['groups']): summary['status']='WARN'
        # Round result.json is the immediate pre-log result; summary.json is the enriched final verdict.
        summary['finished_utc'] = datetime.now(timezone.utc).isoformat()
        write(out / 'summary.json', summary)
        print(f'{summary["status"]}: {out}', flush=True)
    return 1 if summary['status'] == 'FAIL' else 0


if __name__ == '__main__':
    raise SystemExit(main())
