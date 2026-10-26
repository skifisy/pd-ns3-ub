"""Read actual node.csv/topology.csv; never generate replacement topologies."""
import csv
import gzip
import hashlib
from pathlib import Path


def csv_rows(path):
    path = Path(path)
    if not path.exists() and Path(str(path) + ".gz").exists():
        path = Path(str(path) + ".gz")
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8-sig", newline="") as stream:
        yield from csv.DictReader(stream)


def read_topology(directory, rows=csv_rows):
    directory = Path(directory)
    nodes = []
    for row in rows(directory / 'node.csv'):
        identifier = row['nodeId']
        lo, hi = map(int, identifier.split('..')) if '..' in identifier else (int(identifier), int(identifier))
        nodes.extend({'id': n, 'type': row['nodeType'], 'ports': int(row['portNum'])}
                     for n in range(lo, hi + 1))
    switches = sorted(n['id'] for n in nodes if n['type'].upper() == 'SWITCH')
    switch_set = set(switches)
    for node in nodes:
        node['group'] = switches.index(node['id']) // 4 if node['id'] in switch_set else None
        node['role'] = ('storage-leaf' if node['id'] in switches[-4:] else 'compute-leaf') if node['id'] in switch_set else ('compute-server' if node['ports'] == 64 else 'storage-server')
    links = []
    used = set()
    for row in rows(directory / 'topology.csv'):
        a, ap, b, bp = [int(row[k]) for k in ['nodeId1', 'portId1', 'nodeId2', 'portId2']]
        for endpoint in [(a, ap), (b, bp)]:
            if endpoint in used:
                raise ValueError(f'Duplicate physical endpoint: {endpoint}')
            used.add(endpoint)
        rate = row['bandwidth'].strip()
        multiplier = 1e9 if rate.endswith('Gbps') else 1e6 if rate.endswith('Mbps') else 1
        number = rate.removesuffix('Gbps').removesuffix('Mbps').removesuffix('bps')
        links.append({'src': a, 'src_port': ap, 'dst': b, 'dst_port': bp,
                      'bps': int(float(number) * multiplier),
                      'kind': 'fabric' if a in switch_set and b in switch_set else 'access'})
    known = {n['id']: n for n in nodes}
    for node, port in used:
        if node not in known or not 0 <= port < known[node]['ports']:
            raise ValueError(f'Invalid node/port: {node}:{port}')
    raw = b''
    for name in ('node.csv', 'topology.csv'):
        path = directory / name
        raw += path.read_bytes() if path.exists() else Path(str(path) + '.gz').read_bytes()
    return {'nodes': nodes, 'links': links, 'leaves': switches,
            'sha256': hashlib.sha256(raw).hexdigest(),
            'physicalLinks': len(links), 'fabricCircuits': sum(l['kind'] == 'fabric' for l in links)}

