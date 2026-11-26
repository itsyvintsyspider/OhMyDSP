#!/usr/bin/env python3
"""Decode bounded HAL/daemon numeric diagnostics from unified logs (no audio payload)."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re


def parse(text, sessions):
    records = {}; malformed = []; conflicts = []; recovered = {}
    # v1 emitted kernel_session=0. Recover only beside a complete, timestamped kernel lifecycle.
    last_start = None; last_stop = None
    def stamp(line):
        m=re.match(r'(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+[+-]\d{4})',line)
        return datetime.strptime(m[1],'%Y-%m-%d %H:%M:%S.%f%z') if m else None
    for line in text.splitlines():
        if 'kernel[0]:' in line and '(OhMyDSP)' in line:
            lifecycle=re.search(r'IPC4 session (\d+): (starting|stopped)\s*$',line)
            if lifecycle:
                if lifecycle[2]=='starting': last_start=int(lifecycle[1]);last_stop=None
                elif last_start==int(lifecycle[1]): last_stop=(last_start,stamp(line))
        if 'OhMyDSP DIAG ' not in line:
            continue
        try:
            value = json.loads(line.split('OhMyDSP DIAG ', 1)[1])
            if value.get('v') != 1 or value.get('source') not in ('hal', 'hal_event', 'daemon', 'daemon_anchor', 'clock', 'pump') or not isinstance(value.get('id'), int):
                raise ValueError('unknown schema/source/id')
            if value['source'] in ('hal_event','daemon_anchor') and (not isinstance(value.get('index'),int) or not 0 <= value['index'] < 64):
                raise ValueError('invalid diagnostic event index')
            if value['source']=='daemon' and value.get('kernel_session')==0 and last_stop and last_stop[1]:
                emitted=stamp(line)
                if emitted and 0 <= (emitted-last_stop[1]).total_seconds() <= 2:
                    recovered[value['id']]=last_stop[0]
            key = (value['source'], value['id'], value.get('index', -1))
            if key in records and records[key] != value:
                conflicts.append(str(key))
            records[key] = value
        except (ValueError, TypeError, AttributeError) as exc:
            malformed.append(str(exc))
    results = []
    for session in sessions:
        candidates = [v for v in records.values() if v['source'] == 'daemon' and (v.get('kernel_session') == session or (v.get('kernel_session') == 0 and recovered.get(v['id']) == session))]
        result = dict(kernel_session=session, available=False, errors=[], warnings=[])
        if len(candidates) != 1:
            result['warnings'].append('missing or ambiguous daemon summary for kernel session')
            results.append(result); continue
        daemon = candidates[0]; identity = daemon['id']
        hal = records.get(('hal', identity, -1))
        fallback=daemon.get('kernel_session')==0 and recovered.get(identity)==session
        if not hal or not (hal.get('kernel_session')==session or (fallback and hal.get('kernel_session')==0 and hal.get('epoch')==daemon.get('epoch'))):
            result['warnings'].append('matching HAL summary unavailable')
            results.append(result); continue
        result['correlation']='kernel-lifecycle-and-token' if fallback else 'explicit-session-and-token'
        if fallback: result['warnings'].append('v1 zero session number recovered from kernel stop timing and matching HAL/daemon token; original records retained')
        result.update(available=True, id=identity, hal=hal, daemon=daemon,
                      hal_events=sorted([v for v in records.values() if v['source']=='hal_event' and v['id']==identity],key=lambda x:x['index']),
                      daemon_anchors=sorted([v for v in records.values() if v['source']=='daemon_anchor' and v['id']==identity],key=lambda x:x['index']))
        required_hal = ('steady_missing','state_frames','snapshot_frames','clock_regressions','clock_snapshot_failures','negative','future','stale','dropped','skipped','events')
        required_daemon = ('overruns','epoch_changes','stop_status','snapshot_failures','dropped','events')
        if any(not isinstance(hal.get(k),int) or hal[k]<0 for k in required_hal) or any(not isinstance(daemon.get(k),int) or daemon[k]<0 for k in required_daemon):
            result['available']=False
            result['errors'].append('incomplete diagnostic counters')
            results.append(result);continue
        if hal['events']>64 or daemon['events']>64:
            result['available']=False;result['errors'].append('event count exceeds archive bounds')
            results.append(result);continue
        pump=records.get(('pump',identity,-1))
        if pump:
            result['pump']=pump
            if pump.get('mode')!='deadline_thread' or not isinstance(pump.get('late_wakeups'),int) or pump['late_wakeups']<0 or pump.get('policy_status')!=0 or pump.get('period_us')!=2000 or pump.get('budget_us')!=200 or pump.get('constraint_us')!=1000:
                result['errors'].append('capture pump realtime policy unavailable or incorrect')
        clock=records.get(('clock',identity,-1))
        if clock:
            result['clock']=clock
            period = clock.get('period')
            if clock.get('profile') == 'ipc3':
                valid_period = (clock.get('contract') == 3 and isinstance(period, int)
                                and 2048 <= period <= 131072 and period % 1024 == 0)
            else:
                valid_period = clock.get('contract') in (2,3) and period == {2:512,3:131072}[clock['contract']]
            if not valid_period or not isinstance(clock.get('failures'),int) or not isinstance(clock.get('rejected'),int):
                result['errors'].append('invalid clock contract summary')
            elif clock['failures'] or clock['rejected']:
                result['errors'].append('device clock estimator failed or rejected hardware anchors')
        for field in ('steady_missing','state_frames','snapshot_frames','clock_regressions','clock_snapshot_failures'):
            if hal[field]: result['errors'].append(f'HAL {field}={hal[field]}')
        for field in ('overruns','epoch_changes','stop_status'):
            if daemon[field]: result['errors'].append(f'daemon {field}={daemon[field]}')
        if hal['negative']+hal['future']+hal['stale'] and not hal['steady_missing']:
            result['warnings'].append('unavailable frames confined to startup sample range; inspect HAL events')
        if hal['dropped'] or hal['skipped'] or daemon['dropped'] or daemon['snapshot_failures']:
            result['warnings'].append('bounded diagnostic coverage/position reads incomplete')
        for source, events in ((hal,result['hal_events']),(daemon,result['daemon_anchors'])):
            if [e['index'] for e in events] != list(range(source['events'])):
                result['warnings'].append('unified log omitted diagnostic events')
        results.append(result)
    return dict(schema=1, sessions=results, malformed=malformed, conflicts=conflicts)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('--sessions', type=int, nargs='+', required=True)
    args=p.parse_args()
    print(json.dumps(parse(args.log.read_text(),args.sessions),indent=2))


if __name__=='__main__': main()
