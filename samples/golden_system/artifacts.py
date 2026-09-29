"""Independent offline validation of actual portable golden execution artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

HERE = Path(__file__).resolve().parent
FROZEN = '9c93b5d5970caa5dc4570a4d0c43c9ca21ab7aa5a7211c02ce946cf8ffa62771'
CONFIG = ('mode', 'count', 'ticks', 'workers', 'grain', 'campaign', 'external')
CAMPAIGNS = ('nominal', 'overload', 'stale_input', 'control_rejected', 'control_replaced', 'peer_missing')
FILES = ('execution.json', 'state.bin', 'checkpoint.bin', 'active.bin', 'trusted.bin')

def require(value, message):
    if not value:
        raise ValueError(message)

def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()

def sha(value):
    return hashlib.sha256(value).hexdigest()

def fnv(value):
    h = 14695981039346656037
    for b in value:
        h = ((h ^ b) * 1099511628211) & ((1 << 64)-1)
    return h

def load(path):
    def unique(pairs):
        out = {}
        for key, value in pairs:
            require(key not in out, 'duplicate JSON key')
            out[key] = value
        return out
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)

def contract():
    d = load(HERE / 'contract.json')
    require(sha(canonical(d)) == FROZEN, 'frozen contract changed')
    return d

def provenance(path):
    p = load(path)
    require(set(p) == {'schema', 'source_commit', 'source_tree', 'dirty', 'files'}, 'provenance fields')
    require(p['schema'] == 1 and type(p['dirty']) is bool, 'provenance schema')
    for key in ('source_commit', 'source_tree'):
        require(isinstance(p[key], str) and re.fullmatch('[0-9a-f]{40}', p[key]), 'source identity')
    require(isinstance(p['files'], dict) and len(p['files']) >= 15, 'source inventory')
    for name, checksum in p['files'].items():
        require(Path(name).name == name and name not in ('provenance.json', 'run.json'), 'source path')
        require((HERE/name).is_file() and sha((HERE/name).read_bytes()) == checksum, 'source digest: '+name)
    actual = {x.name for x in HERE.iterdir() if x.is_file() and x.name != 'provenance.json'}
    require(actual == set(p['files']), 'source inventory coverage')
    return p

def reference(e):
    """Iterative scalar Python reference, independent of the C++ segment oracle."""
    n, ticks = e['count'], e['ticks']
    x, v, acceleration = [[[0]*256 for _ in range(3)] for _ in range(3)]
    seed = 1
    def draw(modulus, offset):
        nonlocal seed
        seed = (1664525*seed+1013904223) % 2**32
        return seed % modulus-offset
    for i in range(n):
        for a in range(3):
            x[a][i], v[a][i], acceleration[a][i] = draw(2049,1024), draw(129,64), draw(9,4)
    actuator = [row[:] for row in acceleration]
    sensor_x, sensor_v = [[[0]*256 for _ in range(3)] for _ in range(2)]
    sums = [0]*6
    stale = missing = 0
    target, gain, calibration, fault = 8, 1, 0, 0
    for t in range(ticks):
        if t == 1: target = 16
        if t == 2: calibration = 2
        if t == 3: gain = 2
        if t == 6:
            if e['campaign'] == 'control_replaced': gain = 3
            if e['campaign'] == 'stale_input': fault = 2
            if e['campaign'] == 'peer_missing': fault = 10
        if t in (12,18): fault = 0
        old_sensor = [row[:] for row in sensor_v]
        command = [row[:] for row in actuator]
        for a in range(3):
            for i in range(n):
                acceleration[a][i] = command[a][i]
                v[a][i] += command[a][i]
                x[a][i] += v[a][i]
        if t % 2 == 0:
            for a in range(3):
                for i in range(n):
                    sensor_x[a][i], sensor_v[a][i] = x[a][i]+calibration, v[a][i]+calibration
        if t % 3 == 0:
            expired = e['campaign'] == 'stale_input' and t == 6
            absent = fault == 10 or (e['external'] and t//3 >= e['peer_responses'])
            stale += expired
            missing += absent
            for a in range(3):
                for i in range(n):
                    measured = 0 if expired else (old_sensor[a][i] if e['external'] else sensor_v[a][i])
                    actuator[a][i] = 0 if absent else max(-4,min(4,gain*(target-measured)))
        if t % 6 == 0:
            sums = [sum(row) for rows in (x,v) for row in rows]
    body = [value for rows in (x,v,acceleration,command,sensor_x,sensor_v) for row in rows for value in row]
    return body, sums, (target,gain,calibration,fault), stale, missing

def inspect(directory):
    d = contract()
    e = load(directory/'execution.json')
    for key, lo, hi in [('count',1,256),('ticks',1,1024),('workers',1,3),('grain',1,64)]:
        require(type(e[key]) is int and lo <= e[key] <= hi, 'configuration '+key)
    require(e['grain'] in (1,4,16,64) and e['mode'] in ('native','host') and type(e['external']) is bool and e['campaign'] in CAMPAIGNS, 'configuration enum')
    require(e['campaign'] == 'nominal' or e['ticks'] >= 19, 'fault horizon')
    numeric=('schema','runtime_id','runtime_planned_bytes','sample_owned_bytes','trace_events','trace_lost','control_accepted','control_invalid','control_replaced','control_committed','replay_frames','replay_actions','replay_generations','recoveries','jobs_accepted','jobs_completed','memory_acquired','memory_released','peer_responses','rate_actions','mixed_actions','control_actions','action_gaps','deadline_failures')
    for key in numeric:require(type(e[key]) is int and 0 <= e[key] < 2**64, 'execution integer '+key)
    require(isinstance(e['session_identity'],str) and re.fullmatch('[1-9][0-9]*-[1-9][0-9]*',e['session_identity']), 'session identity')
    require(isinstance(e['phase_calls'],list) and all(type(x) is int for x in e['phase_calls']), 'phase integer counts')
    require(e['schema'] == 1 and e['contract_sha256'] == FROZEN, 'execution identity')
    require(e['oracle'] is True and e['cleanup'] is True and e['trace_lost'] == 0 and e['trace_events'] > 0, 'execution/cleanup/telemetry')
    require(0 < e['runtime_planned_bytes'] <= d['execution']['runtime_budget_bytes'] and 0 < e['sample_owned_bytes'] <= d['execution']['sample_budget_bytes'], 'memory budgets')
    require(e['action_gaps']==0 and e['deadline_failures']==(1 if e['campaign']=='overload' else 0) and all(e[k]>0 for k in ('rate_actions','mixed_actions','control_actions')), 'action telemetry')
    require(e['memory_acquired'] == e['memory_released'] == (6 if e['campaign']=='overload' else 3), 'memory conservation')
    require(e['jobs_accepted'] == e['jobs_completed'] and (e['mode'] != 'host' or e['jobs_accepted'] > 0), 'job conservation')
    ticks=e['ticks'];t=ticks-1
    require(e['phase_calls'] == [t//p+1 for p in (1,1,1,2,3,3,6,6)], 'phase counts')
    require(e['recoveries'] == (1 if e['campaign']=='overload' else 0) and e['replay_frames'] == ticks-(6 if e['campaign']=='overload' else 0), 'replay/recovery count')
    require(e['control_invalid'] == (1 if e['campaign']=='control_rejected' else 0) and e['control_replaced'] == (1 if e['campaign']=='control_replaced' else 0), 'admission outcomes')
    releases=sum(t//period+1 for period in (1,2,3,3,6))+e['recoveries']
    require(e['rate_actions']==releases and e['mixed_actions']==releases, 'exact rate/mixed action counts')
    controls = sum(ticks>b for b in (1,2,3,12,18))+(e['campaign'] in ('stale_input','peer_missing','control_replaced'))
    require(e['control_committed'] == controls and e['control_accepted'] == controls+e['control_replaced'], 'control counts')
    require(e['replay_generations'] == (2 if e['campaign']=='overload' else controls) and e['replay_actions'] > 0, 'replay transcript')
    require(type(e['peer_responses']) is int and 0 <= e['peer_responses'] <= t//3+1, 'peer count')
    if not e['external']: require(e['peer_responses']==0 and e['peer_status']=='ok' and e['peer_cleanup']=='ok', 'nonexternal peer claims')
    elif e['peer_status']=='ok': require(e['peer_responses']==t//3+1, 'peer completeness')
    inspect_state(directory,e)
    return e

def inspect_state(directory,e):
    """Shared complete state and paired transcript checks; no variant normalization."""
    ticks=e['ticks'];t=ticks-1
    controls=sum(x<ticks for x in (1,2,3,12,18))+(1 if e['campaign'] in ('stale_input','control_replaced','peer_missing') else 0)
    b=(directory/'state.bin').read_bytes()
    require(len(b)==18944, 'state size')
    u32=lambda at:struct.unpack_from('<I',b,at)[0]
    u64=lambda at:struct.unpack_from('<Q',b,at)[0]
    require([u32(at) for at in range(0,40,4)] == [0x5336324d,1,e['count'],ticks,e['workers'],e['grain'],int(e['mode']=='host'),int(e['external']),CAMPAIGNS.index(e['campaign']),0], 'state configuration')
    require(b[448:480].hex()==FROZEN and not any(b[488:512]), 'state contract/reserved')
    checked=bytearray(b);checked[480:488]=bytes(8)
    require(fnv(checked)==u64(480), 'state checksum')
    body,sums,configuration,stale,missing=reference(e)
    require(list(struct.unpack_from('<4608i',b,512))==body, 'every-field independent state oracle')
    require(struct.unpack_from('<4i',b,40)==configuration and u64(56)==ticks and bool(u64(64))==bool(controls) and u64(392)==controls, 'state control generation')
    require(struct.unpack_from('<6q',b,400)==tuple(sums) and u64(376)==stale and u64(384)==missing, 'aggregate/fault oracle')
    require(list(struct.unpack_from('<8Q',b,72))==e['phase_calls'], 'state phase counts')
    expected={136:[ticks,t//2+1,t//3+1,t//3+1,ticks],176:[t//2+1,t//3+1,t//3+1,ticks,t//6+1],216:[0,(t//3*3%2)*10**7,0,((t-1)%3+1)*10**7 if t else 0,0],256:[t//2*2+2,t//3*3//2+2,t//3+2,(t-1)//3+2 if t else 1,t//6*6+2],296:[t//2*2,t//3*3//2,t//3,(t-1)//3 if t else 0,t//6*6]}
    for at,values in expected.items():require(list(struct.unpack_from('<5Q',b,at))==values, 'channel metadata '+str(at))
    times=[t//2*2,t//3*3//2*2,t//3*3,(t-1)//3*3 if t else 0,t//6*6]
    if e['campaign']=='stale_input' and t//3*3==6:times[1]=2
    require(list(struct.unpack_from('<5Q',b,336))==[x*10**7 for x in times], 'selected payload times')
    for name in FILES[2:]:require(0 < (directory/name).stat().st_size <= 16*1024**2, 'artifact size')
    trusted=(directory/'trusted.bin').read_bytes()
    require(trusted[:8]==b'RTFWLCR2' and len(trusted)>=400, 'trusted lossless schema')
    word=lambda at:struct.unpack_from('<Q',trusted,at)[0]
    count=lambda at:struct.unpack_from('<I',trusted,at)[0]
    checked=bytearray(trusted);checked[24:32]=bytes(8)
    require(word(16)==len(trusted) and fnv(checked)==word(24), 'trusted checksum')
    require(word(32)==e['runtime_id'] and count(184)==e['replay_generations'] and count(168)==e['replay_actions'], 'trusted runtime/transcript identity')
    for file,off,size in [('checkpoint.bin',96,104),('active.bin',120,128)]:
        require(trusted[word(off):word(off)+word(size)]==(directory/file).read_bytes(), 'nested artifact pairing')
    if count(184):
        last=word(176)+(count(184)-1)*count(188)
        require(count(188)==128 and last+128<=len(trusted) and struct.unpack_from('<Q',trusted,last+40)[0]==u64(64), 'state generation binding')
    return e

def expected(directory, p):
    e=inspect(directory)
    configuration={key:e[key] for key in CONFIG}
    identity={'contract_sha256':FROZEN,'configuration_sha256':sha(canonical(configuration))}
    files={name:sha((directory/name).read_bytes()) for name in FILES}
    good=e['peer_status']=='ok' and e['peer_cleanup']=='ok'
    run=dict(identity,session_identity=e['session_identity'],runtime_id=e['runtime_id'],source_commit=p['source_commit'],source_tree=p['source_tree'],source_dirty=p['dirty'],source_files=p['files'],variant='cpu_external' if e['external'] else 'cpu',host=e['mode'],evidence='portable_process' if e['external'] else 'portable_cpu',status='PASS' if good else 'FAIL',configuration=configuration,release_counts=[(e['ticks']-1)//period+1 for period in (1,2,3,3,6)],phase_calls=e['phase_calls'],state_digest=files['state.bin'],oracle='independent_every_field_exact',cleanup=e['cleanup'] and e['peer_cleanup']=='ok',files=files)
    replay=dict(identity,sample_snapshot_digest=files['state.bin'],runtime_snapshot_digest=files['checkpoint.bin'],trusted_payload_digest=files['trusted.bin'],action_digest=files['active.bin'],gap_count=e['trace_lost'],replay_status='PASS',frames=e['replay_frames'],actions=e['replay_actions'],generations=e['replay_generations'],ownership='originating_runtime',recovery='fresh_owner_checkpoint_resume' if e['recoveries'] else 'not_required')
    return run,replay

def validate(directory, provenance_path, publish=False):
    p=provenance(provenance_path);run,replay=expected(directory,p)
    if publish:
        for name,value in [('run.json',run),('replay.json',replay)]:
            (directory/name).write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
    require(load(directory/'run.json')==run, 'run artifact mismatch')
    require(load(directory/'replay.json')==replay, 'replay artifact mismatch')
    return run

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--provenance',type=Path,default=HERE/'provenance.json');p.add_argument('--publish',action='store_true');a=p.parse_args()
    try:
        result=validate(a.directory,a.provenance,a.publish)
        require(result['status']=='PASS','retained failed execution')
    except (ValueError,OSError,KeyError,TypeError) as e:p.exit(1,str(e)+'\n')
    print('PASS: independent golden run/state/replay validation')
if __name__=='__main__':main()
