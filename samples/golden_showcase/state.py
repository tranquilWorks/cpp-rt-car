"""Derived complete golden-state checks; independent scalar reference stays immutable."""
import struct
from artifacts import cpu
require,fnv,FROZEN,CAMPAIGNS,reference=cpu.require,cpu.fnv,cpu.FROZEN,cpu.CAMPAIGNS,cpu.reference

def inspect_state(directory,e,filename="state.bin"):
    """Shared complete state and paired transcript checks; no variant normalization."""
    ticks=e['ticks'];t=ticks-1
    controls=sum(x<ticks for x in (1,2,3,12,18))+(1 if e['campaign'] in ('stale_input','control_replaced','peer_missing') else 0)
    b=(directory/filename).read_bytes()
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
    return e
