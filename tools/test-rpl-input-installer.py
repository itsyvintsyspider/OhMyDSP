#!/usr/bin/env python3
"""Exercise install/rollback/failure rollback in a private filesystem with mocked system tools."""
from pathlib import Path
import os, shlex, subprocess, tempfile
source=(Path(__file__).resolve().parent/'install-rpl-system-input.sh').read_text()
with tempfile.TemporaryDirectory(prefix='rpl-installer-test-') as directory:
 root=Path(directory); mock=root/'mock';mock.mkdir();(root/'tools').mkdir()
 for path in ['/Library/Application Support/OhMyDSP','/Library/LaunchDaemons','/Library/Audio/Plug-Ins/HAL']:
  source=source.replace(shlex.quote(path),shlex.quote(str(root)+path))
 script=root/'tools/install.sh';script.write_text(source)
 commands={
 'id':'echo 0', 'chown':':', 'codesign':':','plutil':':','killall':':',
 'kmutil':'echo com.ohmydsp.OhMyDSP.IPC4Session',
 'ioreg':'''echo '\"ipc4-session-ready\" = Yes'; if [ \"${RUNNING:-0}\" = 1 ]; then echo '\"ipc4-session-state\" = \"running\"'; elif [ "${PREPARED:-0}" = 1 ]; then echo "live capture ready: DSP idle, powers up when the user client starts the stream"; else echo '\"ipc4-session-state\" = \"stopped\"'; fi ''',
 'launchctl':'''case "$1" in print) test -e "$MOCK_STATE";; bootout) rm -f "$MOCK_STATE";; bootstrap) touch "$MOCK_STATE";; *) exit 2;; esac''',
 'cp':'''case "$*" in *userspace/build/OhMyDSP.driver*) if [ "${FAIL_COPY:-0}" = 1 ]; then exit 42; fi;; esac
exec /bin/cp "$@"'''}
 for name,body in commands.items():
  p=mock/name;p.write_text('#!/bin/sh\n'+body+'\n');p.chmod(0o755)
 build=root/'ohmydsp/userspace/build';(build/'OhMyDSP.driver').mkdir(parents=True)
 (build/'ohmydspd').write_text('new-daemon');(build/'ohmydspd').chmod(0o755)
 (build/'OhMyDSP.driver/content').write_text('new-driver')
 (build/'OhMyDSP.driver/Contents/MacOS').mkdir(parents=True)
 (build/'OhMyDSP.driver/Contents/MacOS/OhMyDSPDriver').write_text('executable')
 # Deliberately private source modes, as well as a private caller umask.
 (build/'OhMyDSP.driver/Contents/MacOS/OhMyDSPDriver').chmod(0o700)
 (build/'OhMyDSP.driver/Contents').chmod(0o700)
 (build/'OhMyDSP.driver/content').chmod(0o600)
 base=root/'Library/Application Support/OhMyDSP';base.mkdir(parents=True)
 plist=root/'Library/LaunchDaemons/com.ohmydsp.daemon.plist';plist.parent.mkdir(parents=True)
 drv=root/'Library/Audio/Plug-Ins/HAL/OhMyDSP.driver';drv.mkdir(parents=True)
 (base/'ohmydspd').write_text('old-daemon');plist.write_text('old-plist');(drv/'content').write_text('old-driver')
 (base/'rpl').mkdir();(base/'rpl/asset').write_text('keep-firmware')
 state=root/'running';state.touch()
 env=dict(PATH=str(mock)+':'+os.defpath,MOCK_STATE=str(state))
 def run(*args,expected=0,**extra):
  p=subprocess.run(['sh','-c','umask 077; exec sh "$@"','installer-test',str(script),*args],env=dict(env,**extra),capture_output=True,text=True)
  assert p.returncode==expected,(p.returncode,p.stdout,p.stderr)
 def restored():
  assert (base/'ohmydspd').read_text()=='old-daemon'
  assert plist.read_text()=='old-plist' and (drv/'content').read_text()=='old-driver'
  assert (base/'rpl/asset').read_text()=='keep-firmware' and state.exists()
 run('install',expected=1,RUNNING='1');restored()
 run('install',PREPARED='1');assert (drv/'content').read_text()=='new-driver'
 assert drv.stat().st_mode & 0o777 == 0o755
 assert (drv/'Contents').stat().st_mode & 0o777 == 0o755
 assert (drv/'Contents/MacOS/OhMyDSPDriver').stat().st_mode & 0o777 == 0o755
 assert (drv/'content').stat().st_mode & 0o777 == 0o644
 backup=next((base/'backups').iterdir());assert backup.stat().st_mode & 0o777 == 0o700;run('rollback',str(backup));restored()
 run('install',expected=42,FAIL_COPY='1');restored()
 print('Installer: strict umask/source permission normalization, private backups, install, explicit rollback, injected-failure rollback and asset preservation passed')
