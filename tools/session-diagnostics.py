#!/usr/bin/env python3
"""Export read-only IPC4 session evidence; does not open the exclusive user client."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess


def trace_decoder():
    spec = importlib.util.spec_from_file_location('mtrace', Path(__file__).with_name('decode-ipc4-mtrace.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.extract_stream


def summarize(text):
    def value(key):
        match = re.search(r'"' + re.escape(key) + r'"\s*=\s*("[^"\n]*"|Yes|No|\d+)', text)
        if not match:
            return None
        raw = match[1]
        return {'Yes': True, 'No': False}.get(raw, raw.strip('"') if raw.startswith('"') else int(raw) if raw.isdigit() else raw)
    summary = dict(schema=1, ready=value('ipc4-session-ready'), state=value('ipc4-session-state'),
                   session_number=value('ipc4-session-number'), bundle=value('CFBundleIdentifier'), host_log_truncated=value('phase1-log-truncated'), sessions=[])
    decode = trace_decoder()
    for slot in range(4):
        prefix = f'ipc4-session-slot{slot}'
        number = value(prefix + '-number')
        if number is None:
            continue
        m = re.search(r'"' + prefix + r'-log"\s*=\s*\(([^\n]*)\)', text)
        entries = []
        if m:
            entries = [json.loads(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"', m[1])]
        failures = [x for x in entries if re.search(r'\bFAIL(?:ED)?\b|\bFAULT\b|timeout|start-failed|stop-failed|blocked=1', x, re.I)]
        session = dict(number=number, slot=slot, log_entries=len(entries),
                       start_pass='IPC4 session start: PASS' in entries,
                       stop_pass='IPC4 session stop: PASS blocked=0' in entries,
                       idle_restored=any('final ADSPCS=0x00000f0f' in x for x in entries), failures=failures)
        t = re.search(r'"' + prefix + r'-trace"\s*=\s*<([0-9a-fA-F]+)>', text)
        if t:
            try:
                meta, records = decode(bytes.fromhex(t[1]))
                session['trace'] = meta
                # Text log messages only; sample continuity cannot be inferred from firmware logs.
                payload = b''.join(p for _, p in records).decode('utf-8', errors='replace')
                session['firmware_errors'] = [x for x in payload.splitlines() if re.search(r'<err>|\bxrun\b', x, re.I)]
                session['firmware_warnings'] = [x for x in payload.splitlines() if '<wrn>' in x]
                session['expected_scheduler_fallback'] = [x for x in session['firmware_warnings'] if x.endswith('ipc_pipeline_complete(): no scheduling component specified, use comp 0x20004')]
                session['scheduler_overruns'] = [int(x) for x in re.findall(r'overruns (\d+)', payload)]
            except ValueError as exc:
                session['trace_error'] = str(exc)
        summary['sessions'].append(session)
    summary['sessions'].sort(key=lambda s: s['number'])
    # This build only publishes session properties on its first START. Do not
    # infer idle from absence alone: require its explicit successful preparation log.
    if (summary['bundle'] == 'com.ohmydsp.OhMyDSP.IPC4Session' and summary['ready'] is True
            and summary['state'] is None and summary['session_number'] is None
            and not summary['sessions'] and not summary['host_log_truncated']
            and 'live capture ready: DSP idle, powers up when the user client starts the stream' in text
            and not re.search(r'IPC4 session \d+:', text)):
        summary.update(state='prepared', session_number=0,
                       initial_state_evidence='ready-property-and-initial-idle-log')
    # This tool observes the existing build; do not invent unavailable driver counters.
    summary['unavailable_counters'] = ['kernel_final_pcm_frames', 'hal_missing_frames', 'daemon_copied_frames']
    return summary


def snapshot(directory, source=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=False, mode=0o700)
    if source:
        text = Path(source).read_text()
    else:
        result = subprocess.run(['ioreg', '-rc', 'OhMyDSP', '-l', '-w0'], capture_output=True, text=True, timeout=15)
        (directory / 'ioreg.stderr').write_text(result.stderr)
        if result.returncode:
            raise RuntimeError(f'ioreg exited {result.returncode}')
        text = result.stdout
    # ioreg -w0 emits each OSData property on a single line.
    text = ''.join(line for line in text.splitlines(keepends=True)
                   if '"ipc4-pcm-capture"' not in line)
    (directory / 'ohmydsp-ioreg').write_text(text)
    data = summarize(text)
    (directory / 'summary.json').write_text(json.dumps(data, indent=2) + '\n')
    return data


def validate_transition(before, after, expected=1):
    errors, warnings = [], []
    if after.get('bundle') != 'com.ohmydsp.OhMyDSP.IPC4Session':
        errors.append('expected IPC4Session bundle is not present')
    if after.get('ready') is not True or after.get('state') != 'stopped':
        errors.append('session is not ready and stopped after test (another reader may still be active)')
    if after.get('host_log_truncated'):
        warnings.append('host log truncated; diagnostic coverage limited')
    old, new = before.get('session_number'), after.get('session_number')
    if not isinstance(old, int) or not isinstance(new, int) or new - old != expected:
        errors.append(f'expected {expected} new kernel session(s), observed {old} -> {new}')
    else:
        sessions = {s['number']: s for s in after['sessions']}
        for number in range(old + 1, new + 1):
            s = sessions.get(number)
            if not s or not s['start_pass'] or not s['stop_pass'] or not s['idle_restored']:
                errors.append(f'session {number}: missing successful start/stop/idle evidence')
                continue
            if s['failures'] or s.get('firmware_errors') or any(s.get('scheduler_overruns', [])):
                errors.append(f'session {number}: host/firmware fault evidence')
            unexpected = [x for x in s.get('firmware_warnings', []) if x not in s.get('expected_scheduler_fallback', [])]
            if unexpected:
                warnings.append(f'session {number}: firmware warnings: {unexpected}')
            if 'trace_error' in s or 'trace' not in s:
                errors.append(f'session {number}: missing or invalid trace')
            elif s['trace']['flags']:
                warnings.append(f'session {number}: trace coverage limited (flags={s["trace"]["flags"]}); see summary')
    return errors, warnings


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path, help='new output directory')
    p.add_argument('--source', type=Path, help='parse an existing ohmydsp-ioreg instead of live capture')
    args = p.parse_args()
    import os
    os.umask(0o077)
    data = snapshot(args.output, args.source)
    print(json.dumps(data, indent=2))


if __name__ == '__main__':
    main()
