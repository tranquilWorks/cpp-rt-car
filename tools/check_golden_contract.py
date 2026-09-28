#!/usr/bin/env python3
"""Offline M26-01 design validation. This does not execute the Runtime scenario."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / 'samples/golden_system/contract.json'
# Canonical JSON (sorted keys, compact ASCII); update only with reviewed versioning.
FROZEN_SHA256 = '9c93b5d5970caa5dc4570a4d0c43c9ca21ab7aa5a7211c02ce946cf8ffa62771'

# Closed, recursive wire/document shape. i excludes bool and floating point.
SHAPES = {
    'root': 'version:i scenario_id:s status:s implementation_batch:s state:state clock:clock rates:[rate] phases:[phase] resources:[s] edges:[[s]] channels:[channel] execution:execution controls:controls faults:[fault] levers:[lever] benchmarks:benchmarks artifacts:[artifact] telemetry:telemetry variants:[variant] external_cil:external sources:[source] nonclaims:[s]',
    'state': 'capacity:i default_entities:i axes:i layout:s alignment:i scalar:s owner:s fields:[field] plant_bytes:i controller_bytes:i sensor_bytes:i initialization:s update:s controller:s oracle:s rollback:s source:s auxiliary_layout:s',
    'field': 'id:s arrays:i unit:s initial_abs:i maximum_abs:i offset_bytes:i',
    'clock': 'base_tick_ns:i default_ticks:i maximum_ticks:i supercycle_ticks:i epoch:s substeps:i host_step_ticks:i',
    'rate': 'id:s period_ticks:i budget_ns:i default_releases:i',
    'phase': 'id:s rate:s kind:s reads:[s] writes:[s] default_calls:i',
    'channel': 'id:s identity:i producer:s consumer:s elements_per_entity:i capacity_payload_bytes:i frame_header_bytes:i ring_slots:i max_age_ticks:i encoding:s scale:[i] offset:[i] calibration_identity:i trigger_identity:i sequence_start:i samples_per_frame:i timestamp:s initial:s stale:s underrun:s overrun:s safe:s device_rule:s units_identity:i payload_layout:s clock_domain_identity:i sample_interval_ns:i device_timeout_ns:i',
    'execution': 'hosts:[s] workers:[i] default_workers:i entity_grains:[i] default_grain:i nested_depth:i axes_tasks:i task_slots:i queue_slots:i task_scratch_bytes:i default_inflight:i maximum_inflight:i runtime_budget_bytes:i sample_budget_bytes:i lifecycle_timeout_ms:i cleanup_attempts:i policy:s ordering:s staging:s',
    'controls': 'schema:i envelope:s mailbox:i producer:i slots:i payload_stride:i initial_sequence:i rollback:s schemas:[control] boundaries:[boundary] ordering:s replay:s checkpoint_bytes:i artifact_bytes:i application_latch:s',
    'control': 'id:s type_id:i kind:i body_bytes:i fields:[control_field]',
    'control_field': 'id:s minimum:i maximum:i default:i offset:i',
    'boundary': 'kind:s phase:s tick:i target:s',
    'fault': 'id:s index:i phase:s tick:i detection_releases:i outcome:s recovery:s batch:s status:s',
    'lever': 'id:s values:[i] scope:s rule:s',
    'benchmarks': 'provider:s provider_version:i descriptor_schema:i warmup:i repetitions:i retain_raw:b parameter_limit:i counter_limit:i claim:s cases:[case]',
    'case': 'id:s subsystem:s batch:s status:s timing:s oracle:s counters:[s]',
    'artifact': 'id:s path:s fields:[s] batch:s status:s rule:s',
    'telemetry': 'identity:s application_schema:i rate_schema:i mixed_rate_schema:i live_control_schema:i global_schema:i capacity_each:i drain:s loss:s fields:[s]',
    'variant': 'id:s batch:s status:s evidence:s requirements:[s] device_phases:[s]',
    'external': 'protocol:s polls_per_boundary:i peer_timeout_ms:i platforms:[s]',
    'source': 'path:s tokens:[s]',
}


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def shape(value, spec: str, path: str = 'contract') -> None:
    if spec.startswith('['):
        require(type(value) is list, f'{path}: expected list')
        for index, item in enumerate(value):
            shape(item, spec[1:-1], f'{path}[{index}]')
    elif spec in ('i', 's', 'b'):
        require(type(value) is {'i': int, 's': str, 'b': bool}[spec], f'{path}: wrong scalar type')
        if spec == 's':
            require(bool(value.strip()), f'{path}: empty string')
        elif spec == 'i':
            require(-(2**63) < value < 2**63, f'{path}: integer overflow')
    else:
        expected = dict(item.split(':', 1) for item in SHAPES[spec].split())
        require(type(value) is dict and value.keys() == expected.keys(), f'{path}: missing/unknown fields')
        for key, child in expected.items():
            shape(value[key], child, f'{path}.{key}')


def unique(rows, key='id') -> dict:
    result = {x[key]: x for x in rows}
    require(len(result) == len(rows) and bool(rows), f'duplicate/empty {key} inventory')
    return result


def digest(data: dict) -> str:
    return hashlib.sha256(json.dumps(data, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode('ascii')).hexdigest()


def no_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'duplicate JSON key: {key}')
        result[key] = value
    return result


def read(path=CONTRACT):
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=no_duplicate_keys)


def validate(data: dict, root: Path = ROOT, *, frozen=True) -> dict:
    shape(data, 'root')
    require(data['version'] == 1 and data['scenario_id'] == 'rtfw.golden.vehicle.v1', 'scenario/version identity')
    require(data['status'] == 'contract_only' and data['implementation_batch'] == 'M26-02', 'implementation overclaim')
    state, clock, execution = data['state'], data['clock'], data['execution']
    require(1 <= state['default_entities'] <= state['capacity'] <= 256, 'entity capacity')
    require(state['axes'] == 3 and state['alignment'] == 64 and state['scalar'] == 'int32_le', 'state layout')
    fields = unique(state['fields'])
    require(set(fields) == {'position', 'velocity', 'acceleration'}, 'plant fields')
    require(all(f['arrays'] == 3 and 0 <= f['initial_abs'] <= f['maximum_abs'] < 2**31 for f in fields.values()), 'field bounds')
    n = clock['maximum_ticks']
    require(1 <= clock['default_ticks'] <= n <= 1024 and clock['base_tick_ns'] > 0, 'clock bounds')
    require(clock['substeps'] == 1 and clock['host_step_ticks'] == 1, 'clock mapping')
    x, v, a = (fields[f] for f in ('position', 'velocity', 'acceleration'))
    xmax = x['initial_abs'] + n*v['initial_abs'] + a['maximum_abs']*n*(n+1)//2
    vmax = v['initial_abs'] + n*a['maximum_abs']
    require(xmax == x['maximum_abs'] and vmax == v['maximum_abs'] and a['maximum_abs'] == 4, 'arithmetic envelope')
    require([f['offset_bytes'] for f in fields.values()] == [0,3072,6144], 'SoA field offsets')
    require(state['plant_bytes'] == state['capacity']*9*4 and state['controller_bytes'] == state['capacity']*3*4 and state['sensor_bytes'] == state['capacity']*6*4, 'SoA bytes')
    # Independent iterative extremal paths versus closed-form bound, both signs.
    for sign in (-1, 1):
        px, pv = sign*x['initial_abs'], sign*v['initial_abs']
        for _ in range(n):
            pv += sign*a['maximum_abs']; px += pv
        require(px == sign*xmax and pv == sign*vmax, 'closed-form mismatch')
    # Piecewise constant oracle with signed changes, independent weighted sum.
    segments = [(1, -4), (3, 4), (7, -1), (n-11, 2)] if n >= 11 else [(n, -4)]
    sx, sv, accelerations = 17, -9, []
    for length, acceleration in segments:
        sx += length*sv + acceleration*length*(length+1)//2
        sv += length*acceleration
        accelerations.extend([acceleration]*length)
    require(sx == 17+n*(-9)+sum((n-i)*a for i,a in enumerate(accelerations)) and sv == -9+sum(accelerations), 'segment oracle')

    rates, phases = unique(data['rates']), unique(data['phases'])
    require(set(rates) == {'plant','sensor','controller','actuator','observe'}, 'rate coverage')
    require(set(phases) == {'input','physics','stage','sensor','controller','actuator','aggregate','telemetry'}, 'phase coverage')
    for rate in rates.values():
        period = rate['period_ticks']
        require(1 <= period <= n and 0 < rate['budget_ns'] <= period*clock['base_tick_ns'], 'invalid rate/budget')
        require(rate['default_releases'] == len(range(0, clock['default_ticks'], period)), 'release count')
    require(math.lcm(*(r['period_ticks'] for r in rates.values())) == clock['supercycle_ticks'], 'supercycle')
    horizon = clock['supercycle_ticks']
    require(horizon <= n, 'reference horizon')
    resources = data['resources']
    require(len(set(resources)) == len(resources) and set(resources) == {'command','plant','stage','sensor','controller','actuator','aggregate'}, 'resource inventory')
    adjacency = {p: set() for p in phases}
    for edge in data['edges']:
        require(len(edge) == 2 and all(p in phases for p in edge) and edge[0] != edge[1], 'dangling/self edge')
        a,b = edge
        require(b not in adjacency[a], 'duplicate edge')
        adjacency[a].add(b)
        require(phases[a]['rate'] == phases[b]['rate'], 'cross-domain ordinary edge')
    visiting, visited = set(), set()
    def visit(p):
        require(p not in visiting, 'cyclic phase graph')
        if p in visited:
            return
        visiting.add(p)
        for q in adjacency[p]: visit(q)
        visiting.remove(p); visited.add(p)
    for p in phases: visit(p)
    def reaches(a,b):
        return b in adjacency[a] or any(reaches(q,b) for q in adjacency[a])
    for phase in phases.values():
        require(phase['rate'] in rates and phase['kind'] in ('cpu','cpu_or_cuda','cpu_or_xdma'), 'phase binding')
        require(phase['default_calls'] == rates[phase['rate']]['default_releases'], 'phase count')
        require(len(set(phase['reads']+phase['writes'])) == len(phase['reads']+phase['writes']) and set(phase['reads']+phase['writes']) <= set(resources), 'phase resource')
    for i,p in enumerate(data['phases']):
        for q in data['phases'][i+1:]:
            conflict = set(p['writes']) & set(q['reads']+q['writes']) or set(q['writes']) & set(p['reads'])
            require(not conflict or reaches(p['id'],q['id']) or reaches(q['id'],p['id']), 'unordered resource conflict')
    # Every rate must own a phase. Exact ordered reference records and channel ages.
    require({p['rate'] for p in phases.values()} == set(rates), 'empty rate domain')
    order = {r:i for i,r in enumerate(rates)}
    records = sorted((t,order[p['rate']],i,p['id']) for i,p in enumerate(data['phases']) for t in range(0,horizon,rates[p['rate']]['period_ticks']))
    require(len(records) <= 65536, 'reference capacity')
    channels = unique(data['channels']); unique(data['channels'],'identity')
    require(set(channels) == {'plant_sensor','sensor_controller','controller_actuator','actuator_input','plant_aggregate'}, 'channel coverage')
    variants = unique(data['variants'])
    ages = {}
    for channel in channels.values():
        a,b = channel['producer'],channel['consumer']
        require(a in phases and b in phases and phases[a]['rate'] != phases[b]['rate'], 'cross-rate endpoints')
        require(channel['capacity_payload_bytes'] == channel['elements_per_entity']*state['capacity']*4 and channel['elements_per_entity'] in (3,6), 'channel geometry')
        require(channel['frame_header_bytes'] == 120 and channel['capacity_payload_bytes']+120 <= 65536, 'sample frame extent')
        require(channel['units_identity'] > 0 and channel['clock_domain_identity'] == 1 and channel['sample_interval_ns'] == clock['base_tick_ns'] and 0 < channel['device_timeout_ns'] <= clock['base_tick_ns'], 'sample units/clock/timeout')
        require(channel['samples_per_frame'] == 1 and channel['ring_slots'] >= execution['maximum_inflight']+1, 'sample ring capacity')
        require(channel['scale'] == [1,1] and channel['offset'] == [0,1] and channel['encoding'] == 'signed_int32_le', 'sample encoding/scale')
        require(channel['calibration_identity'] > 0 and channel['trigger_identity'] > 0 and channel['sequence_start'] == 1, 'sample identity')
        require(channel['stale'] == 'substitute_initial' and channel['underrun'] == 'substitute_safe' and channel['overrun'] == 'fail_release', 'sample failure policy')
        worst = 0
        sources = [(i,r[0]) for i,r in enumerate(records) if r[3] == a]
        for i,r in enumerate(records):
            if r[3] != b: continue
            prior = [t for j,t in sources if j < i]
            # Check both first and repeating horizons independently.
            first = r[0] - (prior[-1] if prior else 0)
            repeating = r[0] - (prior[-1] if prior else sources[-1][1]-horizon)
            worst = max(worst, first, repeating)
        require(worst <= channel['max_age_ticks'] <= n, 'unsafe sample maximum age')
        ages[channel['id']] = worst
        for variant in variants.values():
            require(not (a in variant['device_phases'] and b in variant['device_phases']), 'device/device channel')

    require(execution['hosts'] == ['native','independent_host'] and execution['workers'] == [1,2,3] and execution['default_workers'] in execution['workers'], 'host/worker contract')
    require(execution['entity_grains'] == [1,4,16,64] and execution['default_grain'] in execution['entity_grains'], 'grain bounds')
    max_tasks = state['axes']*(1+math.ceil(state['capacity']/min(execution['entity_grains'])))
    for count in (1,16,17,255,state['capacity']):
        for grain in execution['entity_grains']:
            visits = [0]*count
            for first in range(0,count,grain):
                for index in range(first,min(first+grain,count)):
                    visits[index] += 1
            require(visits == [1]*count, 'partition gap/overlap')
    require(execution['nested_depth'] == 2 and execution['axes_tasks'] == 3 and execution['task_slots'] >= max_tasks and execution['queue_slots'] >= max_tasks, 'nested capacity')
    require(1 <= execution['default_inflight'] <= execution['maximum_inflight'] <= 2, 'device depth')
    require(0 < execution['lifecycle_timeout_ms'] <= 5000 and execution['cleanup_attempts'] == 2, 'unbounded cleanup')
    require(execution['runtime_budget_bytes'] == 128*1024**2 and execution['sample_budget_bytes'] == 32*1024**2 and execution['task_scratch_bytes'] == 64, 'memory budgets')
    controls = data['controls']; schemas = unique(controls['schemas']); unique(controls['schemas'],'type_id')
    require(set(schemas) == {'scenario','controller','calibration','fault','clear_fault'} and [s['kind'] for s in schemas.values()] == [1,2,3,4,5], 'control schema inventory')
    require(controls['schema'] == 1 and controls['mailbox'] > 0 and controls['producer'] > 0 and controls['slots'] == 16 and controls['initial_sequence'] == 1, 'control capacity/identity')
    require(controls['rollback'] == 'restore_step_entry_generation', 'control rollback')
    for schema in schemas.values():
        require(schema['body_bytes'] == 16 and controls['payload_stride'] >= schema['body_bytes']+32, 'control payload geometry')
        unique(schema['fields']); offsets = []
        for field in schema['fields']:
            require(-(2**31) <= field['minimum'] <= field['default'] <= field['maximum'] < 2**31, 'control bounds')
            require(0 <= field['offset'] <= schema['body_bytes']-4 and field['offset'] % 4 == 0, 'control field offset')
            offsets.append(field['offset'])
        require(len(set(offsets)) == len(offsets), 'overlapping control fields')
    require(schemas['controller']['fields'][0]['maximum'] == 4 and schemas['scenario']['fields'][0]['minimum'] == -64 and schemas['scenario']['fields'][0]['maximum'] == 64 and schemas['calibration']['fields'][0]['minimum'] == -16 and schemas['calibration']['fields'][0]['maximum'] == 16, 'unsafe controller/calibration envelope')
    require(4*(64+vmax+16) < 2**31, 'controller intermediate overflow')
    for boundary in controls['boundaries']:
        require(boundary['kind'] in schemas and boundary['phase'] in phases and 0 <= boundary['tick'] < clock['default_ticks'], 'control boundary reference')
        require(boundary['target'] in ('rate','host_frame'), 'control boundary target')
        if boundary['target'] == 'rate':
            require(boundary['tick'] < horizon and boundary['tick'] % rates[phases[boundary['phase']]['rate']]['period_ticks'] == 0, 'rate control not exact first-cycle release')
    require({b['kind'] for b in controls['boundaries']} == set(schemas), 'control boundary coverage')
    require(controls['checkpoint_bytes'] == 4*1024**2 and controls['artifact_bytes'] == 16*1024**2, 'replay byte bounds')
    faults = unique(data['faults'])
    require(set(faults) == {'overload','stale_input','underflow','overrun','device_loss','control_rejected','control_replaced','reset_failure','stop_failure','peer_missing','telemetry_loss'}, 'fault coverage')
    for fault in faults.values():
        require(fault['index'] == list(faults).index(fault['id'])+1 and schemas['fault']['fields'][0]['maximum'] == len(faults), 'fault selector identity')
        require(fault['phase'] in phases and 0 <= fault['tick'] < clock['default_ticks'] and fault['tick'] % rates[phases[fault['phase']]['rate']]['period_ticks'] == 0, 'fault injection point')
        require(fault['detection_releases'] == 1 and fault['status'] == 'planned' and fault['batch'] in ('M26-02','M26-03','M26-04','M26-05'), 'fault bound/claim')
    levers = unique(data['levers'])
    require(set(levers) == {'workers','grain','rate_multiplier','budget_percent','queue_slots','scratch_bytes','device_depth','cuda_graph','transfer_batch','staging_slots','telemetry_capacity','control_burst','overload_policy'}, 'lever coverage')
    for lever in levers.values():
        require(bool(lever['values']) and len(set(lever['values'])) == len(lever['values']) and all(0 <= v <= 65536 for v in lever['values']), 'lever finite bounds')
    bench = data['benchmarks']; cases = unique(bench['cases'])
    require(bench['provider'] == 'rtfw.golden' and bench['provider_version'] == bench['descriptor_schema'] == 1 and bench['warmup'] == 2 and bench['repetitions'] == 5 and bench['retain_raw'], 'M23 identity/retention')
    expected_cases = {'input','physics','stage','sensor','controller','actuator','aggregate','telemetry','host','memory','rates','controls','replay','external','loop'}
    require(set(cases) == {'golden-'+x for x in expected_cases}, 'subsystem benchmark coverage')
    require(bench['parameter_limit'] == bench['counter_limit'] == 16 and len(levers) <= 16, 'M23 capacity')
    for case in cases.values():
        require(case['batch'] == 'M26-05' and case['status'] == 'planned' and len(case['counters']) == len(set(case['counters'])) <= 16, 'benchmark claim/counters')
    artifacts = unique(data['artifacts']); unique(data['artifacts'],'path')
    require(set(artifacts) == {'run','state','replay','fault','benchmark','coverage'}, 'artifact inventory')
    required_fields = {'run':{'contract_sha256','configuration_sha256','source_commit','source_tree','oracle','cleanup'},'state':{'all_active_fields','checksum'},'replay':{'trusted_payload_digest','gap_count','replay_status'},'fault':{'observed_status','safe_ack','cleanup_status'},'benchmark':{'descriptor','raw_samples','summary','environment','correctness','cleanup'},'coverage':{'capability','source','test','artifact','status','claim_boundary'}}
    for key, artifact in artifacts.items():
        require(artifact['status'] == 'planned' and required_fields[key] <= set(artifact['fields']) and not Path(artifact['path']).is_absolute() and '..' not in Path(artifact['path']).parts, 'artifact claim/shape/path')
    tel = data['telemetry']
    require(all(tel[k] == 1 for k in ('application_schema','rate_schema','mixed_rate_schema','live_control_schema')) and tel['global_schema'] == 2 and tel['capacity_each'] == 16384, 'telemetry schema/capacity')
    require({'gap_count','cleanup_status','state_digest','control_generation','sample_status'} <= set(tel['fields']), 'telemetry observability')
    require(set(variants) == {'cpu','sim_cuda','sim_xdma','sim_combined','external_cil','real_cuda','real_xdma','unreal'}, 'variant inventory')
    for key, variant in variants.items():
        optional = key in ('real_cuda','real_xdma','unreal')
        require(variant['status'] == ('not_run' if optional else 'planned'), 'variant execution overclaim')
        require(set(variant['device_phases']) <= {'physics','sensor','actuator'} and len(set(variant['device_phases'])) == len(variant['device_phases']), 'variant phase mapping')
        require(variant['batch'] in (('M18','M19') if optional else ('M26-02','M26-03','M26-04')), 'variant gate')
    require(data['external_cil']['polls_per_boundary'] == 1 and data['external_cil']['peer_timeout_ms'] == 5000, 'external CIL bound')
    require(set(data['nonclaims']) == {'production_vehicle_model','general_physics_engine','peer_dma','hardware_qualification','rt1','rt2','controlled_performance','unreal_completion','release','deployment','m25_human_acceptance','m26_completion'}, 'claim boundary')
    unique(data['sources'],'path')
    for source in data['sources']:
        path = root / source['path']
        require(not Path(source['path']).is_absolute() and '..' not in Path(source['path']).parts, 'source path escape')
        require(path.is_file(), f'missing source {source["path"]}')
        contents = path.read_text(encoding='utf-8')
        require(bool(source['tokens']) and all(token in contents for token in source['tokens']), f'source link drift: {source["path"]}')
    checksum = digest(data)
    if frozen:
        require(checksum == FROZEN_SHA256, 'frozen contract identity changed; explicit version/hash review required')
    return {'evidence':'offline_design_validation','contract_sha256':checksum,'reference_records':len(records),'default_phase_calls':sum(p['default_calls'] for p in phases.values()),'maximum_position_abs':xmax,'maximum_velocity_abs':vmax,'maximum_nested_tasks':max_tasks,'maximum_channel_age_ticks':ages,'faults':len(faults),'benchmark_cases':len(cases),'scenario_executed':False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--contract', type=Path, default=CONTRACT)
    args = parser.parse_args()
    try:
        result = validate(read(args.contract))
    except (ValueError, OSError) as exc:
        parser.exit(1, f'FAIL: golden contract: {exc}\n')
    print(json.dumps(result, sort_keys=True))
    print('PASS: M26-01 golden design contract; no Runtime scenario execution')


if __name__ == '__main__':
    main()
