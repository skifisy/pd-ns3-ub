"""Bounded playback queries; trend aggregation never changes raw frame metrics."""
import math
from decimal import Decimal

PS = 10**12


def interval(info, start, end):
    start = info['recording']['start'] if start is None else start
    end = info['recording']['end'] if end is None else end
    if not math.isfinite(start) or not math.isfinite(end) or start < 0 or end <= start:
        raise ValueError('Choose a positive finite simulation interval')
    return start, end


def progress_rows(db, case_id, start, end):
    # Include the preceding boundary so a seek starts with the correct counters.
    lo, hi = int(Decimal(str(start))*PS), int(Decimal(str(end))*PS)
    seed = db.execute('SELECT time_ps,started,completed,running FROM playback_progress '
                      'WHERE case_id=? AND time_ps<=? ORDER BY time_ps DESC LIMIT 1',
                      (case_id,lo)).fetchone()
    rows = ([seed] if seed else [(lo,0,0,0)]) + list(db.execute(
        'SELECT time_ps,started,completed,running FROM playback_progress '
        'WHERE case_id=? AND time_ps>? AND time_ps<=? ORDER BY time_ps',(case_id,lo,hi)))
    return [dict(time=t/PS,started=s,completed=c,running=r) for t,s,c,r in rows]


def completion_trend(db, case_id, start, end, points):
    lo, hi = int(Decimal(str(start))*PS), int(Decimal(str(end))*PS)
    stride = max(1, math.ceil((hi-lo)/points))
    seed = db.execute('SELECT time_ps,completed FROM playback_progress WHERE case_id=? '
                      'AND time_ps<=? ORDER BY time_ps DESC LIMIT 1',(case_id,lo)).fetchone()
    result = [dict(time=start,completed=seed[1] if seed else 0)]
    rows = db.execute('SELECT MAX(time_ps),MAX(completed) FROM playback_progress '
                      'WHERE case_id=? AND time_ps>? AND time_ps<=? '
                      'GROUP BY CAST((time_ps-?-1)/? AS INTEGER) ORDER BY 1',
                      (case_id,lo,hi,lo,stride))
    result.extend(dict(time=t/PS,completed=n) for t,n in rows)
    return result


def global_trend(db, case_id, start, end, points, scale):
    width = (end-start)/points
    rows = db.execute('''SELECT MIN(t0),MAX(t1),MAX(max_util),MAX(hot),MIN(measured),
        SUM(decode_bits),SUM(decode_duration),COUNT(decode_bits)=COUNT(*),
        SUM(fabric_bits),SUM(fabric_capacity),COUNT(fabric_bits)=COUNT(*)
        FROM global_samples WHERE case_id=? AND t0>=? AND t0<?
        GROUP BY CAST((t0-?)/? AS INTEGER) ORDER BY 1''',
        (case_id,start,end,start,width))
    return [dict(time=t,end=b,maxUtilization=u,hotLinks=h,measuredLinks=n,
                 storageDecodeGbps=bits/duration/1e9*scale if decode_ok and duration else None,
                 fabricUtilization=wire/cap if fabric_ok and cap else None)
            for t,b,u,h,n,bits,duration,decode_ok,wire,cap,fabric_ok in rows]


def link_trend(db, case_id, src, dst, start, end, points, scale):
    width = (end-start)/points
    rows = db.execute('''SELECT MIN(g.t0),MAX(g.t1),
        SUM(l.capacity*(g.t1-g.t0))/SUM(g.t1-g.t0),
        SUM(l.gbps*(g.t1-g.t0))/SUM(g.t1-g.t0),
        SUM(l.gbps*(g.t1-g.t0))/NULLIF(SUM(l.capacity*(g.t1-g.t0)),0),
        MAX(l.util),MAX(l.queue)
        FROM global_samples g LEFT JOIN link_trends l
        ON l.case_id=g.case_id AND l.t0=g.t0 AND l.src=? AND l.dst=?
        WHERE g.case_id=? AND g.t0>=? AND g.t0<?
        GROUP BY CAST((g.t0-?)/? AS INTEGER) ORDER BY 1''',
        (src,dst,case_id,start,end,start,width))
    return [dict(time=t,end=b,capacity=cap*scale if cap is not None else 0,
                 gbps=rate*scale if rate is not None else None,
                 utilization=u,maxUtilization=peak,queueMB=q)
            for t,b,cap,rate,u,peak,q in rows]
