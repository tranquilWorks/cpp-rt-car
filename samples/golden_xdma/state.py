"""Private derivation of golden_system's independent scalar/state oracle.
Only frozen tick-6 missing-command semantics differ; sibling sources are hashed.
"""
import struct
import importlib.util
from pathlib import Path
_spec=importlib.util.spec_from_file_location('xdma_state_cpu',Path(__file__).resolve().parent.parent/'golden_system/artifacts.py')
cpu=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(cpu)
require,FROZEN,CAMPAIGNS,FILES,fnv=cpu.require,cpu.FROZEN,cpu.CAMPAIGNS,cpu.FILES,cpu.fnv

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
            if e['fault'] == 'underflow': fault = 3
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
                    actuator[a][i] = 0 if absent or fault == 3 else max(-4,min(4,gain*(target-measured)))
        if t % 6 == 0:
            sums = [sum(row) for rows in (x,v) for row in rows]
    body = [value for rows in (x,v,acceleration,command,sensor_x,sensor_v) for row in rows for value in row]
    return body, sums, (target,gain,calibration,fault), stale, missing

def inspect_state(directory,e):
    """Shared complete state and paired transcript checks; no variant normalization."""
    ticks=e['ticks'];t=ticks-1
    controls=sum(x<ticks for x in (1,2,3,12,18))+(1 if e['campaign'] in ('stale_input','control_replaced','peer_missing') else 0)+int(e['fault']=='underflow')
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
    if e['fault']=='underflow':expected[136][2]-=2
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
