#!/usr/bin/env python3
"""Exercise the actual plan installer against a private destination and stub generator."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('install-rpl-ipc4-module-plan.sh').read_text()
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    tools = root/'tools'
    tools.mkdir()
    dest = root/'assets'
    dest.mkdir()
    script = tools/'install.sh'
    script.write_text(source.replace('/Library/Application Support/OhMyDSP/rpl', '$FIXTURE_ASSETS')
                      .replace('/private/tmp/', '"$FIXTURE_ROOT"/'))
    (root/'NHLT.bin').touch()
    (tools/'extract-nhlt-vendor-blob.py').write_text(
        'import pathlib,sys\npathlib.Path(sys.argv[2]).write_bytes(b"blob")\n'
        'pathlib.Path(sys.argv[4]).write_text("{}")\n')
    (tools/'gen-ipc4-messages.py').write_text(
        'import os,pathlib,sys\n'
        'def arg(k): return pathlib.Path(sys.argv[sys.argv.index(k)+1])\n'
        'arg("--output").write_text("{}")\n'
        'arg("--binary-output").write_bytes(os.environ["PAYLOAD"].encode())\n'
        'sys.exit(int(os.environ.get("FAIL",0)))\n')
    for name, body in [('id','echo 0'),('chown',':')]:
        mock=tools/name
        mock.write_text('#!/bin/sh\n'+body+'\n')
        mock.chmod(0o700)
    asset=dest/'rpl-dmic.i4pc'
    asset.write_bytes(b'old')
    other=root/'existing'
    other.write_text('preserve')
    (dest/'rpl-dmic.i4pc.tmp').symlink_to(other)
    (root/'nhlt-endpoints.json').symlink_to(other)
    def install(label, fail=0):
        env=dict(PATH=str(tools)+':'+os.defpath,FIXTURE_ROOT=str(root),FIXTURE_ASSETS=str(dest),
                 PAYLOAD=label,FAIL=str(fail))
        return subprocess.run(['sh',str(script)],env=env,capture_output=True,text=True)
    assert install('partial',1).returncode and asset.read_bytes()==b'old'
    with ThreadPoolExecutor(2) as pool:
        results=list(pool.map(install, ['first','second']))
    paths=[]
    for result in results:
        assert result.returncode==0,result.stderr
        plan=Path(next(line.removeprefix('reviewable plan: ') for line in result.stdout.splitlines()
                       if line.startswith('reviewable plan: ')))
        assert plan.exists() and plan.parent.stat().st_mode & 0o777 == 0o700
        paths.append(plan)
    assert paths[0]!=paths[1] and asset.read_bytes() in (b'first',b'second')
    assert other.read_text()=='preserve' and not list(dest.glob('*.stage.*'))
print('Private concurrent plans, untouched existing files and failed-install rollback PASS')
