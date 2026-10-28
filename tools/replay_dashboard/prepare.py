"""Stream replay logs into disk-backed statistics and indexed 10 ms playback."""
import argparse
import hashlib
import json
import math
import sqlite3
import zlib
from collections import defaultdict
from pathlib import Path

from topology import csv_rows, read_topology
from recording import build_recording
from progress import Progress
from task_store import (create_tables, import_workload, import_tasks, completion_buckets,
                        common_quantiles, task_sources, picoseconds)

CACHE_SCHEMA = 13


def dumps(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'), allow_nan=False)


def optional_rows(path):
    path = Path(path)
    return csv_rows(path) if path.exists() or Path(str(path) + '.gz').exists() else iter(())


def audit_directory(root, case_path, config, metadata, name):
    """Support synthetic nested logs, native sibling logs, and relocated overrides."""
    field = 'te_logs' if name == 'jupiter_te' else 'toe_logs'
    if config.get(field):
        resolved = (root / config[field]).resolve()
        if not resolved.is_dir():
            raise ValueError(f'Audit log directory does not exist: {resolved}')
        return resolved
    candidates = [case_path / name, case_path.parent / name]
    recorded = metadata.get('te_log_directory' if name == 'jupiter_te' else 'toe_log_directory')
    if recorded:
        candidates.extend([(root / recorded).resolve(), Path(recorded).resolve()])
    return next((p for p in candidates if p.is_dir()), candidates[0])


def quantile(values, q=.99, ordered=False):
    values = values if ordered else sorted(values)
    if not values:
        return None
    x = (len(values) - 1) * q
    lo = int(x)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] * (hi - x) + values[hi] * (x - lo) if hi != lo else values[lo]


def fingerprint(manifest):
    manifest = Path(manifest).resolve()
    config = json.loads(manifest.read_text(encoding='utf-8'))
    files = {manifest, Path(__file__), Path(__file__).with_name('topology.py'), Path(__file__).with_name('recording.py'), Path(__file__).with_name('progress.py'), Path(__file__).with_name('task_store.py'), Path(__file__).with_name('queries.py')}
    root = manifest.parent
    for case in config['cases']:
        case_path = (root / case['path']).resolve()
        files.update(p for p in case_path.rglob('*') if p.is_file())
        _, sources = task_sources(root, case)
        files.update(sources)
        metadata = {r['key']: r['value'] for r in csv_rows(case_path / 'replay_run.csv')}
        for name in ('jupiter_te', 'toe'):
            audit = audit_directory(root, case_path, case, metadata, name)
            if audit.is_dir():
                files.update(p for p in audit.rglob('*') if p.is_file())
    for name in ('node.csv', 'topology.csv'):
        path = (root / config['topology'] / name).resolve()
        files.add(path if path.exists() else Path(str(path) + '.gz'))
    if config.get('workload'):
        path = (root / config['workload']).resolve()
        files.add(path if path.exists() else Path(str(path) + '.gz'))
    info = [(str(p), p.stat().st_size, p.stat().st_mtime_ns) for p in sorted(files)]
    return hashlib.sha256(dumps([CACHE_SCHEMA, info]).encode()).hexdigest()


def load_case(db, root, config, topology, reporter):
    csv_rows = reporter.rows
    def optional_rows(path):
        return csv_rows(path) if Path(path).exists() or Path(str(path)+'.gz').exists() else iter(())
    case_id = config['id']
    path = (root / config['path']).resolve()
    node_path_file = path / 'replay_node_path_samples.csv'
    has_node_paths = node_path_file.exists() or Path(str(node_path_file)+'.gz').exists()
    metadata = {r['key']: r['value'] for r in csv_rows(path / 'replay_run.csv')}
    mode = metadata.get('mode', config.get('mode', case_id))
    detail_spec=metadata.get('detail_windows','all')
    def seconds(value):
        import re
        match=re.fullmatch(r'([0-9.eE+-]+)(ps|ns|us|ms|s|min|h)',value.strip())
        if not match: raise ValueError('Invalid detail range in run metadata')
        return float(match[1])*{'ps':1e-12,'ns':1e-9,'us':1e-6,'ms':1e-3,'s':1,'min':60,'h':3600}[match[2]]
    detail_windows=None if detail_spec=='all' else ([] if detail_spec in ('none','') else [[seconds(v) for v in w.split(':')] for w in detail_spec.split(',')])
    has_node_paths=has_node_paths and metadata.get('node_path_enabled','true')!='false'
    step_ms = float(config['window_ms'])
    if abs(float(metadata.get('sample_interval_ns',0))-step_ms*1e6)>1e-3:
        raise ValueError(f'{case_id}: sample interval differs from window_ms={step_ms:g}; '
                         f'recollect with --replay-interval={step_ms:g}ms (old samples cannot be subdivided)')
    te_directory = audit_directory(root, path, config, metadata, 'jupiter_te')
    toe_directory = audit_directory(root, path, config, metadata, 'toe')
    case_files = [path/name for name in ('replay_run.csv','replay_topology.csv','replay_queue_samples.csv',
                  'replay_events.csv','replay_link_samples.csv')]
    case_files += [te_directory/'epoch_summary.csv',te_directory/'weights.csv',toe_directory/'toe_circuits.csv']
    case_files += [node_path_file] if has_node_paths else [path/'replay_path_samples.csv']
    _, sources = task_sources(root, config)
    case_files += sources
    reporter.case(case_id,case_files)
    snapshots = {}
    snapshot_rates = {}
    snapshot_times = {}
    leaves = set(topology['leaves'])
    devices = {n['id'] for n in topology['nodes'] if n['id'] not in leaves}
    access_pairs = {(l['src'],l['dst']) for l in topology['links'] if l['kind']=='access'}
    access_pairs |= {(b,a) for a,b in list(access_pairs)}
    access_sampled = False
    for r in csv_rows(path / 'replay_topology.csv'):
        snapshots.setdefault(int(r['topology_epoch']), {})[(int(r['src_node']), int(r['src_port']))] = (int(r['dst_node']), int(r['dst_port']))
        snapshot_rates.setdefault(int(r['topology_epoch']), {})[(int(r['src_node']), int(r['src_port']))] = int(r['data_rate_bps'])
        snapshot_times[int(r['topology_epoch'])] = float(r['time_ns']) / 1e9
    original = {}
    for link in topology['links']:
        original[(link['src'], link['src_port'])] = (link['dst'], link['dst_port'])
        original[(link['dst'], link['dst_port'])] = (link['src'], link['src_port'])
    for endpoint, peer in snapshots.get(0, {}).items():
        if original.get(endpoint) != peer:
            raise ValueError(f'{case_id}: initial replay topology differs from experiment topology at {endpoint}')
    pair_totals = {}
    for epoch,peers in snapshots.items():
        totals={}
        for endpoint,peer in peers.items():
            pair=(endpoint[0],peer[0]);n,rate=totals.get(pair,(0,0))
            totals[pair]=(n+1,rate+snapshot_rates[epoch][endpoint])
        pair_totals[epoch]=totals
    connections_by_epoch = {epoch:{(a,b) for (a,_),(b,_) in peers.items()} for epoch,peers in snapshots.items()}
    storage_nodes = {n['id'] for n in topology['nodes'] if n['role']=='storage-server'}
    expected_storage_tx = {(a,b) for a,b in access_pairs if a in storage_nodes and b in leaves}
    expected_storage_rx = {(a,b) for a,b in access_pairs if a in leaves and b in storage_nodes}
    # Measure both directions of each storage-facing full-duplex access link.
    # RX is the Leaf-side successful Tx counter, not deduplicated delivery.
    expected_storage = expected_storage_tx | expected_storage_rx
    if not expected_storage_tx or not expected_storage_rx:
        raise ValueError(f'{case_id}: no bidirectional storage-server access links in topology')
    expected_fabric_by_epoch = {epoch:{(a,b) for a,b in pairs if a in leaves and b in leaves}
                                for epoch,pairs in connections_by_epoch.items()}
    # Deduplicate repeated boundary snapshots per physical port before summing.
    # Disk-backed staging keeps queue preprocessing bounded by batch size.
    db.execute('DROP TABLE IF EXISTS queue_stage')
    db.execute('CREATE TABLE queue_stage(t REAL,top INTEGER,te INTEGER,phase TEXT,src INTEGER,port INTEGER,dst INTEGER,q INTEGER,PRIMARY KEY(t,top,te,phase,src,port))')
    batch = []
    for r in optional_rows(path / 'replay_queue_samples.csv'):
        batch.append((float(r['time_ns']), int(r['topology_epoch']), int(r['te_epoch']), r['toe_phase'], int(r['src_node']), int(r['src_port']), int(r['dst_node']), int(r['total_bytes'])))
        if len(batch) >= 2000:
            db.executemany('INSERT OR REPLACE INTO queue_stage VALUES(?,?,?,?,?,?,?,?)', batch); batch.clear()
    if batch:
        db.executemany('INSERT OR REPLACE INTO queue_stage VALUES(?,?,?,?,?,?,?,?)', batch)
    events = []
    for r in optional_rows(path / 'replay_events.csv'):
        events.append(dict(id=int(r['event_id']), time=float(r['time_ns']) / 1e9, event=r['event'], topologyEpoch=int(r['topology_epoch']), teEpoch=int(r['te_epoch']), toeEpoch=int(r['toe_decision_epoch']), phase=r['toe_phase']))
    task_info = import_tasks(db, root, config, reporter)
    epochs = {int(r['epoch']): float(r['sim_time_seconds']) for r in optional_rows(te_directory / 'epoch_summary.csv')}
    weights = [dict(time=epochs.get(int(r['epoch'])), epoch=int(r['epoch']), src=int(r['src_leaf']), dst=int(r['dst_leaf']), transit=int(r['transit_leaf']), weight=float(r['weight'])) for r in optional_rows(te_directory / 'weights.csv')]
    plans = [dict(epoch=int(r['epoch']), time=float(r['time_seconds']), status=r['status'], src=int(r['src_leaf']), srcPort=int(r['src_port']), dst=int(r['dst_leaf']), dstPort=int(r['dst_port'])) for r in optional_rows(toe_directory / 'toe_circuits.csv') if r['status'] in ('remove', 'add')]
    path_samples = defaultdict(list)
    for r in (() if has_node_paths else optional_rows(path / 'replay_path_samples.csv')):
        path_samples[(float(r['start_ns']) / 1e9, float(r['end_ns']) / 1e9)].append(dict(src=int(r['src_leaf']), dst=int(r['dst_leaf']), transit=int(r['transit_leaf']), payloadBytes=int(r['payload_bytes'])))
    # Stage endpoint-attributed paths on disk, keyed to exact logger windows.
    db.execute('CREATE TABLE node_path_stage(sample INTEGER,t0 REAL,t1 REAL,top INTEGER,te INTEGER,toe INTEGER,sn INTEGER,dn INTEGER,src INTEGER,dst INTEGER,mid INTEGER,bytes INTEGER,used INTEGER DEFAULT 0)')
    batch = []
    for r in optional_rows(path / 'replay_node_path_samples.csv'):
        if r['case_id'] != case_id:
            raise ValueError(f'{case_id}: node path case_id mismatch')
        sn,dn,src,dst,mid = (int(r[k]) for k in ('src_node','dst_node','src_leaf','dst_leaf','transit_leaf'))
        count = int(r['tx_payload_bytes']); top = int(r['topology_epoch'])
        t0,t1 = float(r['start_ns'])/1e9,float(r['end_ns'])/1e9
        if (not math.isfinite(t0) or not math.isfinite(t1) or t0 < 0 or t1 <= t0 or count < 0
                or sn not in devices or dn not in devices or src not in leaves or dst not in leaves
                or src == dst or (sn,src) not in access_pairs or (dst,dn) not in access_pairs
                or (mid != -1 and (mid not in leaves or mid in (src,dst)))):
            raise ValueError(f'{case_id}: invalid node path {r}')
        hops = [(src,dst)] if mid == -1 else [(src,mid),(mid,dst)]
        connections = connections_by_epoch.get(top,set())
        if any(hop not in connections for hop in hops):
            raise ValueError(f'{case_id}: node path uses an absent connection in topology epoch {top}')
        batch.append((int(r['sample_id']),t0,t1,top,int(r['te_epoch']),int(r['toe_decision_epoch']),sn,dn,src,dst,mid,count))
        if len(batch)>=2000:
            db.executemany('INSERT INTO node_path_stage(sample,t0,t1,top,te,toe,sn,dn,src,dst,mid,bytes) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',batch);batch.clear()
    if batch:
        db.executemany('INSERT INTO node_path_stage(sample,t0,t1,top,te,toe,sn,dn,src,dst,mid,bytes) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',batch)
    db.execute('CREATE INDEX node_path_window ON node_path_stage(sample,t0,t1,top,te,toe)')
    sample = None
    edges = {}
    seen_ports = set()
    num_frames = 0
    pair_scores = {}
    max_rate = 0.0
    sampled_routes = {}
    threshold = float(config.get('hotspot_threshold', .9))
    def save_frame():
        nonlocal num_frames, max_rate
        if sample is None:
            return
        duration = sample['end'] - sample['start']
        if duration <= 0 or duration > step_ms/1000+1e-9:
            raise ValueError(f'{case_id}: invalid link sample duration for window_ms={step_ms:g}')
        rows = db.execute('SELECT src,dst,SUM(q) FROM queue_stage WHERE t=? AND top=? AND te=? AND phase=? GROUP BY src,dst',
                          (sample['endNs'], sample['topologyEpoch'], sample['teEpoch'], sample['phase'])).fetchall()
        queues = {(a, b): q / 1e6 for a, b, q in rows}
        fabric_bits = fabric_capacity = storage_bits = storage_capacity = 0
        storage_tx_bits = storage_rx_bits = 0
        # Missing either direction of any storage access port is unknown, not zero.
        expected_counts = pair_totals.get(sample['topologyEpoch'], {})
        storage_complete = bool(expected_storage) and all(
            k in edges and k in expected_counts and
            edges[k]['circuits'] == expected_counts[k][0] for k in expected_storage)
        expected_fabric = expected_fabric_by_epoch.get(sample['topologyEpoch'],set())
        fabric_complete = bool(expected_fabric) and expected_fabric <= edges.keys()
        for k, edge in edges.items():
            wire_bits = edge.pop('wireBytes') * 8
            bits = edge.pop('capacityBits')
            if edge['src'] in leaves and edge['dst'] in leaves:
                fabric_bits += wire_bits
                fabric_capacity += bits
            elif k in expected_storage:
                storage_bits += wire_bits
                storage_capacity += bits
                if k in expected_storage_tx:
                    storage_tx_bits += wire_bits
                else:
                    storage_rx_bits += wire_bits
            # Preserve precision before display scaling (e.g. Mbps -> Gbps).
            edge['gbps'] = wire_bits / duration / 1e9
            edge['capacity'] = bits / duration / 1e9
            edge['queueMB'] = queues.get(k)
            edge['utilization'] = edge['gbps'] / edge['capacity'] if edge['capacity'] else None
        frame = {k: v for k, v in sample.items() if k != 'endNs'}
        frame['edges'] = list(edges.values())
        max_rate=max(max_rate,max((e['gbps'] for e in frame['edges']),default=0))
        fabric = [e for e in frame['edges'] if e['src'] in leaves and e['dst'] in leaves]
        measured = [e for e in fabric if e['utilization'] is not None]
        frame['global'] = dict(maxUtilization=max((e['utilization'] for e in measured),default=None),
                               fabricUtilization=fabric_bits/fabric_capacity if fabric_complete and fabric_capacity else None,
                               storageThroughputGbps=storage_bits/duration/1e9 if storage_complete else None,
                               storageTxGbps=storage_tx_bits/duration/1e9 if storage_complete else None,
                               storageRxGbps=storage_rx_bits/duration/1e9 if storage_complete else None,
                               storageSampleComplete=storage_complete,
                               fabricComplete=fabric_complete,
                               hotLinks=sum(e['utilization'] >= threshold for e in measured) if measured else None,
                               measuredLinks=len(measured),
                               queueMB=sum(e['queueMB'] for e in fabric if e['queueMB'] is not None) if any(e['queueMB'] is not None for e in fabric) else None)
        for edge in fabric:
            score = pair_scores.setdefault((edge['src'],edge['dst']),dict(src=edge['src'],dst=edge['dst'],wireBytes=0,maxUtilization=0))
            score['wireBytes'] += edge['gbps']*1e9/8*duration
            score['maxUtilization'] = max(score['maxUtilization'],edge['utilization'] or 0)
        frame['pathSamples'] = path_samples.get((sample['start'], sample['end']), [])
        window = (sample['id'],sample['start'],sample['end'],sample['topologyEpoch'],sample['teEpoch'],sample['toeEpoch'])
        # Keep endpoint detail on disk; a browser frame contains only its selected Leaf pair.
        db.execute('INSERT INTO node_paths SELECT ?,sample,t0,t1,sn,dn,src,dst,mid,SUM(bytes) FROM node_path_stage WHERE sample=? AND t0=? AND t1=? AND top=? AND te=? AND toe=? GROUP BY sn,dn,src,dst,mid',(case_id,*window))
        for a,b,m in db.execute('SELECT DISTINCT src,dst,mid FROM node_path_stage WHERE sample=? AND t0=? AND t1=? AND top=? AND te=? AND toe=? AND bytes>0',window):
            sampled_routes.setdefault((a,b,m),[sample['start'],sample['end']])
        frame['nodePaths'] = []
        db.execute('UPDATE node_path_stage SET used=1 WHERE sample=? AND t0=? AND t1=? AND top=? AND te=? AND toe=?',window)
        db.execute('INSERT INTO global_samples VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                   (case_id,sample['start'],sample['end'],frame['global']['maxUtilization'],
                    frame['global']['hotLinks'],len(measured),
                    storage_bits if storage_complete else None,
                    duration if storage_complete else 0,
                    storage_capacity if storage_complete else None,
                    fabric_bits if fabric_complete else None,fabric_capacity if fabric_complete else None,
                    frame['global']['fabricUtilization'],
                    storage_tx_bits if storage_complete else None,
                    storage_rx_bits if storage_complete else None))
        db.executemany('INSERT INTO link_trends VALUES(?,?,?,?,?,?,?,?)',[(case_id,sample['start'],e['src'],e['dst'],e['capacity'],e['gbps'],e['utilization'],e['queueMB']) for e in fabric])
        blob = zlib.compress(dumps(frame).encode(), 3)
        db.execute('INSERT INTO frames VALUES(?,?,?,?)', (case_id, sample['start'], sample['end'], blob))
        num_frames += 1
        reporter.update(samples=num_frames,simulationTime=sample['end'])
    for r in csv_rows(path / 'replay_link_samples.csv'):
        sample_id = int(r['sample_id'])
        if sample is None or sample_id != sample['id']:
            save_frame()
            if sample and float(r['start_ns']) / 1e9 < sample['end'] - 1e-10:
                raise ValueError(f'{case_id}: overlapping link sample windows')
            sample = dict(id=sample_id, start=float(r['start_ns']) / 1e9, end=float(r['end_ns']) / 1e9,
                          endNs=float(r['end_ns']), topologyEpoch=int(r['topology_epoch']), teEpoch=int(r['te_epoch']), toeEpoch=int(r['toe_decision_epoch']), phase=r['toe_phase'])
            edges = {}; seen_ports = set()
        if (float(r['start_ns'])/1e9 != sample['start'] or float(r['end_ns'])/1e9 != sample['end']
                or int(r['topology_epoch']) != sample['topologyEpoch']
                or int(r['te_epoch']) != sample['teEpoch']
                or int(r['toe_decision_epoch']) != sample['toeEpoch'] or r['toe_phase'] != sample['phase']):
            raise ValueError(f'{case_id}: mixed windows/epochs within link sample {sample_id}')
        grouped = r.get('aggregation') == 'node_pair'
        src, port, dst, dp = [int(r[k]) for k in ('src_node', 'src_port', 'dst_node', 'dst_port')]
        circuit_count = int(r['circuit_count']) if grouped else 1
        access_sampled = access_sampled or src not in leaves or dst not in leaves
        identity = (src,dst) if grouped else (src,port)
        if identity in seen_ports:
            raise ValueError(f'{case_id}: repeated port in sample {sample_id}')
        seen_ports.add(identity)
        if grouped:
            expected=pair_totals.get(int(r['topology_epoch']),{}).get((src,dst))
            if expected!=(circuit_count,int(r['data_rate_bps'])):
                raise ValueError(f'{case_id}: aggregated link disagrees with topology {src}->{dst}')
        elif snapshots.get(int(r['topology_epoch']), {}).get((src, port)) != (dst, dp):
            raise ValueError(f'{case_id}: sample endpoint disagrees with applied topology: {src}:{port}->{dst}:{dp}')
        key = (src, dst)
        edge = edges.setdefault(key, dict(src=src, dst=dst, circuits=0, wireBytes=0,
                                          capacityBits=0))
        wire_bytes = int(r['tx_wire_bytes'])
        if wire_bytes < 0:
            raise ValueError(f'{case_id}: negative tx_wire_bytes in sample {sample_id}')
        edge['circuits'] += circuit_count
        edge['wireBytes'] += wire_bytes
        edge['capacityBits'] += float(r['capacity_bits'])

    save_frame()
    if db.execute('SELECT 1 FROM node_path_stage WHERE used=0 LIMIT 1').fetchone():
        raise ValueError(f'{case_id}: node path windows/epochs do not match link samples')
    db.execute('DROP TABLE node_path_stage')
    db.execute('DROP TABLE queue_stage')
    if not num_frames:
        raise ValueError(f'{case_id}: no link samples')
    bounds = db.execute('SELECT MIN(t0),MAX(t1) FROM frames WHERE case_id=?', (case_id,)).fetchone()
    total_windows, complete_windows, total_storage_bits, total_storage_capacity, total_storage_duration = db.execute(
        'SELECT COUNT(*),COUNT(storage_bits),SUM(storage_bits),SUM(storage_capacity),SUM(storage_duration) '
        'FROM global_samples WHERE case_id=?', (case_id,)).fetchone()
    storage_utilization = (
        total_storage_bits / total_storage_capacity
        if complete_windows == total_windows and total_storage_capacity and total_storage_capacity > 0
        else None
    )
    # Aggregate both storage ingress and egress over the full simulated duration.
    storage_throughput_gbps = (
        total_storage_bits / total_storage_duration / 1e9
        if complete_windows == total_windows and total_storage_duration and total_storage_duration > 0
        else None
    )
    route_evidence = []
    path_file = path / 'replay_path_samples.csv'
    final_epoch = max(snapshots)
    final_edges = {}
    for (src, port), (dst, peer_port) in snapshots[final_epoch].items():
        edge = final_edges.setdefault((src, dst), dict(src=src, dst=dst, circuits=0, capacity=0, gbps=None, queueMB=None, utilization=None))
        edge['circuits'] += 1
        edge['capacity'] += snapshot_rates[final_epoch][(src, port)] / 1e9
    topology_states = []
    for epoch in sorted(snapshots):
        bundles = {}
        for (src, port), (dst, peer_port) in snapshots[epoch].items():
            edge = bundles.setdefault((src,dst),dict(src=src,dst=dst,circuits=0,capacity=0,gbps=None,queueMB=None,utilization=None))
            edge['circuits'] += 1
            edge['capacity'] += snapshot_rates[epoch][(src,port)]/1e9
        topology_states.append(dict(epoch=epoch,time=snapshot_times[epoch],edges=list(bundles.values())))
    info = dict(id=case_id, mode=mode, label=config.get('label', case_id.upper()), metadata=metadata,
                synthetic=metadata.get('synthetic', '').lower() == 'true', events=events, weights=weights, plans=plans,
                routeEvidence=route_evidence, hasPathSamples=path_file.exists() or Path(str(path_file) + '.gz').exists(),
                capabilities=dict(accessSamples=access_sampled,nodePaths=has_node_paths),
                detailWindows=detail_windows,maxRateGbps=max_rate,
                storageUtilization=storage_utilization,
                storageThroughputGbps=storage_throughput_gbps,
                storageSampleComplete=(complete_windows == total_windows),
                **task_info, frameCount=num_frames,
                topologyStates=topology_states, pairScores=list(pair_scores.values()),
                finalTopologyEpoch=final_epoch, finalEdges=list(final_edges.values()),
                start=bounds[0], end=bounds[1])
    if has_node_paths:
        info['sampledRouteIntervals'] = [dict(src=a,dst=b,transit=m,start=t0,end=t1) for (a,b,m),(t0,t1) in sampled_routes.items()]
    return info


def prepare(manifest_path, cache_path, force=False, progress=None):
    reporter = Progress(progress)
    reporter.stage('checking','检查输入与缓存')
    manifest_path, cache_path = Path(manifest_path).resolve(), Path(cache_path).resolve()
    token = fingerprint(manifest_path)
    if cache_path.exists() and not force:
        with sqlite3.connect(cache_path) as old:
            try:
                row = old.execute("SELECT value FROM metadata WHERE key='fingerprint'").fetchone()
                if row and row[0] == token:
                    reporter.stage('cached','命中缓存，读取已处理数据')
                    return cache_path
            except sqlite3.DatabaseError:
                pass
    config = json.loads(manifest_path.read_text(encoding='utf-8'))
    step_ms = float(config.get('window_ms',10))
    if not math.isfinite(step_ms) or step_ms <= 0 or picoseconds(step_ms,10**9) <= 0:
        raise ValueError('window_ms must be a positive finite sampling interval')
    if not config.get('cases') or len({c['id'] for c in config['cases']}) != len(config['cases']):
        raise ValueError('Experiment must contain cases with nonempty, unique ids')
    root = manifest_path.parent
    reporter.stage('topology','读取拓扑和工作负载')
    topology = read_topology(root / config['topology'],rows=reporter.rows)
    threshold = float(config.get('hotspot_threshold', .9))
    if not math.isfinite(threshold) or not 0 < threshold <= 1:
        raise ValueError('hotspot_threshold must be finite and in (0, 1]')
    if len(topology['leaves']) < 2:
        raise ValueError('Replay requires at least two switches in the input topology')
    if config.get('focus'):
        focus = config['focus']
        if (focus.get('src') not in topology['leaves'] or focus.get('dst') not in topology['leaves']
                or focus['src'] == focus['dst']):
            raise ValueError('focus must identify two different switches in the input topology')
    if not config.get('workload'):
        raise ValueError('workload must point to the actual traffic.csv for task totals and identity')
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = cache_path.with_suffix('.building.sqlite')
    if temporary.exists(): temporary.unlink()
    cases = []
    with sqlite3.connect(temporary) as db:
        db.executescript("PRAGMA journal_mode=DELETE; PRAGMA synchronous=NORMAL; PRAGMA temp_store=FILE; PRAGMA cache_size=-65536; PRAGMA mmap_size=0; CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT); CREATE TABLE frames(case_id TEXT,t0 REAL,t1 REAL,data BLOB,PRIMARY KEY(case_id,t0)); CREATE INDEX frames_range ON frames(case_id,t1);")
        db.executescript('CREATE TABLE global_samples(case_id TEXT,t0 REAL,t1 REAL,max_util REAL,hot INTEGER,measured INTEGER,storage_bits INTEGER,storage_duration REAL,storage_capacity REAL,fabric_bits INTEGER,fabric_capacity REAL,fabric_util REAL,storage_tx_bits INTEGER,storage_rx_bits INTEGER,PRIMARY KEY(case_id,t0)); CREATE TABLE link_trends(case_id TEXT,t0 REAL,src INTEGER,dst INTEGER,capacity REAL,gbps REAL,util REAL,queue REAL,PRIMARY KEY(case_id,src,dst,t0)); CREATE TABLE node_paths(case_id TEXT,sample INTEGER,t0 REAL,t1 REAL,sn INTEGER,dn INTEGER,src INTEGER,dst INTEGER,mid INTEGER,bytes INTEGER);')
        db.set_progress_handler(lambda: (reporter.update(),0)[1],200000)
        create_tables(db)
        reporter.stage('workload','分批导入 workload 到磁盘')
        import_workload(db,root/config['workload'],reporter)
        for case in config['cases']:
            print(f"Preparing {case['id']} (streaming CSV → SQLite)", flush=True)
            reporter.stage('import','导入日志、聚合链路与节点路径',case['id'])
            info = load_case(db, root, {**case,'window_ms':step_ms,'hotspot_threshold':threshold}, topology,reporter)
            cases.append(info)
            db.commit()
        reporter.stage('statistics','计算共同任务集与 FCT 分位数')
        workload_fingerprints = {c['metadata']['traffic_fnv1a64'] for c in cases if c['metadata'].get('traffic_fnv1a64') not in (None, '', 'unavailable')}
        if len(workload_fingerprints) > 1:
            raise ValueError('Input workload fingerprints differ across cases')
        common_count, baseline = common_quantiles(db,cases,config,reporter)
        for case in cases:
            completion_buckets(db,case['id'],picoseconds(step_ms,10**9),case['end'],reporter,retain=False)
        recording = build_recording(config,cases,baseline,topology['leaves'])
        selected = None
        reporter.stage('indexing','建立路径查询索引')
        db.execute('CREATE INDEX node_paths_focus ON node_paths(case_id,src,dst,t0,t1)')
        default_focus = config.get('focus',recording['chapters'][0]['focus'])
        for case in cases:
            # Browser receives coalesced route evidence, not a potentially huge
            # list of every task/flow decision or every candidate task id.
            case.pop('routeEvidence',None)
            case.pop('sampledRouteIntervals',None)
            case.pop('pairScores',None)
            summaries = {}
            for weight in case.pop('weights'):
                k=(weight['epoch'],weight['src'],weight['dst'])
                summary=summaries.setdefault(k,dict(epoch=k[0],src=k[1],dst=k[2],direct=0.0,transit=0.0))
                summary['direct' if weight['transit']<0 else 'transit']+=weight['weight']
            case['policySummary']=list(summaries.values())
            case.pop('plans',None)
        bootstrap = dict(title=config.get('title', 'TE / TOE 回放'), description=config.get('description', ''),
                         synthetic=bool(config.get('synthetic', False)) or any(c['synthetic'] for c in cases),
                         topologySource=config.get('topologySource', 'experiment node.csv / topology.csv'),
                         topology=topology, cases=cases, commonTasks=common_count, windowSeconds=step_ms/1000,
                         blockSeconds=max(step_ms/1000,min(.5,50*step_ms/1000)),
                         start=min(c['start'] for c in cases), end=max(c['end'] for c in cases),
                         focus=default_focus,
                         focusOptions=[dict(src=a,dst=b) for a in topology['leaves'] for b in topology['leaves'] if a!=b],
                         selectedTask=selected, recording=recording, statistics=config.get('statistics',{}),
                         rateScaleGbps=max((c['maxRateGbps'] for c in cases),default=1))
        reporter.stage('indexing','提交统计与索引，准备回放')
        for k, value in [('fingerprint', token), ('bootstrap', dumps(bootstrap)), ('manifest', str(manifest_path))]:
            db.execute('INSERT INTO metadata VALUES(?,?)', (k, value))
    temporary.replace(cache_path)
    return cache_path


def decode_frame(blob):
    return json.loads(zlib.decompress(blob))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--cache', type=Path, default=Path(__file__).parent / 'cache/replay.sqlite')
    parser.add_argument('--force', action='store_true')
    args = parser.parse_args()
    try:
        print(prepare(args.manifest, args.cache, args.force))
    except Exception as error:
        parser.exit(2, f'ERROR: {error}\n')
