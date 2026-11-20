"""Small deterministic synthetic Replay logs; never simulation results."""
import csv
from pathlib import Path


def write_csv(path, columns, records):
    with Path(path).open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns.split())
        writer.writeheader()
        writer.writerows(records)


def make_case(root, physical=False, full=False, count=128):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    p = list(range(37, 49)) if full else [37, 38]
    d = list(range(49, 53)) if full else [49]
    s = list(range(53, 57)) if full else [53, 54]
    write_csv(root / "node.csv", "nodeId nodeType portNum",
              [dict(nodeId=n, nodeType="SWITCH", portNum=128) for n in p + d + s]
              + [dict(nodeId=n, nodeType="SERVER", portNum=ports) for n, ports in [(0,64),(12,64),(16,12)]])
    write_csv(root / "topology.csv", "nodeId1 nodeId2",
              [dict(nodeId1=0,nodeId2=n) for n in p]
              + [dict(nodeId1=12,nodeId2=n) for n in d]
              + [dict(nodeId1=16,nodeId2=n) for n in s])
    # One event-split 1 ms window followed by 9 ms; all other windows 10 ms.
    windows, time = [], 0
    for i in range(count):
        dt = 1000000 if i == 65 else 9000000 if i == 66 else 10000000
        windows.append((time, time + dt))
        time += dt
    epoch_time = windows[64][0] if count > 64 else 0
    epochs = [0, 1] if count > 64 else [0]
    topology = []
    port = {n: 0 for n in p + d + s}
    circuits = []
    for a in p + d:
        for b in s:
            if a == 38 and b == 54:
                continue  # A valid pair without a direct circuit.
            for lane in range(2):
                ap, bp = port[a], port[b]
                port[a] += 1
                port[b] += 1
                circuits.extend([(a,ap,b,bp),(b,bp,a,ap)])
    for epoch in epochs:
        bps = 1000000 if epoch == 0 else 500000
        for a, ap, b, bp in circuits:
            topology.append(dict(time_ns=0 if epoch == 0 else epoch_time, topology_epoch=epoch,
                                 src_node=a,src_port=ap,dst_node=b,dst_port=bp,data_rate_bps=bps))
    write_csv(root / "replay_topology.csv", "time_ns topology_epoch src_node src_port dst_node dst_port data_rate_bps", topology)
    meta = dict(schema_version=2,case_id="ecmp",mode="ecmp",synthetic="true",sample_interval_ns=10000000,
                node_path_enabled="true",detail_windows="all",start_ns=0,end_ns=time)
    write_csv(root / "replay_run.csv", "key value", [dict(key=k,value=v) for k,v in meta.items()])
    links, paths = [], []
    for i,(t0,t1) in enumerate(windows):
        epoch = 1 if i >= 64 else 0
        bps = 1000000 if epoch == 0 else 500000
        base = dict(sample_id=i,case_id="ecmp",start_ns=t0,end_ns=t1,topology_epoch=epoch)
        totals = {}
        for a,ap,b,bp in circuits:
            wire = 250 if (a,b)==(37,53) and i==65 else 0
            if physical:
                links.append(dict(base,src_node=a,src_port=ap,dst_node=b,dst_port=bp,
                                  data_rate_bps=bps,capacity_bits=bps*(t1-t0)/1e9,
                                  tx_wire_bytes=wire,aggregation="physical_port"))
            else:
                rate,byte_count = totals.get((a,b),(0,0))
                totals[a,b] = (rate+bps,byte_count+wire)
        for (a,b),(total_bps,wire) in totals.items():
            links.append(dict(base,src_node=a,src_port=-1,dst_node=b,dst_port=-1,
                              data_rate_bps=total_bps,capacity_bits=total_bps*(t1-t0)/1e9,
                              tx_wire_bytes=wire,aggregation="node_pair"))
        if i == 65:
            # Two rows in the same OD/window must sum BEFORE calculating a peak.
            paths.extend([dict(base,src_leaf=37,dst_leaf=53,tx_payload_bytes=value) for value in (100,200)])
    write_csv(root / "replay_link_samples.csv",
              "sample_id case_id start_ns end_ns topology_epoch src_node src_port dst_node dst_port data_rate_bps capacity_bits tx_wire_bytes aggregation",links)
    write_csv(root / "replay_node_path_samples.csv",
              "sample_id case_id start_ns end_ns topology_epoch src_leaf dst_leaf tx_payload_bytes",paths)
    return windows
