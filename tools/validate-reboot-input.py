#!/usr/bin/env python3
"""Before/after reboot evidence and gated first CoreAudio recording; no installation."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
ID = 'com.ohmydsp.OhMyDSP.IPC4Session'
BASE = Path('/Library/Application Support/OhMyDSP')
KEXT = Path('/Library/Application Support/dmic-macos/kext/rpl-manual-ipc4-session/OhMyDSP.kext')
FILES = [BASE/'ohmydspd', BASE/'rpl/sof-rpl.ri', BASE/'rpl/rpl-dmic.i4pc',
         Path('/Library/LaunchDaemons/com.ohmydsp.daemon.plist'),
         Path('/Library/Audio/Plug-Ins/HAL/OhMyDSP.driver/Contents/Info.plist'),
         Path('/Library/Audio/Plug-Ins/HAL/OhMyDSP.driver/Contents/MacOS/OhMyDSPDriver'),
         KEXT/'Contents/Info.plist', KEXT/'Contents/MacOS/OhMyDSP']
STARTUP_FILES = [BASE/'startup/load-session.sh', BASE/'startup/pins.sha256',
                 Path('/Library/LaunchDaemons/com.ohmydsp.startup.plist')]
spec = importlib.util.spec_from_file_location('session', ROOT/'tools/session-diagnostics.py')
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)


def save(path, data):
    path.write_text(json.dumps(data, indent=2)+'\n')


def command(out, name, argv, timeout=30):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        result = dict(argv=argv, returncode=r.returncode, stdout=r.stdout, stderr=r.stderr)
    except (OSError, subprocess.TimeoutExpired) as exc:
        result = dict(argv=argv, returncode=-1, stdout='', stderr=str(exc))
    save(out/(name+'.json'), result)
    return result


def receipt_action(log, boot_id):
    lines = log.splitlines()
    if [line for line in lines if line.startswith('boot_uuid=')] != ['boot_uuid=' + boot_id]:
        return None
    if 'PASS: session ready; recording remains client-controlled' not in lines:
        return None
    markers = [line.removeprefix('load_action=') for line in lines if line.startswith('load_action=')]
    legacy = [action for message, action in (
        ('loading qualified session kext', 'loaded'),
        ('expected session already loaded; no reload', 'already_loaded')) if message in lines]
    actions = markers or legacy
    if len(actions) == 1 and actions[0] in ('loaded', 'already_loaded') and (not legacy or legacy == actions):
        return actions[0]
    return None


def snapshot(out):
    out.mkdir(mode=0o700)
    boot = command(out, 'boot', ['sysctl', '-n', 'kern.bootsessionuuid'])
    command(out, 'boottime', ['sysctl', '-n', 'kern.boottime'])
    loaded = command(out, 'loaded', ['kmutil', 'showloaded', '--bundle-identifier', ID])
    command(out, 'collections', ['kmutil', 'inspect', '--show-kext-uuids', '--bundle-identifier', ID])
    daemon = command(out, 'daemon', ['launchctl', 'print', 'system/com.ohmydsp.daemon'])
    enumeration = command(out, 'enumeration', [str(ROOT/'local/coreaudio-stability'), '--list'])
    command(out, 'audio-devices', ['system_profiler', 'SPAudioDataType'])
    command(out, 'audio-log', ['/usr/bin/log', 'show', '--last', '10m', '--style', 'compact',
                             '--predicate', 'process == "ohmydspd" OR sender == "OhMyDSPDriver" OR (process == "kernel" AND eventMessage BEGINSWITH "OhMyDSP")'])
    state = diag.snapshot(out/'kernel')
    hashes = {}
    for file in FILES:
        try:
            hashes[str(file)] = hashlib.sha256(file.read_bytes()).hexdigest()
        except OSError:
            hashes[str(file)] = None
    match = re.search(re.escape(ID)+r'\s+\([^)]*\)\s+([A-Fa-f0-9-]{36})', loaded['stdout'])
    result = dict(schema=1, boot_uuid=boot['stdout'].strip() if boot['returncode']==0 else '',
                  kext_uuid=match[1].upper() if loaded['returncode']==0 and match else None,
                  daemon_running=daemon['returncode']==0 and '\tstate = running' in daemon['stdout'],
                  device_visible=enumeration['returncode']==0, hashes=hashes, kernel=state)
    startup = dict(hashes={}, registered=False, latest_boot_exit=None)
    for file in STARTUP_FILES:
        try:
            startup['hashes'][str(file)] = hashlib.sha256(file.read_bytes()).hexdigest()
        except OSError:
            startup['hashes'][str(file)] = None
    if any(startup['hashes'].values()):
        job = command(out, 'startup-service', ['launchctl', 'print', 'system/com.ohmydsp.startup'])
        startup['registered'] = job['returncode']==0
        fallback = Path('/Library/Logs/OhMyDSP/startup-launch.log')
        try:
            if fallback.is_file():
                with fallback.open('rb') as stream:
                    stream.seek(max(0, fallback.stat().st_size-131072))
                    (out/'startup-launch.log').write_bytes(stream.read(131072))
        except OSError as exc:
            startup['launch_log_error'] = str(exc)
        boot_id = result['boot_uuid']
        if re.fullmatch(r'[A-Fa-f0-9-]{36}', boot_id):
            try:
                logs = sorted(Path('/Library/Logs/OhMyDSP/startup').glob(boot_id+'.*'), key=lambda p:p.stat().st_birthtime)
                if logs:
                    recent = logs[-1]
                    startup['log_directory'] = str(recent)
                    for name in ('loader.log', 'exit-status', 'loaded.txt'):
                        file = recent/name
                        if file.is_file():
                            (out/('startup-'+name)).write_bytes(file.read_bytes()[:131072])
                    startup['load_action'] = receipt_action((out/'startup-loader.log').read_text(), boot_id)
                    loaded_line = (out/'startup-loaded.txt').read_text()
                    identity = re.search(re.escape(ID) + r'\s+\([^)]*\)\s+([A-Fa-f0-9-]{36})', loaded_line)
                    startup['loaded_uuid'] = identity[1].upper() if identity else None
                    startup['latest_boot_exit'] = int((recent/'exit-status').read_text().strip())
            except (OSError, ValueError) as exc:
                startup['log_error'] = str(exc)
    result['startup'] = startup
    save(out/'snapshot.json', result)
    return result


def readiness(data):
    errors = []
    if not re.fullmatch(r'[A-Fa-f0-9-]{36}', data.get('boot_uuid', '')):
        errors.append('boot identity unavailable')
    if not data.get('kext_uuid'):
        errors.append('IPC4Session kext not loaded; HAL enumeration alone is insufficient')
    if not data.get('daemon_running'):
        errors.append('OhMyDSP LaunchDaemon is not running')
    if not data.get('device_visible'):
        errors.append('DMIC device/format unavailable')
    if any(not data.get('hashes', {}).get(str(p)) for p in FILES):
        errors.append('required installed artifact missing or unreadable')
    startup = data.get('startup', {})
    if not startup.get('registered') or any(not startup.get('hashes', {}).get(str(p)) for p in STARTUP_FILES):
        errors.append('automatic startup requires the registered loader and all pinned configuration files')
    k = data.get('kernel', {})
    if k.get('bundle') != ID or k.get('ready') is not True or k.get('state') not in ('prepared', 'stopped'):
        errors.append('kernel session not prepared/ready and idle; no recording attempted')
    return errors


def compare(before, after):
    errors = readiness(after)
    if not before.get('boot_uuid') or before['boot_uuid'] == after.get('boot_uuid'):
        errors.append('a different boot session has not been demonstrated')
    if not before.get('kext_uuid') or before['kext_uuid'] != after.get('kext_uuid'):
        errors.append('loaded kext UUID differs from baseline or is unavailable')
    if before.get('hashes') != after.get('hashes'):
        errors.append('installed artifacts differ from baseline')
    previous = before.get('startup', {}).get('hashes', {})
    current = after.get('startup', {})
    current_hashes = current.get('hashes', {})
    if previous != current_hashes:
        errors.append('startup configuration differs from baseline')
    if current.get('latest_boot_exit') != 0:
        errors.append('startup loader lacks a successful exit for this boot')
    if current.get('load_action') != 'loaded' or current.get('loaded_uuid') != after.get('kext_uuid'):
        errors.append('automatic load of the expected kext was not demonstrated by this boot receipt')
    if after.get('kernel', {}).get('session_number') != 0:
        errors.append('first recording after boot cannot be verified: session already used or number unavailable')
    return errors


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('phase', choices=['before', 'after'])
    p.add_argument('--baseline', type=Path, help='before-phase output directory')
    p.add_argument('--output', type=Path, help='new evidence directory')
    args = p.parse_args()
    if os.geteuid()==0:
        p.error('run without sudo; CoreAudio microphone permission belongs to the logged-in user')
    if (args.phase=='after') != (args.baseline is not None):
        p.error('--baseline is required only for the after phase')
    before = None
    if args.baseline:
        before = json.loads((args.baseline/'state/snapshot.json').read_text())
        receipt = json.loads((args.baseline/'summary.json').read_text())
        if receipt.get('phase')!='before' or receipt.get('status')!='READY_FOR_REBOOT' or readiness(before):
            p.error('baseline did not pass pre-reboot checks')
    os.umask(0o077)
    out = args.output or ROOT/'diagnostics'/('reboot-'+args.phase+'-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=False, mode=0o700)
    summary = dict(schema=1, phase=args.phase, status='INCOMPLETE', errors=[], saves_audio=False)
    save(out/'summary.json', summary)
    print(f'Evidence: {out}', flush=True)
    state = snapshot(out/'state')
    summary['errors'] = readiness(state) if before is None else compare(before, state)
    if summary['errors']:
        summary['status']='FAIL'
    elif before is None:
        summary['status']='READY_FOR_REBOOT'
    else:
        # No reinstall, reload, service restart, default-device change or saved PCM.
        code = subprocess.call([sys.executable, str(ROOT/'tools/run-coreaudio-stability.py'), 'smoke',
                                '--require-clock-contract', '--require-deadline-pump', '--output', str(out/'first-recording')])
        report = out/'first-recording/summary.json'
        test = json.loads(report.read_text()) if report.exists() else {}
        summary['status'] = test.get('status') if code==0 and test.get('status') in ('PASS', 'WARN') else 'FAIL'
        summary['recording_status'] = test.get('status', 'missing')
        if summary['status']!='PASS':
            summary['errors'].append('first recording did not fully pass; inspect first-recording/summary.json')
    save(out/'summary.json', summary)
    print(json.dumps(summary, indent=2))
    return 0 if summary['status'] in ('READY_FOR_REBOOT', 'PASS') else 1


if __name__ == '__main__':
    sys.exit(main())
