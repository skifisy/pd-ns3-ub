"""Bounded multiresolution queries with exact boundary descent and raw peaks."""
import math

try:
    from .metrics import PS_PER_SECOND
    from .prepare import FANOUT
except ImportError:
    from metrics import PS_PER_SECOND
    from prepare import FANOUT


def tiles(db, direction, level, first, last, start_ps, end_ps):
    """Use coarse tiles only when wholly inside the request, descend both edges."""
    width = FANOUT**level
    result = []
    found = db.execute("""SELECT idx,first,last,t0,t1,payload,wire,capacity,cap_min,cap_max,
                         payload_peak,wire_peak FROM tiles
                         WHERE direction=? AND level=? AND idx BETWEEN ? AND ? ORDER BY idx""",
                       (direction, level, first // width, last // width)).fetchall()
    for row in found:
        _, lo, hi, t0, t1, *_ = row
        if hi < first or lo > last or t1 <= start_ps or t0 >= end_ps:
            continue
        if level > 0 and (lo < first or hi > last or t0 < start_ps or t1 > end_ps):
            result.extend(tiles(db, direction, level - 1, max(first, lo), min(last, hi), start_ps, end_ps))
        else:
            result.append(row)
    return result


def series(db, info, start, end, points=400, metric="payload", pair_ids=None):
    if not math.isfinite(start) or not math.isfinite(end) or end <= start:
        raise ValueError("Specify a finite, positive time range")
    if start < info["start"] or end > info["end"]:
        raise ValueError("Time range is outside the recording")
    if metric not in info["metrics"]:
        raise ValueError("Requested metric is unavailable in this cache")
    if not 16 <= points <= 2000:
        raise ValueError("points must be between 16 and 2000")
    selected = info["pairs"]
    if pair_ids is not None:
        known = {p["id"] for p in selected}
        if not pair_ids or not set(pair_ids) <= known:
            raise ValueError("Unknown or empty pair selection")
        selected = [p for p in selected if p["id"] in pair_ids]
    # Round only to the logger's picosecond resolution, never to a 10 ms grid.
    start_ps, end_ps = round(start * PS_PER_SECOND), round(end * PS_PER_SECOND)
    first_row = db.execute("SELECT id FROM windows WHERE t1>? ORDER BY t1 LIMIT 1", (start_ps,)).fetchone()
    last_row = db.execute("SELECT id FROM windows WHERE t0<? ORDER BY t0 DESC LIMIT 1", (end_ps,)).fetchone()
    if not first_row or not last_row or first_row[0] > last_row[0]:
        raise ValueError("No samples intersect this time range")
    first, last = first_row[0], last_row[0]
    level = 0
    while (last - first + 1) / FANOUT**level > points and level < info["max_level"]:
        level += 1
    scale = info["throughput_scale"] / 1e9
    result = {}
    for pair in selected:
        for direction in pair["directions"]:
            data = []
            for row in tiles(db, direction, level, first, last, start_ps, end_ps):
                _, _, _, t0, t1, payload, wire, capacity, cap_min, cap_max, payload_peak, wire_peak = row
                left, right = max(t0, start_ps), min(t1, end_ps)
                duration = (t1 - t0) / PS_PER_SECOND
                byte_count = payload if metric == "payload" else wire
                peak = payload_peak if metric == "payload" else wire_peak
                # A partially selected raw window has its recorded mean rate.
                # No claim of finer-than-logged instantaneous measurement is made.
                data.append([left / PS_PER_SECOND, right / PS_PER_SECOND,
                             byte_count * 8 / duration * scale, peak * scale,
                             capacity / duration * scale, cap_min * scale, cap_max * scale])
            current = db.execute("SELECT bps FROM capacities WHERE direction=? AND time<=? ORDER BY time DESC LIMIT 1",
                                 (direction, start_ps)).fetchone()
            capacity = [[start, (current[0] if current else 0) * scale]]
            capacity.extend([time / PS_PER_SECOND, bps * scale] for time, bps in db.execute(
                "SELECT time,bps FROM capacities WHERE direction=? AND time>? AND time<? ORDER BY time",
                (direction, start_ps, end_ps)))
            capacity.append([end, capacity[-1][1]])
            result[str(direction)] = {"bins": data, "capacity": capacity}
    return {"start": start, "end": end, "metric": metric, "level": level, "directions": result,
            "columns": ["start_s", "end_s", "mean_gbps", "peak_gbps", "capacity_mean_gbps",
                        "capacity_min_gbps", "capacity_max_gbps"]}
