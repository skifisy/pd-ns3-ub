#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Prefill Batch Read/Compute/Write pipeline comparison for the current pd-ns3-ub V15 workflow.

Compatible with the repository main revision 7a41f3456c0f65c8831c68f070cfbb7aa57cd682
(2026-11-04): mooncake_trace_to_dag_v15.py + mooncake_dag_to_traffic_v15.py.
The analyzer consumes the current logical V15 DAG, current converter mapping.csv,
and ns-3 output/task_statistics.csv. It does not modify or run the simulator.

Based on the current repository versions of mooncake_trace_to_dag_v15.py and
mooncake_dag_to_traffic_v15.py. Current V15 mapping.csv contains taskId,
dag_task_id, request_id, task_type, layer_index and the V14 metadata columns
(including batch_id) appended to each physical task mapping row.
Read/write = ns-3 per-flow observed time, compute = inferred from v15 DAG layer
compute durations and observed completed per-layer reads. Forecast compute time
from batches_debug.csv is the initial anchor, NOT an observed GPU event.

Examples:
 python3 analyze_prefill_pipeline_v4.py --case DAG_OUTPUT_DIR \
  --ecmp-dir ECMP --te-dir TE --toe-dir TOE --eps2-dir EPS2 \
  --mapping DAG_OUTPUT_DIR/mapping.csv --out-dir pipeline_analysis

If mapping is unavailable, DAG->TP member->uint32 chunk reconstruction is used.
Physical TIMER rows (including converter-generated Decode wait timers) retain an empty
physical taskId slot; network rows are strictly verified (provide --data-size-divisor).

Outputs: pipeline_report.html (self-contained interactive viewer),
 pipeline_batch_summary.csv, pipeline_layer_detail.csv, pipeline_request_detail.csv.

Only selected Prefill batches are retained in RAM. Default: first 6 batches.
Select explicit batches with repeated --batch-id; more can be chosen on re-run.
The selected batches' detailed Requests are limited by --detail-requests, but
batch/layer totals contain every Request in the batch.
No simulation is performed, only existing traces are analyzed.
"""
import argparse
from array import array
from collections import Counter, defaultdict
import csv
import json
import math
import mmap
import os
from pathlib import Path
import struct
import sys
import tempfile

VERSION = 'v15-prefill-batch-pipeline-v5-flush-index-tail-20261105'
UPSTREAM_REVISION = '7a41f3456c0f65c8831c68f070cfbb7aa57cd682'
# Limit HTML chart primitives per stage. Exact overlap statistics still use
# the full global network-activity union, including stages beyond this cap.
MAX_STAGE_BUSY_INTERVALS_HTML = 400
U32_MAX = 2**32 - 1
SCHEMA = 'v14-domain-serial-batch-tp-logical'
STAGES = ('PREFIX_READ', 'PREFIX_STORE_WRITE')
SCHEMES = ('ecmp', 'te', 'toe', 'eps2')
LABELS = {'ecmp':'ECMP','te':'TE','toe':'TOE','eps2':'EPS-2L'}


def rows(path, required):
    with Path(path).open('r', encoding='utf-8-sig', newline='', buffering=1024*1024) as f:
        reader = csv.reader(f)
        try:
            header = [s.strip() for s in next(reader)]
        except StopIteration:
            raise ValueError(f'empty CSV: {path}') from None
        for col in required:
            if header.count(col) != 1:
                raise ValueError(f'{path}: missing or duplicated field {col}')
        idx = [header.index(c) for c in required]
        for line in reader:
            if not line:
                continue
            if len(line) != len(header):
                raise ValueError(f'{path}, line {reader.line_num}: invalid field count')
            yield tuple(line[i].strip() for i in idx)


def number(raw, desc):
    try:
        x = int(raw)
    except (ValueError, TypeError):
        raise ValueError(f'{desc}: invalid integer {raw!r}') from None
    if x < 0:
        raise ValueError(f'{desc}: negative integer {x}')
    return x


def csv_header(path):
    with Path(path).open('r', encoding='utf-8-sig', newline='') as f:
        try:
            return [value.strip() for value in next(csv.reader(f))]
        except StopIteration:
            raise ValueError(f'empty CSV: {path}') from None


def select_batches(path, ids, nmax):
    out = {}
    fields = ('batch_id','batch_type','request_ids','service_domain','compute_duration_us',
              'forecast_compute_start_us','batch_size','tp_degree')
    wanted = set(ids)
    for raw_id,typ,member,domain,compute,forecast,size,degree in rows(path,fields):
        if typ != 'PREFILL':
            continue
        bid = number(raw_id, 'batch_id')
        if ids and bid not in wanted:
            continue
        if bid in out:
            raise ValueError(f'duplicate Prefill batch {bid}')
        members = json.loads(member)
        if not isinstance(members,list) or not all(type(x)==int and x>=0 for x in members):
            raise ValueError(f'batch {bid}: invalid request_ids')
        out[bid] = {'batch_id':bid,'requests':members,'service_domain':domain,
                    'compute_duration_us':number(compute,'batch compute duration'),
                    'forecast_compute_start_us':number(forecast,'forecast compute start'),
                    'batch_size':number(size,'batch size'),'tp_degree':number(degree,'TP degree')}
        if not ids and len(out) >= nmax:
            break
    if ids and wanted-set(out):
        raise ValueError(f'unknown Prefill batches: {sorted(wanted-set(out))}')
    if not out:
        raise ValueError('no Prefill batches available')
    return out


def load_requests(path, batches, detail_limit, pinned):
    selected = set(rid for b in batches.values() for rid in b['requests'])
    details = {bid:set(b['requests'][:detail_limit]) | (set(b['requests']) & pinned)
               for bid,b in batches.items()}
    found = {}
    for raw,phase,arrival,output,miss,hit in rows(path,
              ('request_id','phase','timestamp_us','output_length','miss_tokens','cached_tokens')):
        rid = number(raw,'request_id')
        if rid in selected and phase == 'FORMAL':
            if rid in found:
                raise ValueError(f'duplicate request {rid}')
            found[rid] = {'id':rid,'arrival_us':number(arrival,'arrival time'),
                          'output_tokens':number(output,'output tokens'),
                          'miss_tokens':number(miss,'miss tokens'),
                          'cached_tokens':number(hit,'cached tokens')}
    if selected - set(found):
        raise ValueError(f'missing formal request metadata, e.g. {sorted(selected-set(found))[:5]}')
    return found,details


def load_dag_layers(path, batches):
    durations = defaultdict(dict)
    logical_bytes = defaultdict(lambda: [0,0])
    fields = ('task_id','task_class','task_type','batch_id','request_id','layer_index',
              'duration_us','bytes','execution_schema')
    previous = -1
    for tid,kind,typ,raw_batch,raw_req,layer,us,byte,sc in rows(path,fields):
        task_id = number(tid,'DAG task ID')
        if task_id <= previous:
            raise ValueError('DAG task IDs must be strictly increasing')
        previous=task_id
        if sc != SCHEMA:
            raise ValueError(f'expected logical v15 schema {SCHEMA}, got {sc}')
        if not raw_batch or int(raw_batch) not in batches:
            continue
        bid=int(raw_batch)
        if kind=='COMPUTE' and typ=='PREFILL_LAYER':
            lid=number(layer,'layer index')
            if lid in durations[bid]:
                raise ValueError(f'duplicate compute layer in batch={bid}, layer={lid}')
            durations[bid][lid]=number(us,'compute duration')
        elif kind=='NETWORK' and typ in STAGES:
            lid=number(layer,'network layer')
            rid=number(raw_req,'network request ID')
            if rid not in batches[bid]['requests']:
                raise ValueError(f'DAG network request {rid} not in batch {bid}')
            logical_bytes[(bid,lid,typ,rid)][0] += number(byte,'network bytes')
    for bid, layers in durations.items():
        if not layers:
            continue
        if set(layers) != set(range(len(layers))):
            raise ValueError(f'non-contiguous compute layers for batch {bid}')
    for bid in batches:
        if not durations[bid]:
            raise ValueError(f'no PREFILL_LAYER compute durations in DAG for batch {bid}')
    return durations,logical_bytes


class Buckets:
    def __init__(self, batches, detail):
        self.batches = batches
        self.detail = detail
        self.key2code = {}
        self.keys = [None]
        self.expected = [0]
        self.logical_bytes = {}

    def encode(self, bid, lid, stage, rid):
        if bid not in self.batches or stage not in STAGES:
            return 0
        if lid < 0:
            raise ValueError(f'negative Prefill layer index for batch {bid}')
        # All network traffic counts; detailed per-request lanes are limited.
        requester = rid if rid in self.detail[bid] else -1
        key = (bid,lid,stage,requester)
        code=self.key2code.get(key)
        if code is None:
            code=len(self.keys)
            if code>U32_MAX:
                raise ValueError('too many index buckets')
            self.key2code[key]=code
            self.keys.append(key)
            self.expected.append(0)
        self.expected[code]+=1
        return code


def build_index(mapping, dag, traffic, divisor, work, buckets, progress):
    path=work/'flow_to_bucket.bin'
    mapping_mode = bool(mapping)
    buffer=array('I')
    first=None
    count=0

    def emit(tid, bid, lid, typ, rid):
        nonlocal first,count,buffer
        if first is None:
            first=tid
        if tid != first+count:
            raise ValueError('physical task IDs must be contiguous and ascending; cannot use reordered mapping/traffic')
        code=buckets.encode(bid,lid,typ,rid)
        buffer.append(code)
        count+=1
        if len(buffer)>=262144:
            buffer.tofile(f)
            buffer=array('I')
        if progress and count % progress==0:
            print(f'Building flow index: {count:,}',file=sys.stderr,flush=True)

    with path.open('wb',buffering=1024*1024) as f:
        if mapping_mode:
            # Current V15 converter mapping.csv is MAPPING_FIELDS + V14_MAPPING_FIELDS:
            # batch_id is appended near the end of the header, not adjacent to taskId.
            # rows() resolves by header name, so the current format is read directly.
            header=set(csv_header(mapping))
            direct_fields={'taskId','batch_id','layer_index','task_type','request_id'}
            if direct_fields.issubset(header):
                for task, batch, layer, typ, req in rows(
                        mapping,('taskId','batch_id','layer_index','task_type','request_id')):
                    if batch and typ in STAGES and int(batch) in buckets.batches:
                        emit(number(task,'taskId'), int(batch),number(layer,'layer index'),typ,number(req,'request_id'))
                    else:
                        emit(number(task,'taskId'),-1,-1,'',-1)
            elif {'taskId','dag_task_id'}.issubset(header):
                # Compatibility with mapping variants that carry only the logical
                # DAG task ID. Join only selected Prefill network rows to keep RAM bounded.
                dag_meta={}
                for tid,kind,typ,batch,req,layer in rows(
                        dag,('task_id','task_class','task_type','batch_id','request_id','layer_index')):
                    if kind=='NETWORK' and typ in STAGES and batch and int(batch) in buckets.batches:
                        dag_meta[number(tid,'DAG task_id')]=(int(batch),number(layer,'DAG layer_index'),
                                                             typ,number(req,'DAG request_id'))
                for task, logical in rows(mapping,('taskId','dag_task_id')):
                    meta=dag_meta.get(number(logical,'dag_task_id')) if logical else None
                    if meta is None:
                        emit(number(task,'taskId'),-1,-1,'',-1)
                    else:
                        emit(number(task,'taskId'),*meta)
            else:
                raise ValueError(
                    f'{mapping}: unsupported mapping.csv header; expected current V15 fields '
                    'taskId,batch_id,layer_index,task_type,request_id or taskId,dag_task_id'
                )
        else:
            # Reconstruct network-flow alignment without assuming that every
            # physical TIMER has a one-to-one row in task_dag.csv.
            # The current converter emits explicit Prefill TIMER rows and can
            # also synthesize DECODE_READ_COMPUTE_WAIT TIMER rows while handling
            # STORAGE_TO_D. These synthetic rows are absent from the logical DAG.
            # They do not change the relative order of physical network rows, so
            # skip every TIMER in traffic.csv and align only URMA_READ/URMA_WRITE
            # records against the DAG's logical NETWORK expansion.
            traffic_rows=iter(rows(traffic,('taskId','sourceNode','destNode','dataSize(Byte)','opType')))

            def next_network_record():
                for raw_tid,src,dst,size,op in traffic_rows:
                    flow_id=number(raw_tid,'traffic taskId')
                    if op=='TIMER':
                        timer_bytes=number(size,'traffic TIMER bytes')
                        if timer_bytes != 0:
                            raise ValueError(f'taskId={flow_id}: TIMER row must have zero bytes, got {timer_bytes}')
                        # Keep the physical task ID slot, but do not associate a
                        # control TIMER with any Prefill Read/Write bucket.
                        emit(flow_id,-1,-1,'',-1)
                        continue
                    if op not in ('URMA_READ','URMA_WRITE'):
                        raise ValueError(f'taskId={flow_id}: unsupported traffic opType {op!r}')
                    return (flow_id,number(src,'traffic source'),number(dst,'traffic dest'),
                            number(size,'traffic bytes'),op)
                return None

            dag_fields=('task_id','task_class','task_type','batch_id','request_id','layer_index',
                        'tp_members','bytes','execution_schema')
            for logical_id,kind,typ,raw_batch,raw_req,raw_layer,members_txt,total,sc in rows(dag,dag_fields):
                if sc!=SCHEMA:
                    raise ValueError(f'DAG execution_schema mismatch: {sc}')
                if kind!='NETWORK':
                    continue
                members=json.loads(members_txt)
                if not isinstance(members,list) or not members:
                    raise ValueError(f'DAG task {logical_id}: logical NETWORK requires tp_members')
                total_check=0
                for member in members:
                    if not isinstance(member,list) or len(member)!=6:
                        raise ValueError(f'DAG task {logical_id}: malformed TP member')
                    rank,src,sp,dst,dp,size=member
                    if any(type(x)!=int for x in member) or size<=0:
                        raise ValueError(f'DAG task {logical_id}: invalid TP member')
                    total_check+=size
                    op = 'URMA_READ' if typ in ('PREFIX_READ','STORAGE_TO_D') else 'URMA_WRITE'
                    if op=='URMA_READ':
                        src,dst=dst,src
                    left=size
                    while left>0:
                        chunk=min(left,U32_MAX)
                        left-=chunk
                        record=next_network_record()
                        if record is None:
                            raise ValueError(f'traffic has fewer network flows than DAG expansion at DAG task {logical_id}')
                        flow_id,act_src,act_dst,act_size,act_op=record
                        expected=(src,dst,1+(chunk-1)//divisor,op)
                        actual=(act_src,act_dst,act_size,act_op)
                        if actual!=expected:
                            raise ValueError(f'taskId={flow_id} traffic != DAG network expansion: actual={actual}, expected={expected} (DAG task {logical_id})')
                        bid=int(raw_batch) if raw_batch else -1
                        lid=int(raw_layer) if raw_layer else -1
                        rid=int(raw_req) if raw_req else -1
                        emit(flow_id,bid,lid,typ,rid)
                if total_check!=number(total,'logical DAG bytes'):
                    raise ValueError(f'DAG task {logical_id}: TP member bytes do not match logical bytes')

            # Drain trailing control TIMER rows. Any remaining network row means
            # traffic.csv and task_dag.csv are from different conversions.
            extra=next_network_record()
            if extra is not None:
                raise ValueError(f'traffic has extra network taskId={extra[0]} after DAG expansion')

        # emit() flushes complete 262,144-entry chunks. Flush the final partial
        # chunk too, otherwise the index is shorter than the physical taskId range.
        buffer.tofile(f)

    if buffer.itemsize != 4:
        raise ValueError(f'index requires 4-byte unsigned entries; platform itemsize={buffer.itemsize}')
    expected_size = count * buffer.itemsize
    actual_size = path.stat().st_size
    if actual_size != expected_size:
        raise ValueError(
            f'internal index size mismatch: expected {expected_size} bytes for {count} task slots, '
            f'got {actual_size} bytes'
        )
    return path,first,count


class FlowBucket:
    __slots__=('valid','seen','started','bad','start','finish','intervals')
    def __init__(self):
        self.valid=0
        self.seen=0
        self.started=0
        self.bad=0
        self.start=None
        self.finish=None
        self.intervals=array('d')
    def record(self, st, fin):
        self.seen+=1
        if not (math.isfinite(st) and math.isfinite(fin) and st>=0 and fin>=st and fin>0):
            if math.isfinite(st) and math.isfinite(fin) and st>=0 and fin==0:
                self.started+=1
            else:
                self.bad+=1
            return
        self.valid+=1
        self.start=st if self.start is None else min(self.start,st)
        self.finish=fin if self.finish is None else max(self.finish,fin)
        self.intervals.extend([st,fin])


def scan_stats(path, index, first, count, buckets, progress):
    stats={}
    bitmap=bytearray((count+7)//8)
    summary=Counter()
    with index.open('rb') as f:
        mm=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
        try:
            for raw_id,start,finish in rows(path,('taskId','taskStartTime(us)','taskCompletesTime(us)')):
                tid=number(raw_id,'statistics taskId')
                pos=tid-first
                if not (0<=pos<count):
                    raise ValueError(f'{path}: taskId {tid} out of index range')
                code=struct.unpack_from('=I',mm,pos*4)[0]
                summary['statistics_rows']+=1
                if not code:
                    continue
                byte,bit=pos>>3,1<<(pos&7)
                if bitmap[byte]&bit:
                    raise ValueError(f'{path}: duplicate selected taskId {tid}')
                bitmap[byte]|=bit
                b=stats.get(code)
                if b is None:
                    b=stats[code]=FlowBucket()
                try:
                    st,fin=float(start),float(finish)
                except (ValueError,TypeError):
                    st,fin=math.nan,math.nan
                b.record(st,fin)
                if progress and summary['statistics_rows']%progress==0:
                    print(f'  Scan {path.parent.parent.name}: {summary["statistics_rows"]:,}',file=sys.stderr,flush=True)
        finally:
            mm.close()
    summary['selected_expected_flows']=sum(buckets.expected[1:])
    summary['selected_observed_flows']=sum(b.seen for b in stats.values())
    summary['selected_completed_flows']=sum(b.valid for b in stats.values())
    return stats,summary


def merged_intervals(flat):
    if not flat:
        return []
    # pair array is already bounded to user-selected batches.
    values=sorted((flat[i],flat[i+1]) for i in range(0,len(flat),2))
    out=[[values[0][0], values[0][1]]]
    for st,fin in values[1:]:
        if st <= out[-1][1]:
            if fin>out[-1][1]:
                out[-1][1]=fin
        else:
            out.append([st,fin])
    return out


def union_intervals(intervals):
    if not intervals:
        return []
    out=[]
    for st,fin in sorted(intervals):
        if not out or st>out[-1][1]:
            out.append([st,fin])
        else:
            out[-1][1]=max(out[-1][1],fin)
    return out


def duration(intervals):
    return sum(fin-st for st,fin in intervals)


def intersection_duration(a,b):
    i=j=0
    total=0.0
    while i<len(a) and j<len(b):
        total+=max(0.0,min(a[i][1],b[j][1])-max(a[i][0],b[j][0]))
        if a[i][1]<b[j][1]:
            i+=1
        else:
            j+=1
    return total


def rndms(x):
    return None if x is None else round(x/1000,6)


def collate(scheme, batches, durations, meta, details, buckets, stats):
    # One observed stage aggregate per batch/layer, and optional per-request.
    aggregate={}
    request_detail={}
    for code in range(1,len(buckets.keys)):
        bid,lid,typ,requester=buckets.keys[code]
        stat=stats.get(code)
        if stat is None:
            continue
        key=(bid,lid,typ)
        d=aggregate.setdefault(key, {'start':None,'finish':None,'expected':0,'valid':0,
                                     'active_intervals':array('d')})
        if requester!=-1:
            request_detail[(bid,lid,typ,requester)]=stat
        d['start']=stat.start if d['start'] is None else (min(d['start'],stat.start) if stat.start is not None else d['start'])
        d['finish']=stat.finish if d['finish'] is None else (max(d['finish'],stat.finish) if stat.finish is not None else d['finish'])
        d['valid']+=stat.valid
        d['active_intervals'].extend(stat.intervals)
    for code in range(1,len(buckets.keys)):
        bid,lid,typ,_=buckets.keys[code]
        key=(bid,lid,typ)
        d=aggregate.setdefault(key, {'start':None,'finish':None,'expected':0,'valid':0,
                                     'active_intervals':array('d')})
        d['expected']+=buckets.expected[code]

    stage_logical_bytes=Counter()
    for (bid0,lid0,stage0,_rid0), value in meta.items():
        stage_logical_bytes[(bid0,lid0,stage0)] += value[0]
    result={}
    layer_rows=[]
    batch_rows=[]
    request_rows=[]
    for bid,b in batches.items():
        ls=[]
        current=None
        net_flat=array('d')
        read_flat=array('d')
        write_flat=array('d')
        compute_intervals=[]
        gaps=0.0
        inconsistencies=0
        unresolved=0
        all_starts=[]
        all_ends=[]
        for lid in sorted(durations[bid]):
            stages={}
            for typ,flat in [('PREFIX_READ',read_flat),('PREFIX_STORE_WRITE',write_flat)]:
                a=aggregate.get((bid,lid,typ))
                observed = a is not None and a['expected']>0
                start=a['start'] if a else None
                finish=a['finish'] if a else None
                expected=a['expected'] if a else 0
                valid=a['valid'] if a else 0
                ints=merged_intervals(a['active_intervals']) if a else []
                for st,fin in ints:
                    flat.extend([st,fin])
                    net_flat.extend([st,fin])
                if start is not None:
                    all_starts.append(start)
                    all_ends.append(finish)
                stage_bytes=stage_logical_bytes[(bid,lid,typ)]
                stages[typ]={'start':rndms(start),'end':rndms(finish),'raw_start':start,
                             'raw_end':finish,'bytes':stage_bytes,'expected':expected,
                             'completed':valid,'fully_completed':valid==expected,
                             'active_ms':rndms(duration(ints)),
                             'busy':[[rndms(x),rndms(y)] for x,y in ints]
                                 if len(ints)<=MAX_STAGE_BUSY_INTERVALS_HTML else [],
                             'busy_omitted':len(ints)>MAX_STAGE_BUSY_INTERVALS_HTML}
                if observed and valid<expected:
                    unresolved+=expected-valid
            r=stages['PREFIX_READ']
            w=stages['PREFIX_STORE_WRITE']
            # Compute projection is a hybrid inference: anchor is upstream DAG
            # forecast (NOT ns-3), while each layer waits for actual read end.
            previous=current
            baseline=b['forecast_compute_start_us'] if previous is None else previous
            if r['expected']:
                if not r['fully_completed'] or r['raw_end'] is None:
                    # Explicitly refuse to infer an entire compute pipeline from
                    # an incomplete read barrier.
                    current=None
                    compute_start=compute_end=None
                else:
                    compute_start=max(baseline,r['raw_end'])
                    if previous is not None:
                        gaps+=max(0.0, r['raw_end']-previous)
                    compute_end=compute_start+durations[bid][lid]
                    current=compute_end
            elif previous is None and lid>0:
                compute_start=compute_end=None
            else:
                compute_start=baseline
                compute_end=compute_start+durations[bid][lid]
                current=compute_end
            if compute_end is not None:
                compute_intervals.append([compute_start,compute_end])
                all_starts.append(compute_start)
                all_ends.append(compute_end)
            if compute_end is not None and w['raw_start'] is not None and w['raw_start']+1e-9<compute_end:
                inconsistencies+=1
            layer_network=union_intervals([
                interval for typ in STAGES if (bid,lid,typ) in aggregate
                for interval in merged_intervals(aggregate[(bid,lid,typ)]['active_intervals'])
            ])
            overlap=intersection_duration([[compute_start,compute_end]],layer_network) if compute_end is not None else None
            first_write_gap=(w['raw_start']-compute_end) if compute_end is not None and w['raw_start'] is not None else None
            d={'layer':lid,'compute_start_ms':rndms(compute_start),'compute_end_ms':rndms(compute_end),
               'compute_duration_ms':rndms(durations[bid][lid]),'read':{k:v for k,v in r.items() if not k.startswith('raw_')},
               'write':{k:v for k,v in w.items() if not k.startswith('raw_')},
               'layer_overlap_ms':rndms(overlap),'write_start_gap_ms':rndms(first_write_gap)}
            ls.append(d)
            layer_rows.append({'scheme':scheme,'batch_id':bid,'layer_index':lid,
                'compute_start_ms_est':d['compute_start_ms'],'compute_end_ms_est':d['compute_end_ms'],
                'compute_duration_ms':d['compute_duration_ms'],
                'read_start_ms':r['start'],'read_end_ms':r['end'],'read_bytes':r['bytes'],
                'read_expected_flows':r['expected'],'read_completed_flows':r['completed'],
                'write_start_ms':w['start'],'write_end_ms':w['end'],'write_bytes':w['bytes'],
                'write_expected_flows':w['expected'],'write_completed_flows':w['completed'],
                'compute_network_overlap_ms':d['layer_overlap_ms'],'write_start_gap_ms':d['write_start_gap_ms']})
        compute_merged=union_intervals(compute_intervals)
        read_merged=merged_intervals(read_flat)
        write_merged=merged_intervals(write_flat)
        net_merged=merged_intervals(net_flat)
        overlap=intersection_duration(compute_merged,net_merged)
        compute_active=duration(compute_merged)
        span=max(all_ends)-min(all_starts) if all_starts else None
        info={'scheme':scheme,'batch_id':bid,'batch_size':b['batch_size'],
              'request_count':len(b['requests']),'service_domain':b['service_domain'],
              'forecast_compute_start_ms':rndms(b['forecast_compute_start_us']),
              'compute_model_ms':rndms(sum(durations[bid].values())),
              'compute_estimated_active_ms':rndms(compute_active),
              'read_active_ms':rndms(duration(read_merged)),'write_active_ms':rndms(duration(write_merged)),
              'network_active_ms':rndms(duration(net_merged)),
              'compute_network_overlap_ms':rndms(overlap),
              'compute_overlap_ratio':round(overlap/compute_active,6) if compute_active>0 else None,
              'read_stall_ms_est':rndms(gaps),'pipeline_span_ms_est':rndms(span),
              'missing_network_flows':unresolved,'write_before_est_compute_end_layers':inconsistencies,
              'compute_layers_estimated':len(compute_intervals),'layers_total':len(ls)}
        batch_rows.append(info)
        request_list=[]
        for rid in sorted(details[bid]):
            item={'request_id':rid,'layers':{}}
            for l in ls:
                lid=l['layer']
                track={}
                for typ,name in (('PREFIX_READ','read'),('PREFIX_STORE_WRITE','write')):
                    flow=request_detail.get((bid,lid,typ,rid))
                    size=meta.get((bid,lid,typ,rid),[0])[0]
                    track[name]={'start':rndms(flow.start) if flow else None,
                                 'end':rndms(flow.finish) if flow else None,
                                 'bytes':size,
                                 'expected':buckets.expected[buckets.key2code[(bid,lid,typ,rid)]] if (bid,lid,typ,rid) in buckets.key2code else 0,
                                 'completed':flow.valid if flow else 0}
                    request_rows.append({'scheme':scheme,'batch_id':bid,'request_id':rid,'layer_index':lid,
                                         'stage':name,'start_ms':track[name]['start'],
                                         'end_ms':track[name]['end'],'logical_bytes':size,
                                         'expected_flows':track[name]['expected'],
                                         'completed_flows':track[name]['completed']})
                item['layers'][lid]=track
            request_list.append(item)
        result[bid]={'summary':info,'layers':ls,'requests':request_list,
                     'network_busy':[[rndms(x),rndms(y)] for x,y in net_merged],
                     'read_busy':[[rndms(x),rndms(y)] for x,y in read_merged],
                     'write_busy':[[rndms(x),rndms(y)] for x,y in write_merged]}
    return result,batch_rows,layer_rows,request_rows


def write_csv(path, lines, fields):
    tmp=path.with_suffix(path.suffix+'.tmp')
    try:
        with tmp.open('w',newline='',encoding='utf-8-sig') as out:
            w=csv.DictWriter(out,fieldnames=fields)
            w.writeheader()
            w.writerows(lines)
        os.replace(tmp,path)
    finally:
        if tmp.exists():
            tmp.unlink()


def load_report_template():
    # Fully self-contained HTML; no external CDN or HTTP server.
    return r'''<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Prefill Batch Pipeline v3 · ECMP / TE / TOE / EPS-2L</title>
<style>
:root{color-scheme:light;--bg:#f7f8fa;--ink:#182335;--mut:#68748a;--line:#d9e0e8;--panel:#fff;--read:#1c95b5;--comp:#517bbd;--write:#25957c;--overlap:#9566b2}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI','Microsoft YaHei',sans-serif}
header{background:#fff;border-bottom:1px solid var(--line);padding:18px 28px}h1{font-size:23px;margin:0 0 5px}.sub{color:var(--mut);font-size:12px}
main{margin:auto;max-width:1520px;padding:18px 25px 35px}.bar{display:flex;flex-wrap:wrap;gap:13px;align-items:end;margin-bottom:12px}
label{font-size:12px;color:var(--mut);display:flex;gap:5px;flex-direction:column}select,input{border:1px solid #cfd7e1;border-radius:6px;padding:7px 9px;background:white;min-width:130px;color:var(--ink)}input[type=number]{min-width:80px;max-width:125px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:15px;margin-bottom:14px;overflow:hidden}h2{font-size:16px;margin:0 0 9px}.scroll{overflow:auto;width:100%;touch-action:none;cursor:grab}.scroll:active{cursor:grabbing}#overview{touch-action:none;cursor:crosshair}
.cards{display:grid;grid-template-columns:repeat(5,minmax(120px,1fr));gap:10px}.metric{background:#f5f7fa;border-radius:8px;padding:10px}.metric strong{display:block;font-size:19px}.metric span{font-size:11px;color:var(--mut)}
svg{display:block;max-width:none}svg rect:hover{filter:brightness(1.07)}.legend{display:flex;gap:16px;flex-wrap:wrap;font-size:12px;color:var(--mut);margin:7px 0}.dot{display:inline-block;width:10px;height:10px;margin-right:5px;border-radius:2px;vertical-align:middle}.read{background:var(--read)}.comp{background:var(--comp)}.write{background:var(--write)}.overlap{background:var(--overlap)}
.badge{border-radius:4px;padding:2px 5px;background:#f5d9aa;font-size:11px}.note{font-size:12px;color:#6c6770;margin:9px 0 0}table{border-collapse:collapse;width:100%;min-width:900px}th,td{border-bottom:1px solid #e5e9ef;text-align:right;padding:7px 8px;font-size:12px}td:first-child,th:first-child{text-align:left}th{background:#f6f7fa;position:sticky;top:0}
button{border:1px solid #cbd6e3;background:white;border-radius:6px;padding:7px 10px;cursor:pointer}button:hover{background:#ebf2fc}.foot{font-size:12px;color:var(--mut)}.buttonrow{display:flex;align-items:end;gap:5px;flex-wrap:wrap}.checkbox{display:inline-flex;align-items:center;gap:5px;color:var(--ink);font-size:12px;white-space:nowrap;padding-bottom:6px;flex-direction:row}.checkbox input{min-width:0;max-width:none;width:15px;height:15px}.manual{display:flex;gap:12px;align-items:end}.statusline{display:flex;align-items:center;gap:12px;flex-wrap:wrap;font-size:12px;color:#40516a;margin-bottom:4px}[hidden]{display:none!important}
@media(max-width:800px){.cards{grid-template-columns:repeat(2,minmax(100px,1fr))}header{padding:15px}main{padding:13px}}
</style></head><body>
<header><h1>Prefill Batch · Read / Compute / Write Pipeline</h1><div class="sub">当前仓库 V15 DAG + ns-3 实测 KV 网络流 · Compute 基于 DAG 与 Read 完成时间推算 · 四方案局部同轴 · 逐段虚线边界</div></header>
<main>
<section class="panel"><div class="bar">
<label>Prefill Batch<select id="batch"></select></label>
<label>当前方案<select id="scheme"><option value="ecmp">ECMP</option><option value="te">TE</option><option value="toe">TOE</option><option value="eps2">EPS-2L</option></select></label>
<label>起始 Layer<input id="start" type="number" min="0" value="0"></label>
<label>显示 Layer 数<input id="count" type="number" min="1" max="78" value="12"></label>
<label>Request 分片<select id="req"></select></label>
</div><div id="batchline" class="sub"></div></section>
<section class="panel"><h2>Batch 全局概览 · 拖拽选择局部时间窗口</h2><div id="overview" class="scroll"></div><p id="overviewNote" class="foot"></p>
<div class="legend"><span><i class="dot read"></i>Read</span><span><i class="dot comp"></i>Compute（估）</span><span><i class="dot write"></i>Write</span><span>蓝色虚线框：当前时间窗口</span></div></section>
<section class="panel"><h2>局部时间轴控制</h2>
<div class="bar">
<label>时间范围<select id="rangeMode"><option value="layers">选中 Layer 自动缩放（默认）</option><option value="full">完整 Batch</option><option value="manual">自定义时间窗口</option></select></label>
<label>自动聚焦范围<select id="alignScope"><option value="all">四方案统一刻度（默认）</option><option value="current">仅当前方案（更精细）</option></select></label>
<div class="manual" id="manualGroup" hidden><label>开始时间 ms<input id="timeMin" type="number" step="any"></label><label>结束时间 ms<input id="timeMax" type="number" step="any"></label><button id="manualApply" type="button">应用</button></div>
<div class="buttonrow"><button id="zoomIn" title="时间窗口缩小一半" type="button">放大 +</button><button id="zoomOut" title="时间窗口扩大一倍" type="button">缩小 −</button><button id="panLeft" type="button">← 平移</button><button id="panRight" type="button">平移 →</button><button id="fitLayers" type="button">恢复自动缩放</button></div>
<label class="checkbox"><input type="checkbox" id="bounds" checked>显示每段起止虚线</label>
</div><div class="statusline"><strong id="currentWindow"></strong></div><p id="windowNote" class="note"></p></section>
<section class="panel"><h2>四方案局部流水线对比（同一时间坐标）</h2><div id="compare" class="scroll"></div>
<div class="legend"><span><i class="dot read"></i>Read 活跃</span><span><i class="dot comp"></i>Compute（估算）</span><span><i class="dot write"></i>Write 活跃</span><span><i class="dot overlap"></i>Compute ∩ 任意网络</span></div>
<div id="windowMetrics" class="scroll"></div><p class="foot">当前窗口统计只针对选中的 Layer 计算区间。Read 与 Write 可能同时活跃，因此两者的重叠值不能相加当作总重叠。</p></section>
<section class="panel"><h2>Batch 逐层 Read / Compute / Write · 重叠高亮</h2><div id="timeline" class="scroll"></div>
<div class="legend"><span><i class="dot read"></i>Storage → P：HIT KV Read</span><span><i class="dot comp"></i>Batch 共享 Layer Compute（估）</span><span><i class="dot write"></i>P → Storage：MISS KV Write</span><span><i class="dot overlap"></i>计算与任意网络活跃重叠</span></div>
<p class="foot">Read/Write 浅色为从第一条流开始到最后一条流结束的时间包络，深色为真实完成流的活跃区间并集；计算框与起止虚线均为推算时间。若某层网络活跃片段过多，详情图只显示浅色包络，紫色重叠仍使用全量网络并集计算。</p></section>
<section class="panel"><h2>完整 Batch 流水线统计</h2><div id="metrics" class="cards"></div><p id="warning" class="note"></p></section>
<section class="panel"><h2>所选 Request 的逐层传输</h2><p class="foot">本区域独立展示当前 Request 的逐层 Read / Write；Batch Compute 是共享计算区间，不属于单个 Request 独占的计算任务。</p><div id="requestinfo" class="sub"></div><div id="requestgraph" class="scroll"></div></section>
<section class="panel"><h2>逐层详细数据</h2><div id="layersTable" class="scroll"></div><p class="foot">与计算相关的统计仍是推算量。Read/Write 区间来自 task_statistics.csv；流时长和活跃片段按已完成流统计，不包括未完成流。</p></section>
<section class="panel"><h2>计算时间推断方法和限制</h2><p class="foot">每层计算时长来自 v15 DAG 的 PREFILL_LAYER.duration_us（GLM FLOPs 或 legacy）。首层起点锚定 batches_debug.csv 的 forecast_compute_start_us，后续每层取“上一层推算计算结束”和“本层全部 Read 观测完成”的较晚者。这不是 ns-3 GPU 计算事件。任何层的必需 Read 缺失/未完成时，该层及无法可信推断的后续计算留空。网络流观测与 DAG 预测存在差异时，推算计算与网络重叠不代表真实 GPU 与网络同步执行。网络活跃时间按每条完成流的区间并集统计；浅色区间包络不代表持续占用。HTML 仅含所选 Batch，通过 --batch-id 添加。</p></section>
</main><script id="pipeline-data" type="application/json">__PIPELINE_JSON__</script><script>
'use strict';
const D=JSON.parse(document.getElementById('pipeline-data').textContent);
const $=id=>document.getElementById(id);
const colors={read:'#1c95b5',compute:'#517bbd',write:'#25957c',overlap:'#9566b2'};
const labels={ecmp:'ECMP',te:'TE',toe:'TOE',eps2:'EPS-2L'};
const names=['ecmp','te','toe','eps2'];
const WIDTH=1140, PLOT_LEFT=126, PLOT_RIGHT=1115, PLOT_WIDTH=PLOT_RIGHT-PLOT_LEFT;
const valid=x=>typeof x==='number' && Number.isFinite(x);
function el(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function fmt(x,d=3){return valid(x)?Number(x).toFixed(d):'—';}
const fmtBytes=n=>n>=1048576?(n/1048576).toFixed(1)+' MiB':n>=1024?(n/1024).toFixed(1)+' KiB':n+' B';
const data=()=>D.schemes[$('scheme').value][$('batch').value];
const allData=()=>names.map(k=>[k,D.schemes[k][$('batch').value]]);
function selectedLayers(d){const layers=d.layers;
  const begin=Math.min(layers.length-1,Math.max(0,Math.trunc(Number($('start').value)||0)));
  const n=Math.min(layers.length-begin,Math.max(1,Math.trunc(Number($('count').value)||12)));
  return layers.slice(begin,begin+n);
}
function collectBounds(layers){const arr=[];
  for(const l of layers){for(const item of [l.read,l.write])if(item&&valid(item.start)&&valid(item.end))arr.push(item.start,item.end);
    if(valid(l.compute_start_ms)&&valid(l.compute_end_ms))arr.push(l.compute_start_ms,l.compute_end_ms);
  }
  return arr;
}
function spanBounds(v){if(!v.length)return null;
  let a=Infinity,b=-Infinity;for(const x of v){if(valid(x)){a=Math.min(a,x);b=Math.max(b,x);}}
  return Number.isFinite(a)&&Number.isFinite(b)?[a,b]:null;
}
function padDomain(domain,fraction){if(!domain)return [0,1];const [a,b]=domain,len=Math.max(0,b-a);
  const pad=Math.max(0.00005,len*fraction);
  return [a-pad,b+pad];
}
function fullDomain(){let values=[];for(const [,d] of allData())values.push(...collectBounds(d.layers));
  return padDomain(spanBounds(values),.025);
}
function localDomain(){const selected=$('alignScope').value==='current'?[[$('scheme').value,data()]]:allData();
  let values=[];for(const [,d] of selected)values.push(...collectBounds(selectedLayers(d)));
  return padDomain(spanBounds(values)||spanBounds(collectBounds(data().layers)),.05);
}
let manualWindow=null;
function currentWindow(){const mode=$('rangeMode').value;
  if(mode==='full')return fullDomain();
  if(mode==='manual'&&manualWindow&&manualWindow[1]>manualWindow[0])return manualWindow.slice();
  return localDomain();
}
function fixedInput(n){return Number(n.toFixed(6)).toString();}
function setManual(a,b){if(!valid(a)||!valid(b)||b-a<0.000001)return;
  manualWindow=[a,b];$('rangeMode').value='manual';
  $('timeMin').value=fixedInput(a);$('timeMax').value=fixedInput(b);
  render();
}
function applyManualInputs(){const a=Number($('timeMin').value),b=Number($('timeMax').value);
  if(!$('timeMin').value.trim()||!$('timeMax').value.trim()||!valid(a)||!valid(b)||b-a<0.000001){
    $('windowNote').textContent='时间区间无效：结束时间必须大于开始时间（单位 ms）';return;
  }
  setManual(a,b);
}
function scaleTime(t,dom){return PLOT_LEFT+(t-dom[0])/(dom[1]-dom[0])*PLOT_WIDTH;}
function xToTime(evt,svg,dom){const r=svg.getBoundingClientRect();
  const x=(evt.clientX-r.left)*WIDTH/r.width;
  return dom[0]+(x-PLOT_LEFT)/PLOT_WIDTH*(dom[1]-dom[0]);
}
function plotCoord(evt,svg){const r=svg.getBoundingClientRect();return (evt.clientX-r.left)*WIDTH/r.width;}
function getRuler(domain,height){const [a,b]=domain,span=b-a;
  const ticks=7,dec=span>=1000?0:span>=100?1:span>=10?2:span>=1?3:span>=.01?4:6;
  let str=`<line x1="${PLOT_LEFT}" y1="32" x2="${PLOT_RIGHT}" y2="32" stroke="#d8e2ee"/>`;
  for(let i=0;i<ticks;i++){const value=a+span*i/(ticks-1),x=scaleTime(value,domain);
    str+=`<line x1="${x}" y1="32" x2="${x}" y2="${height-10}" stroke="#a6b5c8" stroke-opacity=".27" stroke-dasharray="3 5" pointer-events="none"/>`;
    str+=`<text x="${x}" y="20" font-size="10" text-anchor="middle" fill="#64748b">${fmt(value,dec)} ms</text>`;
  }
  return str;
}
function svgStart(key,height,domain){return `<svg data-plot="${key}" width="${WIDTH}" height="${height}" viewBox="0 0 ${WIDTH} ${height}" role="img" aria-label="Batch ${el($('batch').value)} ${key}，${fmt(domain[0],4)} 至 ${fmt(domain[1],4)} ms"><defs><clipPath id="clip-${key}"><rect x="${PLOT_LEFT}" y="27" width="${PLOT_WIDTH}" height="${height-32}"/></clipPath></defs>`;}
function plotRect(a,b,y,h,color,domain,title,opts={}){
  if(!valid(a)||!valid(b)||b<a||b<domain[0]||a>domain[1])return '';
  const left=Math.max(a,domain[0]),right=Math.min(b,domain[1]);
  const x=scaleTime(left,domain),xEnd=scaleTime(right,domain),w=Math.max(1.1,xEnd-x);
  const alpha=opts.envelope?'.16':opts.incomplete?'.27':'.90';
  let result=`<rect x="${x.toFixed(2)}" y="${y}" width="${w.toFixed(2)}" height="${h}" rx="2" fill="${color}" fill-opacity="${alpha}"${opts.estimate?' stroke="#234b7a" stroke-width="1.2" stroke-dasharray="4 2"':opts.envelope?' stroke="'+color+'" stroke-opacity=".5" stroke-dasharray="2 2"':''}><title>${el(title)} · ${fmt(a,6)}–${fmt(b,6)} ms · ${fmt(b-a,6)} ms</title></rect>`;
  if($('bounds').checked && !opts.noBounds){
    for(const t of [a,b])if(t>=domain[0]&&t<=domain[1]){
      const tx=scaleTime(t,domain);
      result+=`<line x1="${tx.toFixed(2)}" y1="${y-3}" x2="${tx.toFixed(2)}" y2="${y+h+3}" stroke="#243449" stroke-opacity=".76" stroke-width="1.15" stroke-dasharray="3 3" pointer-events="none"/>`;
    }
  }
  if(opts.text && w>52)result+=`<text x="${(x+w/2).toFixed(2)}" y="${y+h*.74}" font-size="9" fill="#fff" text-anchor="middle" pointer-events="none">${el(opts.text)}</text>`;
  return result;
}
function cutIntervals(a,b,ints){const out=[];if(!valid(a)||!valid(b)||b<=a)return out;
  for(const item of ints||[]){const s=Math.max(a,item[0]),e=Math.min(b,item[1]);if(e>s)out.push([s,e]);}
  return out;
}
function unionIntervals(lists){const arr=lists.flat().filter(p=>valid(p[0])&&valid(p[1])&&p[1]>p[0]);
  arr.sort((a,b)=>a[0]-b[0]);const out=[];
  for(const p of arr){if(!out.length||p[0]>out[out.length-1][1])out.push([...p]);
    else out[out.length-1][1]=Math.max(out[out.length-1][1],p[1]);}
  return out;
}
const totalDuration=ints=>ints.reduce((x,p)=>x+p[1]-p[0],0);
const networkBusyCache=new WeakMap();
function networkBusy(d){let cached=networkBusyCache.get(d);if(!cached){cached=unionIntervals([d.read_busy||[],d.write_busy||[]]);networkBusyCache.set(d,cached);}return cached;}
function layerOverlap(layer,d){return cutIntervals(layer.compute_start_ms,layer.compute_end_ms,networkBusy(d));}
function visibleMetrics(d,layers,dom){const cs=unionIntervals([layers.filter(l=>valid(l.compute_start_ms)&&valid(l.compute_end_ms))
  .map(l=>[l.compute_start_ms,l.compute_end_ms])]);
  const cActive=totalDuration(cs.flatMap(p=>cutIntervals(dom[0],dom[1],[p])));
  const read=d.read_busy||[],write=d.write_busy||[],net=networkBusy(d);
  const intersect=(list)=>totalDuration(cs.flatMap(([s,e])=>cutIntervals(s,e,list).flatMap(p=>cutIntervals(dom[0],dom[1],[p]))));
  const overlap=intersect(net);
  return {compute:cActive,overlap,read:intersect(read),write:intersect(write),ratio:cActive>0?overlap/cActive:null};
}
function plotStage(stage,name,y,h,dom){
  if(!stage||stage.expected===0)return `<text x="${PLOT_LEFT+5}" y="${y+12}" font-size="10" fill="#94a3b8">无该阶段网络流</text>`;
  if(!valid(stage.start)||!valid(stage.end))return `<text x="${PLOT_LEFT+5}" y="${y+12}" font-size="10" fill="#ba7342">流未完成或没有有效时间</text>`;
  const completed=stage.fully_completed;
  const tip=`${name} ${fmtBytes(stage.bytes)} · ${stage.completed}/${stage.expected} flows`;
  let str=plotRect(stage.start,stage.end,y,h,colors[name],dom,tip+'（起止包络，内部可能有空隙）',{envelope:true});
  if(stage.busy&&stage.busy.length){
    for(const [a,b] of stage.busy)str+=plotRect(a,b,y+2,h-4,colors[name],dom,tip+' · 已完成流活跃区间',{noBounds:true});
  }else if(stage.busy_omitted){
    str+=`<text x="${PLOT_LEFT+5}" y="${y+h-3}" font-size="9" fill="#7c596d" pointer-events="none">活跃片段过多，仅显示包络</text>`;
  }
  if(!completed)str+=`<text x="${PLOT_RIGHT-5}" y="${y+h-3}" font-size="9" fill="#ad653e" text-anchor="end" pointer-events="none">未完成 ${stage.expected-stage.completed}</text>`;
  return str;
}
function renderOverview(){const dom=fullDomain(),w=WIDTH,h=224;
  let str=svgStart('overview',h,dom)+getRuler(dom,h);
  for(let i=0;i<names.length;i++){
    const name=names[i],d=D.schemes[name][$('batch').value],y=45+i*43;
    str+=`<text x="7" y="${y+14}" fill="#344052" font-weight="600" font-size="12">${labels[name]}</text>`;
    for(const [ints,key,off] of [[d.read_busy,'read',0],[d.write_busy,'write',9]])for(const [a,b] of ints||[]){
      str+=plotRect(a,b,y+off,7,colors[key],dom,labels[name]+' '+key, {noBounds:true});
    }
    for(const l of d.layers)if(valid(l.compute_start_ms)&&valid(l.compute_end_ms)){
      str+=plotRect(l.compute_start_ms,l.compute_end_ms,y+20,10,colors.compute,dom,`L${l.layer} 共享计算（估算）`,{estimate:true,noBounds:true});
    }
    str+=`<line x1="${PLOT_LEFT}" y1="${y+37}" x2="${PLOT_RIGHT}" y2="${y+37}" stroke="#e9edf2"/>`;
  }
  const selected=currentWindow(),a=Math.max(dom[0],selected[0]),b=Math.min(dom[1],selected[1]);
  if(b>a){const x=scaleTime(a,dom),width=Math.max(2,scaleTime(b,dom)-x);
    str+=`<rect id="overviewSelection" x="${x}" y="34" width="${width}" height="182" fill="#6379b1" fill-opacity=".11" stroke="#364e92" stroke-width="1.4" stroke-dasharray="5 3" pointer-events="none"/>`;
  }
  str+='</svg>';
  $('overview').innerHTML=str;
  $('overviewNote').textContent=`整体范围 ${fmt(dom[0],3)}～${fmt(dom[1],3)} ms；蓝色虚线框为当前放大区间。拖拽概览图中的时间区域可选取更短的分析范围。`;
}
function renderComparison(dom){const h=361,w=WIDTH;
  let str=svgStart('compare',h,dom)+getRuler(dom,h);
  const windowRows=[];
  for(let i=0;i<names.length;i++){
    const name=names[i],d=D.schemes[name][$('batch').value],layers=selectedLayers(d),y=45+i*78;
    const m=visibleMetrics(d,layers,dom);
    windowRows.push(`<tr><td>${labels[name]}</td><td>${fmt(m.compute,3)}</td><td>${fmt(m.read,3)}</td><td>${fmt(m.write,3)}</td><td>${fmt(m.overlap,3)}</td><td>${m.ratio==null?'—':fmt(m.ratio*100,1)+'%'}</td></tr>`);
    str+=`<text x="5" y="${y+12}" fill="#344052" font-weight="600" font-size="12">${labels[name]}</text>`;
    for(const [offset,initial] of [[0,'R'],[15,'C'],[30,'W'],[45,'O']])str+=`<text x="110" y="${y+offset+10}" fill="#8190a2" text-anchor="end" font-size="10">${initial}</text>`;
    for(const [a,b] of d.read_busy||[])str+=plotRect(a,b,y,11,colors.read,dom,labels[name]+' Read 活跃');
    for(const layer of layers)if(valid(layer.compute_start_ms))str+=plotRect(layer.compute_start_ms,layer.compute_end_ms,y+15,11,colors.compute,dom,labels[name]+` Layer ${layer.layer} 计算估算`,{estimate:true});
    for(const [a,b] of d.write_busy||[])str+=plotRect(a,b,y+30,11,colors.write,dom,labels[name]+' Write 活跃');
    for(const layer of layers)for(const [a,b] of layerOverlap(layer,d))str+=plotRect(a,b,y+45,11,colors.overlap,dom,labels[name]+` Layer ${layer.layer} 计算∩网络活跃`);
    str+=`<line x1="${PLOT_LEFT}" y1="${y+64}" x2="${PLOT_RIGHT}" y2="${y+64}" stroke="#edf1f5"/>`;
  }
  str+='</svg>';
  $('compare').innerHTML=str;
  $('windowMetrics').innerHTML='<table><thead><tr><th>方案</th><th>窗口内计算 ms（估）</th><th>计算∩Read ms</th><th>计算∩Write ms</th><th>计算∩任意网络 ms</th><th>重叠率</th></tr></thead><tbody>'+windowRows.join('')+'</tbody></table>';
}
function renderTimeline(dom){const d=data(),ls=selectedLayers(d),rowH=102,h=52+ls.length*rowH;
  let str=svgStart('timeline',h,dom)+getRuler(dom,h);
  const live=networkBusy(d);
  for(let i=0;i<ls.length;i++){
    const l=ls[i],y=45+i*rowH;
    str+=`<text x="8" y="${y+18}" font-size="12" font-weight="600" fill="#344052">Layer ${l.layer}</text>`;
    for(const [offset,label] of [[0,'Read'],[21,'Compute'],[42,'Write'],[63,'Overlap']])str+=`<text x="111" y="${y+offset+12}" font-size="10" text-anchor="end" fill="#7f8da0">${label}</text>`;
    str+=plotStage(l.read,'read',y,15,dom);
    if(valid(l.compute_start_ms))str+=plotRect(l.compute_start_ms,l.compute_end_ms,y+21,15,colors.compute,dom,`Layer ${l.layer} Batch 共享计算（根据 DAG 估算）`,{estimate:true,text:fmt(l.compute_duration_ms,3)+' ms'});
    else str+=`<text x="${PLOT_LEFT+4}" y="${y+33}" font-size="10" fill="#a56a44">计算区间无法推断</text>`;
    str+=plotStage(l.write,'write',y+42,15,dom);
    if(valid(l.compute_start_ms))for(const [a,b] of cutIntervals(l.compute_start_ms,l.compute_end_ms,live))str+=plotRect(a,b,y+63,15,colors.overlap,dom,`Layer ${l.layer}：Compute 与任意 Read / Write 流活跃区间重叠`);
    str+=`<line x1="5" y1="${y+92}" x2="${PLOT_RIGHT}" y2="${y+92}" stroke="#e9edf3"/>`;
  }
  $('timeline').innerHTML=str+'</svg>';
}
function renderRequest(dom){const d=data(),rid=$('req').value;
  if(!rid){$('requestinfo').textContent='选择 Request 查看其 Read / Write 与 Batch 共享计算的时间关系。';$('requestgraph').innerHTML='';return;}
  const req=d.requests.find(r=>String(r.request_id)===rid);
  if(!req){$('requestinfo').textContent='该 Request 未包含在展示子集，可通过 --request-id 指定';$('requestgraph').innerHTML='';return;}
  const ls=selectedLayers(d),rowH=78,h=51+ls.length*rowH;
  let str=svgStart('request',h,dom)+getRuler(dom,h);
  for(let i=0;i<ls.length;i++){
    const l=ls[i],z=req.layers[l.layer],y=44+i*rowH;
    str+=`<text x="10" y="${y+16}" font-size="12" fill="#344052">Layer ${l.layer}</text>`;
    for(const [stage,off] of [['read',0],['write',42]]){
      const v=z?.[stage];str+=`<text x="110" y="${y+off+12}" text-anchor="end" font-size="10" fill="#8090a1">${stage}</text>`;
      if(v&&valid(v.start)&&valid(v.end))str+=plotRect(v.start,v.end,y+off,14,colors[stage],dom,`Request ${rid} ${stage} ${fmtBytes(v.bytes)} ${v.completed}/${v.expected} · 网络流起止包络（内部可能有空隙）`,{envelope:true});
    }
    str+=`<text x="110" y="${y+33}" text-anchor="end" font-size="10" fill="#8090a1">Batch C</text>`;
    if(valid(l.compute_start_ms))str+=plotRect(l.compute_start_ms,l.compute_end_ms,y+21,14,colors.compute,dom,`Batch 共享 Layer ${l.layer} 计算（估算）`,{estimate:true});
    str+=`<line x1="5" y1="${y+64}" x2="${PLOT_RIGHT}" y2="${y+64}" stroke="#edf0f3"/>`;
  }
  $('requestinfo').textContent=`Request ${rid}：浅色网络矩形表示该 Request 的分片起止包络，内部可能有空隙；Batch Compute 是共享的估算值。时间窗口与四方案对比保持同步。`;
  $('requestgraph').innerHTML=str+'</svg>';
}
function metric(name,val){return `<div class="metric"><strong>${val}</strong><span>${name}</span></div>`;}
function renderMetrics(){const m=data().summary;
  $('metrics').innerHTML=metric('计算总时长 · ms',fmt(m.compute_model_ms))+metric('网络活跃 · ms',fmt(m.network_active_ms))+metric('Read 活跃 · ms',fmt(m.read_active_ms))+metric('Write 活跃 · ms',fmt(m.write_active_ms))+metric('计算/网络重叠 · ms',fmt(m.compute_network_overlap_ms))+metric('计算重叠率',m.compute_overlap_ratio==null?'—':(100*m.compute_overlap_ratio).toFixed(1)+'%')+metric('Read 导致的估算停顿 · ms',fmt(m.read_stall_ms_est))+metric('流水线跨度 · ms',fmt(m.pipeline_span_ms_est))+metric('缺失/未完成流',m.missing_network_flows)+metric('违反推算时间顺序的层数',m.write_before_est_compute_end_layers);
  $('warning').textContent='此处是整个 Batch 的统计值，与上方「选定层 / 当前时间窗口」的比较表不同。计算是 DAG + 观测 Read 推算，不是 GPU 实测。Compute End 之后才可写入的依赖若与网络日志冲突，不能把推算的重叠当成真实 GPU 行为。';
}
function renderTable(){const ls=data().layers;let str='<table><thead><tr><th>Layer</th><th>Read 起 / 终 ms</th><th>Read MiB</th><th>Read 完成</th><th>Compute 起 / 终 ms（估）</th><th>Compute ms</th><th>Write 起 / 终 ms</th><th>Write MiB</th><th>Write 完成</th><th>本层重叠 ms</th></tr></thead><tbody>';
  for(const l of ls)str+=`<tr><td>L${l.layer}</td><td>${fmt(l.read.start,4)} / ${fmt(l.read.end,4)}</td><td>${fmt(l.read.bytes/1048576,2)}</td><td>${l.read.completed}/${l.read.expected}</td><td>${fmt(l.compute_start_ms,4)} / ${fmt(l.compute_end_ms,4)}</td><td>${fmt(l.compute_duration_ms,4)}</td><td>${fmt(l.write.start,4)} / ${fmt(l.write.end,4)}</td><td>${fmt(l.write.bytes/1048576,2)}</td><td>${l.write.completed}/${l.write.expected}</td><td>${fmt(l.layer_overlap_ms,4)}</td></tr>`;
  $('layersTable').innerHTML=str+'</tbody></table>';
}
function updateReq(){const bid=$('batch').value,scheme=$('scheme').value,d=D.schemes[scheme][bid],prev=$('req').value;
  $('req').innerHTML='<option value="">汇总视图</option>'+d.requests.map(r=>`<option value="${r.request_id}">Request ${r.request_id}</option>`).join('');
  if(d.requests.some(r=>String(r.request_id)===prev))$('req').value=prev;
}
function render(){const batch=D.batches[$('batch').value],dom=currentWindow();
  $('batchline').textContent=`Batch ${batch.batch_id} · ${batch.batch_size} Requests · Domain ${batch.service_domain} · TP ${batch.tp_degree} · ${data().layers.length} 层。仅分析选择的 Batch；可通过 --batch-id 添加。`;
  $('currentWindow').textContent=`当前显示：${fmt(dom[0],6)} ～ ${fmt(dom[1],6)} ms（跨度 ${fmt(dom[1]-dom[0],6)} ms）`;
  $('manualGroup').hidden=$('rangeMode').value!=='manual';
  if($('rangeMode').value!=='manual'){ $('timeMin').value=fixedInput(dom[0]);$('timeMax').value=fixedInput(dom[1]);}
  $('windowNote').textContent='时间坐标单位 ms；滚轮缩放、拖拽平移详细图；概览图拖拽选区。默认四方案同刻度，可切换仅当前方案自动聚焦。';
  renderOverview();renderComparison(dom);renderTimeline(dom);renderMetrics();renderRequest(dom);renderTable();
}
// Global overview brush: selecting from 0 to 100 ms is not bound to an arbitrary 78-layer scale.
let brush=null;
$('overview').addEventListener('pointerdown',e=>{
  if(e.button!==0)return;const svg=$('overview').querySelector('svg');if(!svg)return;
  const px=plotCoord(e,svg);if(px<PLOT_LEFT||px>PLOT_RIGHT)return;
  brush={x:px,svg};$('overview').setPointerCapture(e.pointerId);e.preventDefault();
});
$('overview').addEventListener('pointermove',e=>{
  if(!brush)return;const x=Math.max(PLOT_LEFT,Math.min(PLOT_RIGHT,plotCoord(e,brush.svg)));
  const selection=brush.svg.querySelector('#overviewSelection');
  if(selection){selection.setAttribute('x',Math.min(brush.x,x));selection.setAttribute('width',Math.max(2,Math.abs(x-brush.x)));}
});
$('overview').addEventListener('pointerup',e=>{
  if(!brush)return;const dom=fullDomain(),svg=brush.svg,px=brush.x;
  const x=Math.max(PLOT_LEFT,Math.min(PLOT_RIGHT,plotCoord(e,svg)));brush=null;
  const left=dom[0]+(Math.min(x,px)-PLOT_LEFT)/PLOT_WIDTH*(dom[1]-dom[0]);
  const right=dom[0]+(Math.max(x,px)-PLOT_LEFT)/PLOT_WIDTH*(dom[1]-dom[0]);
  if(Math.abs(x-px)>=4 && right-left>=.000001)setManual(left,right);
  else{const center=(left+right)/2,old=currentWindow(),width=old[1]-old[0];setManual(center-width/2,center+width/2);}
});
$('overview').addEventListener('pointercancel',()=>{brush=null;renderOverview();});
function zoom(factor,center=null){const d=currentWindow(),c=center??((d[0]+d[1])/2);
  setManual(c+(d[0]-c)*factor,c+(d[1]-c)*factor);
}
function pan(frac){const d=currentWindow(),dt=(d[1]-d[0])*frac;setManual(d[0]+dt,d[1]+dt);}
for(const id of ['compare','timeline','requestgraph']){
  const box=$(id);
  box.addEventListener('wheel',e=>{
    const svg=box.querySelector('svg');if(!svg)return;
    e.preventDefault();const dom=currentWindow(),loc=xToTime(e,svg,dom);
    zoom(e.deltaY<0?.8:1.25,loc);
  },{passive:false});
  let drag=null;
  box.addEventListener('pointerdown',e=>{
    if(e.button!==0||!box.querySelector('svg'))return;
    const svg=box.querySelector('svg');if(plotCoord(e,svg)<PLOT_LEFT)return;
    drag={x:e.clientX,dom:currentWindow(),width:svg.getBoundingClientRect().width};
    box.setPointerCapture(e.pointerId);e.preventDefault();
  });
  box.addEventListener('pointermove',e=>{
    if(!drag)return;
    const delta=(e.clientX-drag.x)*WIDTH/drag.width/PLOT_WIDTH*(drag.dom[1]-drag.dom[0]);
    setManual(drag.dom[0]-delta,drag.dom[1]-delta);
  });
  box.addEventListener('pointerup',()=>{drag=null;});
  box.addEventListener('pointercancel',()=>{drag=null;});
  box.addEventListener('dblclick',()=>{$('rangeMode').value='layers';render();});
}
for(const bid of Object.keys(D.batches))$('batch').add(new Option(`Batch ${bid}`,bid));
for(const id of ['batch','scheme'])$(id).addEventListener('change',()=>{
  manualWindow=null;$('rangeMode').value='layers';updateReq();render();
});
for(const id of ['start','count','alignScope','bounds'])$(id).addEventListener('change',render);
$('rangeMode').addEventListener('change',()=>{
  if($('rangeMode').value==='manual'&&!manualWindow){manualWindow=localDomain();}
  render();
});
for(const id of ['timeMin','timeMax'])$(id).addEventListener('change',applyManualInputs);
$('manualApply').addEventListener('click',applyManualInputs);
$('req').addEventListener('change',()=>renderRequest(currentWindow()));
$('zoomIn').addEventListener('click',()=>zoom(.5));
$('zoomOut').addEventListener('click',()=>zoom(2));
$('panLeft').addEventListener('click',()=>pan(-.3));
$('panRight').addEventListener('click',()=>pan(.3));
$('fitLayers').addEventListener('click',()=>{$('rangeMode').value='layers';render();});
updateReq();render();

</script></body></html>'''


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--case',required=True,type=Path,help='当前 V15 生成器的 --out-dir，包含 task_dag.csv / requests_debug.csv / batches_debug.csv')
    for scheme in SCHEMES:
        parser.add_argument('--'+scheme+'-dir',type=Path,required=True,help=LABELS[scheme]+' result directory')
        parser.add_argument('--'+scheme+'-statistics',type=Path,help='override statistics path')
    parser.add_argument('--mapping',type=Path,help='当前 mooncake_dag_to_traffic_v15.py 通过 --mapping-output 生成的 mapping.csv（推荐）')
    parser.add_argument('--traffic',type=Path,help='当前 V15 converter 生成的 traffic.csv；仅在没有 mapping.csv 时使用')
    parser.add_argument('--data-size-divisor',type=int,default=1,help='required to validate fallback traffic, default 1')
    parser.add_argument('--batch-id',type=int,action='append',help='explicit Prefill Batch ID, repeatable')
    parser.add_argument('--max-batches',type=int,default=6,help='first N Prefill batches if no --batch-id')
    parser.add_argument('--detail-requests',type=int,default=12,help='Requests per selected batch in interactive viewer')
    parser.add_argument('--request-id',type=int,action='append',help='additional Request to retain in detailed viewer')
    parser.add_argument('--out-dir',type=Path,help='default DAG_OUTPUT_DIR/output/pipeline_analysis')
    parser.add_argument('--work-dir',type=Path,help='temp on-disk flow index')
    parser.add_argument('--progress-every',type=int,default=1000000)
    parser.add_argument('--version',action='version',version=VERSION)
    args=parser.parse_args(argv)
    if args.max_batches<1 or args.detail_requests<0 or args.data_size_divisor<1 or args.progress_every<0:
        parser.error('max-batches >=1, detail-requests >=0, divisor >=1, progress-every >=0 required')
    if args.batch_id and any(x<0 for x in args.batch_id):
        parser.error('batch IDs must be nonnegative')
    case=args.case.resolve()
    dag=case/'task_dag.csv'
    debug=case/'requests_debug.csv'
    bdebug=case/'batches_debug.csv'
    mapping=args.mapping or next((p for p in (case/'mapping.csv',case/'traffic_task_mapping.csv',case/'traffic_mapping.csv') if p.is_file()),None)
    traffic=args.traffic or case/'traffic.csv'
    out=(args.out_dir or case/'output'/'pipeline_analysis').resolve()
    stats_paths={}
    for scheme in SCHEMES:
        special=getattr(args,scheme+'_statistics')
        root=getattr(args,scheme+'_dir')
        candidates=(root/'output'/'task_statistics.csv', root/'test'/'task_statistics.csv',
                    root/'task_statistics.csv')
        discovered=next((p for p in candidates if p.is_file()),root/'output'/'task_statistics.csv')
        stats_path=(special or discovered).resolve()
        stats_paths[scheme]=stats_path
    inputs=[dag,debug,bdebug,*(stats_paths.values()),mapping or traffic]
    for f in inputs:
        if not f.is_file():
            raise ValueError(f'input not found: {f}')
    out.mkdir(parents=True,exist_ok=True)
    for p in (out/'pipeline_report.html',out/'pipeline_batch_summary.csv',out/'pipeline_layer_detail.csv',out/'pipeline_request_detail.csv'):
        if p.resolve() in [x.resolve() for x in inputs]:
            raise ValueError('output cannot overwrite input')
    fingerprints={p.resolve():(p.stat().st_size,p.stat().st_mtime_ns) for p in inputs}
    batches=select_batches(bdebug,args.batch_id or [],args.max_batches)
    reqs,detail=load_requests(debug,batches,args.detail_requests,set(args.request_id or []))
    durations,logical_bytes=load_dag_layers(dag,batches)
    print('Selected Prefill batches: '+', '.join(map(str,batches)),file=sys.stderr)
    print('Index mode: '+('v15 mapping' if mapping else 'traffic/DAG network validation (TIMER slots retained)'),file=sys.stderr)
    bucket=Buckets(batches,detail)
    work=(args.work_dir or out).resolve()
    work.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.pipeline_',dir=work) as name:
        index,first,count=build_index(mapping,dag,traffic,args.data_size_divisor,Path(name),bucket,args.progress_every)
        report={'version':VERSION,'upstream_revision':UPSTREAM_REVISION,'batches':batches,'requests':reqs,'schemes':{}}
        summaries=[]; layers=[]; requests_detail=[]
        for scheme in SCHEMES:
            print(f'Analyzing {LABELS[scheme]}: {stats_paths[scheme]}',file=sys.stderr,flush=True)
            observed,scan_report=scan_stats(stats_paths[scheme],index,first,count,bucket,args.progress_every)
            scheme_report,summary_rows,layer_rows,request_rows=collate(scheme,batches,durations,logical_bytes,detail,bucket,observed)
            report['schemes'][scheme]=scheme_report
            summaries.extend(summary_rows)
            layers.extend(layer_rows)
            requests_detail.extend(request_rows)
            print(f'  selected flows completed {scan_report["selected_completed_flows"]:,} / expected {scan_report["selected_expected_flows"]:,}',file=sys.stderr)
            del observed
    for p in inputs:
        if fingerprints[p.resolve()]!=(p.stat().st_size,p.stat().st_mtime_ns):
            raise ValueError(f'input changed during analysis: {p}')
    write_csv(out/'pipeline_batch_summary.csv',summaries,list(summaries[0].keys()))
    write_csv(out/'pipeline_layer_detail.csv',layers,list(layers[0].keys()))
    if requests_detail:
        write_csv(out/'pipeline_request_detail.csv',requests_detail,list(requests_detail[0].keys()))
    else:
        write_csv(out/'pipeline_request_detail.csv',[],['scheme','batch_id','request_id','layer_index','stage','start_ms','end_ms','logical_bytes','expected_flows','completed_flows'])
    # JSON cannot contain a literal '<' that could close the script tag.
    json_data=json.dumps(report,ensure_ascii=False,separators=(',',':'),allow_nan=False).replace('<','\\u003c').replace('>','\\u003e').replace('&','\\u0026')
    html_path=out/'pipeline_report.html'
    temp_html=html_path.with_suffix('.html.tmp')
    try:
        temp_html.write_text(load_report_template().replace('__PIPELINE_JSON__',json_data),encoding='utf-8')
        os.replace(temp_html,html_path)
    finally:
        temp_html.unlink(missing_ok=True)
    print('Saved '+str(html_path))
    for f in ('pipeline_batch_summary.csv','pipeline_layer_detail.csv','pipeline_request_detail.csv'):
        print('Saved '+str(out/f))
    return 0


if __name__=='__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, csv.Error, json.JSONDecodeError, OverflowError) as e:
        print('Analysis failed: '+str(e),file=sys.stderr)
        sys.exit(1)
