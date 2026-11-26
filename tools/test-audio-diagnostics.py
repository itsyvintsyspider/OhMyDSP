#!/usr/bin/env python3
import importlib.util
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('parser',Path(__file__).with_name('parse-audio-diagnostics.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


def evidence(**changes):
    hal=dict(v=1,source='hal',id=456,kernel_session=7,steady_missing=0,state_frames=0,snapshot_frames=0,
             clock_regressions=0,clock_snapshot_failures=0,negative=0,future=0,stale=0,dropped=0,skipped=0,events=1)
    hal.update(changes)
    daemon=dict(v=1,source='daemon',id=456,kernel_session=7,overruns=0,epoch_changes=0,stop_status=0,snapshot_failures=0,dropped=0,events=1)
    return [hal,daemon,dict(v=1,source='hal_event',id=456,index=0),dict(v=1,source='daemon_anchor',id=456,index=0)]


def parse(records):
    return module.parse('\n'.join('prefix OhMyDSP DIAG '+json.dumps(r) for r in records),[7])


class Tests(unittest.TestCase):
    def test_clean_and_dedup(self):
        result=parse(evidence()+evidence());s=result['sessions'][0]
        self.assertTrue(s['available']);self.assertFalse(s['errors'] or s['warnings'] or result['conflicts'])

    def test_platform_lap_period(self):
        for profile, period, valid in [('ipc3',16384,True),('ipc3',32768,True),
                                       ('ipc4',16384,False),('ipc4',131072,True)]:
            clock=dict(v=1,source='clock',id=456,contract=3,period=period,
                       profile=profile,failures=0,rejected=0)
            self.assertEqual(not parse(evidence()+[clock])['sessions'][0]['errors'],valid)

    def test_missing_summaries(self):
        for records in ([],evidence()[1:]): self.assertFalse(parse(records)['sessions'][0]['available'])

    def test_missing_counters_fail_closed(self):
        records=evidence();del records[0]['steady_missing']
        result=parse(records)['sessions'][0];self.assertFalse(result['available']);self.assertTrue(result['errors'])

    def test_actual_missing_frames_fail(self):
        result=parse(evidence(steady_missing=5044,future=5044))['sessions'][0]
        self.assertIn('HAL steady_missing=5044',result['errors'])

    def test_initial_missing_warn(self):
        result=parse(evidence(negative=480))['sessions'][0]
        self.assertFalse(result['errors']);self.assertTrue(result['warnings'])

    def test_process_correlation(self):
        records=evidence();records[0]['id']=999
        self.assertFalse(parse(records)['sessions'][0]['available'])

    def test_missing_and_dropped_events_warn(self):
        for records in (evidence()[:-1],evidence(dropped=1),evidence(skipped=2)):
            self.assertTrue(parse(records)['sessions'][0]['warnings'])

    def test_pump_policy(self):
        records=evidence()+[dict(v=1,source='pump',id=456,mode='deadline_thread',policy_status=0,period_us=2000,budget_us=200,constraint_us=1000,late_wakeups=4)]
        self.assertFalse(parse(records)['sessions'][0]['errors'])
        for field,value in [('policy_status',5),('mode','unknown'),('budget_us',1000),('late_wakeups',-1)]:
            changed=records[:-1]+[dict(records[-1],**{field:value})]
            self.assertTrue(parse(changed)['sessions'][0]['errors'])

    def test_clock_contract(self):
        records=evidence()+[dict(v=1,source='clock',id=456,contract=2,period=512,failures=0,rejected=0)]
        self.assertFalse(parse(records)['sessions'][0]['errors'])
        records[-1]['contract']=3;records[-1]['period']=131072
        self.assertFalse(parse(records)['sessions'][0]['errors'])
        records[-1]['failures']=1
        self.assertTrue(parse(records)['sessions'][0]['errors'])
        records[-1]['failures']=0;records[-1]['period']=16384
        self.assertTrue(parse(records)['sessions'][0]['errors'])

    def test_v1_timed_lifecycle_recovery(self):
        records=evidence();records[0]['kernel_session']=records[1]['kernel_session']=0
        records[0]['epoch']=records[1]['epoch']=9
        start=datetime(2000,1,1,tzinfo=timezone.utc)
        def prefix(seconds):
            return (start+timedelta(seconds=seconds)).strftime('%Y-%m-%d %H:%M:%S.%f%z  test-host ')
        lifecycle=(prefix(0)+'kernel[0]: (OhMyDSP) OhMyDSP: IPC4 session 7: starting\n'
                   +prefix(10)+'kernel[0]: (OhMyDSP) OhMyDSP: IPC4 session 7: stopped\n')
        def summaries(seconds):
            return '\n'.join(prefix(seconds)+'ohmydspd[1]: OhMyDSP DIAG '+json.dumps(r) for r in records)
        lines=summaries(12)  # Last instant inside the two-second recovery window.
        result=module.parse(lifecycle+lines,[7])['sessions'][0]
        self.assertTrue(result['available']);self.assertEqual(result['correlation'],'kernel-lifecycle-and-token')
        self.assertFalse(module.parse(lines,[7])['sessions'][0]['available'])
        self.assertFalse(module.parse(lifecycle+summaries(12.001),[7])['sessions'][0]['available'])
        self.assertFalse(module.parse(lifecycle.replace('7: starting','6: starting')+lines,[7])['sessions'][0]['available'])

    def test_malformed_conflicting_records(self):
        self.assertTrue(module.parse('OhMyDSP DIAG {truncated',[7])['malformed'])
        records=evidence();records.append(dict(records[0],future=1));self.assertTrue(parse(records)['conflicts'])


if __name__=='__main__':unittest.main()
