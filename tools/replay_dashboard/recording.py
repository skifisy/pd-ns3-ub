"""Build a passive, evidence-driven recording plan from imported logs."""
from collections import defaultdict
from bisect import bisect_right
import math


def route_intervals(case):
    if 'sampledRouteIntervals' in case:
        return sorted(case['sampledRouteIntervals'],key=lambda r:(r['start'],r['src'],r['dst'],r['transit']))
    per_flow = defaultdict(list)
    resets = sorted(e['time'] for e in case['events'] if e['event'] == 'topology-applied')
    for record in case.get('routeEvidence', []):
        per_flow[(record['taskId'], record['flowHash'])].append(record)
    grouped = defaultdict(list)
    for records in per_flow.values():
        records.sort(key=lambda r: r['time'])
        for i, record in enumerate(records):
            start = record['time']
            end = record['end'] if record['end'] is not None else case['end']
            if i + 1 < len(records):
                end = min(end, records[i + 1]['time'])
            if case['mode'] == 'toe+te':
                index = bisect_right(resets,start)
                if index < len(resets):end = min(end,resets[index])
            if end <= start:
                continue
            grouped[(record['src'], record['dst'], record['transit'])].append((start, end))
    result = []
    for (src, dst, transit), intervals in grouped.items():
        merged = []
        for start, end in sorted(intervals):
            if merged and start <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([start, end])
        result += [dict(src=src, dst=dst, transit=transit, start=a, end=b) for a, b in merged]
    return sorted(result, key=lambda r: (r['start'], r['src'], r['dst'], r['transit']))


def change_groups(case):
    """Plans come from add/remove records; outcomes come only from applied snapshots."""
    groups = {}
    for plan in case['plans']:
        epoch = plan['epoch']
        group = groups.setdefault(epoch, dict(epoch=epoch, planTime=plan['time'], time=None, deltas={}))
        group['planTime'] = min(group['planTime'], plan['time'])
        a, b = sorted((plan['src'], plan['dst']))
        group['deltas'][(a, b)] = group['deltas'].get((a, b), 0) + (1 if plan['status'] == 'add' else -1)
    states = sorted(case['topologyStates'], key=lambda x: x['time'])
    for previous, current in zip(states, states[1:]):
        before = {(e['src'], e['dst']): e for e in previous['edges'] if e['src'] < e['dst']}
        after = {(e['src'], e['dst']): e for e in current['edges'] if e['src'] < e['dst']}
        group = groups.setdefault(current['epoch'], dict(epoch=current['epoch'], planTime=None, time=None, deltas={}))
        group['time'] = current['time']
        group['changes'] = [dict(src=a, dst=b, before=before.get((a,b), {}).get('circuits', 0),
                                  after=after.get((a,b), {}).get('circuits', 0),
                                  delta=after.get((a,b), {}).get('circuits', 0)-before.get((a,b), {}).get('circuits', 0))
                            for a,b in sorted(before.keys() | after.keys())
                            if before.get((a,b), {}).get('circuits', 0) != after.get((a,b), {}).get('circuits', 0)]
    for group in groups.values():
        if 'changes' not in group:
            # An un-applied or failed plan remains a plan, never a new topology.
            state = next((s for s in reversed(states) if s['time'] <= group['planTime']), states[0])
            counts = {(e['src'], e['dst']): e['circuits'] for e in state['edges'] if e['src'] < e['dst']}
            group['changes'] = [dict(src=a,dst=b,before=counts.get((a,b),0),after=counts.get((a,b),0)+delta,delta=delta)
                                for (a,b),delta in group['deltas'].items() if delta]
        group.pop('deltas')
        group['changes'].sort(key=lambda x: (-abs(x['delta']),x['src'],x['dst']))
        group['added'] = sum(max(0,c['delta']) for c in group['changes'])
        group['removed'] = sum(max(0,-c['delta']) for c in group['changes'])
    return sorted(groups.values(), key=lambda g: g['planTime'] if g['planTime'] is not None else g['time'])


def build_recording(config, cases, baseline, leaves):
    settings = config.get('recording', {})
    begin = float(settings.get('start_seconds', min(c['start'] for c in cases)))
    end = float(settings.get('end_seconds', max(c['end'] for c in cases)))
    duration = float(settings.get('duration_seconds', 90))
    if not all(math.isfinite(v) for v in (begin,end,duration)) or begin < 0 or end <= begin or duration <= 0:
        raise ValueError('recording start/end/duration must define a positive finite interval')
    if begin < min(c['start'] for c in cases) or end > max(c['end'] for c in cases):
        raise ValueError('recording interval must be inside the available log range')
    base_scores = {(p['src'],p['dst']): p['wireBytes'] for p in baseline['pairScores']}
    for case in cases:
        case['routeIntervals'] = route_intervals(case)
        case['changes'] = change_groups(case)
    candidates = [p for p in baseline['pairScores'] if p['src'] != p['dst']]
    fallback = max(candidates, key=lambda p: p['wireBytes']) if candidates else dict(src=leaves[0],dst=leaves[-1])
    specified = config.get('focus')
    focus = specified or {k:fallback[k] for k in ('src','dst')}
    chapters = [dict(time=begin,kind='overview',title='同一时刻的全局性能',focus=focus)]
    for case in cases:
        for mode in ('te','toe'):
            if mode == 'te' and case['mode'] in ('te','toe+te'):
                routes = [r for r in case['routeIntervals'] if r['transit'] >= 0 and r['start'] < end and r['end'] > begin]
                if routes:
                    pair = tuple(int(specified[k]) for k in ('src','dst')) if specified else max({(r['src'],r['dst']) for r in routes},key=lambda p:base_scores.get(p,0))
                    relevant = [r for r in routes if (r['src'],r['dst']) == pair]
                    if relevant:
                        at = max(begin,min(r['start'] for r in relevant))
                        chapters.append(dict(time=at,kind='te',caseId=case['id'],title='TE：实际中转选路',focus=dict(src=pair[0],dst=pair[1])))
            if mode == 'toe' and case['mode'].startswith('toe'):
                changes = [g for g in case['changes'] if g['time'] is not None and begin <= g['time'] <= end]
                if changes:
                    group = changes[0]
                    positive = [d for d in group['changes'] if d['delta']>0]
                    candidate = max(positive,key=lambda d: max(base_scores.get((d['src'],d['dst']),0),base_scores.get((d['dst'],d['src']),0))) if positive else None
                    pair = focus if specified or candidate is None else dict(src=candidate['src'],dst=candidate['dst'])
                    if not specified and candidate and base_scores.get((pair['dst'],pair['src']),0)>base_scores.get((pair['src'],pair['dst']),0):
                        pair = dict(src=pair['dst'],dst=pair['src'])
                    at = max(begin,group['planTime'] if group['planTime'] is not None else group['time'])
                    chapters.append(dict(time=at,kind='toe',caseId=case['id'],title='TOE：连接容量变化',focus=pair))
    chapters.sort(key=lambda c:c['time'])
    anchors = sorted({e['time'] for c in cases for e in c['events']
                      if e['event'] in ('te-policy-published','toe-draining','topology-applied','toe-drain-timeout')
                      and begin < e['time'] < end} | {c['time'] for c in chapters if begin<c['time']<end})
    windows = []
    for t in anchors:
        lo,hi=max(begin,t-1),min(end,t+3)
        if windows and lo<=windows[-1][1]:windows[-1][1]=max(windows[-1][1],hi)
        else:windows.append([lo,hi])
    cuts=sorted({begin,end} | {x for w in windows for x in w})
    segments=[dict(start=a,end=b,weight=4 if any(lo<=(a+b)/2<=hi for lo,hi in windows) else 1) for a,b in zip(cuts,cuts[1:])]
    total=sum((s['end']-s['start'])*s['weight'] for s in segments)
    for segment in segments:segment['speed']=total/(duration*segment['weight'])
    return dict(start=begin,end=end,durationSeconds=duration,chapters=chapters,segments=segments,
                taskId=None,hotspotThreshold=float(config.get('hotspot_threshold',.9)))
