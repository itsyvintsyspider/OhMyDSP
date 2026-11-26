#!/usr/bin/env python3
"""Isolated startup loader/installer checks; mocks privileged commands and hardware."""
from pathlib import Path
import hashlib
import re
import subprocess
import tempfile

TOOLS=Path(__file__).resolve().parent
loader_source=(TOOLS/'rpl-startup-loader.sh').read_text()
pin=re.search(r'^UUID=([0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12})$',loader_source,re.MULTILINE)
assert pin, 'startup loader must pin a kext UUID'
with tempfile.TemporaryDirectory(prefix='rpl-startup-test-') as tmp:
    root=Path(tmp).resolve();mock=root/'mock';mock.mkdir();(root/'tools').mkdir()
    base=root/'Library/Application Support/OhMyDSP';dest=base/'startup'
    plist=root/'Library/LaunchDaemons/com.ohmydsp.startup.plist'
    commands={
        'id':'echo "${FAKE_UID:-0}"', 'chown':':',
        'stat':'''if [ "$2" = %u ]; then echo 0; elif [ "${UNSAFE_PATH:-0}" = 1 ] && [ "$3" = "$FIXTURE_ROOT/Library/Application Support/OhMyDSP/rpl/asset" ]; then echo 777; else echo 755; fi''',
        'codesign':'exit "${BAD_SIGNATURE:-0}"',
        'sysctl':'''case "$*" in *kern.bootargs*) echo "${BOOT_ARGS:--v}";; *) echo 11111111-1111-1111-1111-111111111111;; esac''',
        'sleep':'/bin/sleep 0.01',
        'ioreg':'''echo "ioreg" >> "$CALLS"
echo '"ipc4-pcm-capture" = <50434d3170726976617465>'
echo '\"CFBundleIdentifier\" = \"com.ohmydsp.OhMyDSP.IPC4Session\"'
if [ "${RUNNING:-0}" = 1 ]; then echo '\"ipc4-session-state\" = \"running\"'; else echo '\"ipc4-session-state\" = \"stopped\"'; fi
case "${READY:-yes}" in yes) echo '\"ipc4-session-ready\" = Yes';; no) echo '\"ipc4-session-ready\" = No';; esac''',
        'kmutil':'''echo "kmutil $*" >> "$CALLS"
case "$1" in
showloaded)
 if [ "${CONFLICT:-0}" = 1 ]; then echo 'com.ohmydsp.OhMyDSP.IPC4Probe (0.3.0) OTHER'; fi
 if [ -f "$KEXT_STATE" ]; then echo "201 0 0 0 0 com.ohmydsp.OhMyDSP.IPC4Session (0.3.0) $KEXT_UUID"; fi;;
load) if [ "${HANG_LOAD:-0}" = 1 ]; then exec /bin/sleep 10; fi; [ "${FAIL_LOAD:-0}" = 0 ] || exit 42; touch "$KEXT_STATE";;
*) exit 2;; esac''',
        'launchctl':'''echo "launchctl $*" >> "$CALLS"
case "$1" in
print) test -f "$REGISTERED";;
bootout) rm -f "$REGISTERED";;
bootstrap) if [ "${FAIL_BOOTSTRAP:-0}" = 1 ] && [ ! -f "$FAIL_ONCE" ]; then touch "$FAIL_ONCE"; exit 42; fi; touch "$REGISTERED";;
*) exit 2;; esac'''}
    for name,body in commands.items():
        f=mock/name;f.write_text('#!/bin/sh\n'+body+'\n');f.chmod(0o755)
    for name in ['rpl-startup-loader.sh','install-rpl-startup.sh']:
        text=(TOOLS/name).read_text()
        # Keep the fixture root quoted in shell words and unquoted in plist text.
        text=text.replace("='/Library/", '''="$FIXTURE_ROOT"'/Library/''')
        text=re.sub(r'([= ])/Library/', r'\1"$FIXTURE_ROOT"/Library/', text)
        text=text.replace('>/Library/', '>$FIXTURE_ROOT/Library/')
        text, count = re.subn(r'^PATH=(.+)$', r'PATH="$MOCK_BIN":\1', text, flags=re.MULTILINE)
        assert count == 1, f'cannot install command mocks for {name}'
        f=root/'tools'/name;f.write_text(text);f.chmod(0o755)
    asset=base/'rpl/asset';asset.parent.mkdir(parents=True);asset.write_text('unchanged firmware')
    pins=root/'tools/rpl-startup-pins.sha256';pins.write_text(hashlib.sha256(asset.read_bytes()).hexdigest()+'  '+str(asset)+'\n')
    loaded=root/'loaded';loaded.touch();registered=root/'registered';calls=root/'calls';calls.touch()
    env=dict(FIXTURE_ROOT=str(root),MOCK_BIN=str(mock),KEXT_UUID=pin[1],KEXT_STATE=str(loaded),
             REGISTERED=str(registered),CALLS=str(calls),FAIL_ONCE=str(root/'failed-once'))
    install=root/'tools/install-rpl-startup.sh'
    def run(script,*args,code=0,**extra):
        r=subprocess.run(['sh',str(script),*args],env=dict(env,**extra),capture_output=True,text=True,timeout=15)
        assert r.returncode==code,(r.returncode,r.stdout,r.stderr,calls.read_text(),[(str(p),p.read_text()) for p in (root/'Library/Logs/OhMyDSP/startup').glob('*/loader.log')][-1:])
        assert asset.read_text()=='unchanged firmware'
        return r
    run(install,code=1,RUNNING='1');assert not dest.exists()
    run(install);assert registered.exists() and plist.exists()
    assert dest.stat().st_mode & 0o777==0o755
    assert (dest/'load-session.sh').stat().st_mode & 0o777==0o755
    helper=dest/'load-session.sh'
    run(helper,'check');calls.write_text('');run(helper)
    assert 'kmutil load ' not in calls.read_text()
    def last_log():
        logs=(root/'Library/Logs/OhMyDSP/startup').glob('*/loader.log')
        return max(logs,key=lambda p:p.stat().st_mtime_ns).read_text()
    assert 'load_action=already_loaded' in last_log()
    dumps = list((root/'Library/Logs/OhMyDSP/startup').glob('*/ohmydsp-ioreg'))
    assert dumps and all('ipc4-pcm-capture' not in f.read_text() and '50434d3170726976617465' not in f.read_text() for f in dumps)
    calls.write_text('');loaded.unlink();run(helper);assert 'kmutil load ' in calls.read_text() and loaded.exists()
    assert 'load_action=loaded' in last_log()
    run(helper, code=1, BAD_SIGNATURE='1')
    assert 'kext signature verification failed' in last_log() and 'exit_status=1' in last_log()
    for extra in [dict(CONFLICT='1'),dict(KEXT_UUID='WRONG'),dict(READY='no'),dict(READY='missing'),dict(BOOT_ARGS='-v -ohmydspoff'),dict(UNSAFE_PATH='1'),dict(BAD_SIGNATURE='1')]:
        calls.write_text('');run(helper,code=1,**extra);assert 'kmutil load ' not in calls.read_text()
    run(helper,code=1,UNSAFE_PATH='1')
    assert 'not group/world writable' in last_log() and 'exit_status=1' in last_log()
    loaded.unlink();run(helper,code=1,FAIL_LOAD='1');assert not loaded.exists()
    run(helper,code=1,HANG_LOAD='1');assert not loaded.exists();loaded.touch()
    (dest/'pins.sha256').write_text('0'*64+'  '+str(asset)+'\n');calls.write_text('');run(helper,code=1);assert 'kmutil' not in calls.read_text()
    assert 'installed artifacts differ' in last_log() and 'exit_status=1' in last_log()
    backups=sorted((base/'backups').iterdir());assert len(backups)==1 and backups[0].stat().st_mode & 0o777==0o700
    run(install,'rollback',str(backups[0]));assert not dest.exists() and not plist.exists() and not registered.exists() and loaded.exists()
    run(install,code=42,FAIL_BOOTSTRAP='1');assert not dest.exists() and not plist.exists() and loaded.exists()
    run(install)
    old=helper.read_bytes();oldplist=plist.read_bytes();(root/'failed-once').unlink()
    run(install,code=42,FAIL_BOOTSTRAP='1');assert helper.read_bytes()==old and plist.read_bytes()==oldplist and registered.exists()
    logs=list((root/'Library/Logs/OhMyDSP/startup').glob('*/exit-status'))
    assert any(p.read_text().strip()=='0' for p in logs) and any(p.read_text().strip()=='1' for p in logs)
    print('Startup tests PASS: pins, conflicts, identity, disabled flag, permissions/signature, bounded readiness, load failure, idempotence, install/rollback and failure restoration; no real hardware touched.')
