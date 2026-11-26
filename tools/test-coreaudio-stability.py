#!/usr/bin/env python3
"""Offline diagnostic classification and runner lifecycle tests; never opens a microphone."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parent

def load(name, file):
    spec = importlib.util.spec_from_file_location(name, TOOLS / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

diag = load('diag', 'session-diagnostics.py')
runner = load('runner', 'run-coreaudio-stability.py')


def fixture(number=1, stop=True, flags=0, message=''):
    entries = ['IPC4 session start: PASS', 'final ADSPCS=0x00000f0f (idle baseline was 0x00000f0f)']
    if stop:
        entries.append('IPC4 session stop: PASS blocked=0')
    raw = struct.pack('<4I', 0x3152544d, 1, flags, 0)
    if message:
        b = message.encode()
        raw += struct.pack('<8I', 32+((len(b)+3)&~3), 0, 0, 0, 0x474f4c00, 0, len(b), len(b))
        raw += b + b'\0' * ((-len(b)) % 4)
    return ('"ipc4-session-ready" = Yes\n"ipc4-session-state" = "stopped"\n'
            '"CFBundleIdentifier" = "com.ohmydsp.OhMyDSP.IPC4Session"\n'
            f'"ipc4-session-number" = {number}\n"ipc4-session-slot0-number" = {number}\n'
            f'"ipc4-session-slot0-log" = ({",".join(json.dumps(x) for x in entries)})\n'
            f'"ipc4-session-slot0-trace" = <{raw.hex()}>\n')


def before():
    return diag.summarize(fixture(0))


class Diagnostics(unittest.TestCase):
    def test_first_session_prepared_state(self):
        raw='"CFBundleIdentifier" = "com.ohmydsp.OhMyDSP.IPC4Session"\n"ipc4-session-ready" = Yes\n'
        marker='live capture ready: DSP idle, powers up when the user client starts the stream'
        self.assertIsNone(diag.summarize(raw)['state'])
        initial=diag.summarize(raw+marker)
        self.assertEqual((initial['state'],initial['session_number']),('prepared',0))
        self.assertEqual(diag.validate_transition(initial,diag.summarize(fixture())),([],[]))
        for extra in ['\n"ipc4-session-state" = "running"', '\nIPC4 session 1: starting', '\n"phase1-log-truncated" = Yes', '\n"ipc4-session-slot0-number" = 1']:
            self.assertNotEqual(diag.summarize(raw+marker+extra)['state'],'prepared')

    def test_clean_and_expected_fallback(self):
        message = '<wrn> ipc: ipc_pipeline_complete: ipc_pipeline_complete(): no scheduling component specified, use comp 0x20004\n'
        after = diag.summarize(fixture(message=message))
        self.assertEqual(diag.validate_transition(before(), after), ([], []))
        self.assertEqual(len(after['sessions'][0]['expected_scheduler_fallback']), 1)

    def test_missing_stop_and_extra_session(self):
        self.assertTrue(diag.validate_transition(before(), diag.summarize(fixture(stop=False)))[0])
        self.assertTrue(diag.validate_transition(before(), diag.summarize(fixture(2)))[0])

    def test_fault_and_overrun(self):
        for msg in ['<err> DMA fault\n', 'stats overruns 1\n', 'xrun detected\n']:
            self.assertTrue(diag.validate_transition(before(), diag.summarize(fixture(message=msg)))[0])

    def test_trace_capacity_and_unknown_warning(self):
        for data in [fixture(flags=4), fixture(message='<wrn> something new\n')]:
            errors, warnings = diag.validate_transition(before(), diag.summarize(data))
            self.assertFalse(errors)
            self.assertTrue(warnings)

    def test_corrupt_trace(self):
        data = fixture().replace('4d545231', '00000000')
        self.assertTrue(diag.validate_transition(before(), diag.summarize(data))[0])

    def test_active_or_absent_device(self):
        for data in ['', fixture().replace('"stopped"','"running"')]:
            self.assertTrue(diag.validate_transition(before(), diag.summarize(data))[0])

    def test_snapshot_excludes_legacy_pcm(self):
        pcm = b'PCM1-synthetic-private-audio'.hex()
        raw = fixture() + f'"ipc4-pcm-capture" = <{pcm}>\n'
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'source'
            source.write_text(raw)
            with patch.object(diag.subprocess, 'run') as ioreg:
                ioreg.return_value.stdout = raw
                ioreg.return_value.stderr = ''
                ioreg.return_value.returncode = 0
                for name, src in [('live', None), ('offline', source)]:
                    directory = Path(tmp) / name
                    data = diag.snapshot(directory, src)
                    self.assertIn('trace', data['sessions'][0])
                    for file in directory.iterdir():
                        self.assertNotIn(pcm, file.read_text())
                        self.assertNotIn('ipc4-pcm-capture', file.read_text())

    def test_export_never_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'source'; src.write_text(fixture())
            out=Path(tmp)/'out'; diag.snapshot(out,src)
            with self.assertRaises(FileExistsError): diag.snapshot(out,src)
            self.assertEqual(json.loads((out/'summary.json').read_text())['session_number'],1)


class FakeProcess:
    def __init__(self): self.done=False
    def poll(self): return 0 if self.done else None


class Runner(unittest.TestCase):
    def test_reports_fail_closed_and_silence_warns(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'report'
            self.assertTrue(runner.inspect_report(path,0)[1])
            path.write_text('{bad')
            self.assertTrue(runner.inspect_report(path,0)[1])
            path.write_text(json.dumps(dict(status='PASS',zero_frames=480,longest_zero_ms=10,channels=[])))
            self.assertTrue(runner.inspect_report(path,0)[2])
            self.assertTrue(runner.inspect_report(path,1)[1])
            path.write_text(json.dumps(dict(status='PASS',zero_frames=1,longest_zero_ms=0.021,channels=[])))
            self.assertFalse(runner.inspect_report(path,0)[2])

    def test_overlap_keeps_first_reader_and_checks_final_stop(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            def spawn(binary,duration,folder,name,sample):
                report=folder/(name+'.json'); report.write_text('{"status":"PASS"}')
                return FakeProcess(),report
            def wait(proc,timeout): proc.done=True; return 0
            active=diag.summarize(fixture()); active['state']='running'
            with patch.object(runner,'spawn',side_effect=spawn), patch.object(runner,'wait',side_effect=wait), \
                 patch.object(runner.time,'sleep'), patch.object(runner.diag,'snapshot',side_effect=[before(),active,active,diag.summarize(fixture())]):
                result=runner.run_group(Path('unused'),'overlap',12,folder,False)
            self.assertEqual(result['status'],'PASS')
            self.assertEqual(len(result['clients']),2)

    def test_busy_session_never_starts_client(self):
        active=before(); active['state']='running'
        with patch.object(runner.diag,'snapshot',return_value=active), patch.object(runner,'spawn') as spawn:
            result=runner.run_group(Path('unused'),'smoke',10,Path('unused'),False)
        spawn.assert_not_called()
        self.assertEqual(result['status'],'FAIL')


if __name__ == '__main__':
    unittest.main()
