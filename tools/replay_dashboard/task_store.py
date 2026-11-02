"""Bounded task import and disk-backed playback progress (no task arrays)."""
import sqlite3
from decimal import Decimal, InvalidOperation
from itertools import chain
from pathlib import Path
from topology import csv_rows

BATCH = 20000
PS = 10**12


def picoseconds(value, scale=1000):
    return int(Decimal(str(value)) * scale)


def task_statistics_path(root, config):
    """Resolve only the required per-case output/task_statistics.csv file."""
    root = Path(root).resolve()
    configured = config.get('task_statistics')
    if configured:
        candidate = Path(configured)
        if not candidate.is_absolute():
            candidate = root / candidate
    else:
        replay_path = (root / config['path']).resolve()
        case_root = replay_path.parent if replay_path.name.lower() == 'replay' else replay_path
        candidate = case_root / 'output' / 'task_statistics.csv'
    candidate = candidate.resolve()
    if candidate.name != 'task_statistics.csv' or candidate.parent.name != 'output':
        raise ValueError(
            f"{config['id']}: task_statistics must point to output/task_statistics.csv, got {candidate}"
        )
    if not candidate.is_file():
        raise FileNotFoundError(
            f"{config['id']}: required output/task_statistics.csv not found: {candidate}"
        )
    return candidate


def task_sources(root, config):
    """Return the playback-progress source; never fall back to events or .tr files."""
    return 'statistics', [task_statistics_path(root, config)]


def create_tables(db):
    db.executescript('''
        CREATE TABLE workload(task_id INTEGER PRIMARY KEY,src INTEGER,dst INTEGER,size INTEGER,op TEXT);
        CREATE TABLE tasks(case_id TEXT,task_id INTEGER,start_ps INTEGER,end_ps INTEGER,kind TEXT,node INTEGER,
                           PRIMARY KEY(case_id,task_id)) WITHOUT ROWID;
        CREATE TABLE completion_bins(case_id TEXT,time_ps INTEGER,started INTEGER,completed INTEGER,ended_started INTEGER,
                                     PRIMARY KEY(case_id,time_ps)) WITHOUT ROWID;
        CREATE TABLE playback_progress(case_id TEXT,time_ps INTEGER,started INTEGER,completed INTEGER,running INTEGER,
                                       PRIMARY KEY(case_id,time_ps)) WITHOUT ROWID;
    ''')


def import_workload(db, path, reporter):
    batch = []
    for r in reporter.rows(path):
        batch.append((int(r['taskId']), int(r['sourceNode']), int(r['destNode']), int(r['dataSize(Byte)']), r.get('opType','')))
        if len(batch) >= BATCH:
            db.executemany('INSERT INTO workload VALUES(?,?,?,?,?)', batch)
            batch.clear(); db.commit()
    if batch:
        db.executemany('INSERT INTO workload VALUES(?,?,?,?,?)', batch)
    db.commit()


def task_time_us(value, case, task_id, column):
    raw = '' if value is None else str(value).strip()
    if not raw or raw.lower() in ('nan', 'na', 'n/a', 'none', 'null'):
        return None
    try:
        number = Decimal(raw)
    except InvalidOperation as error:
        raise ValueError(f'{case}: task {task_id} has invalid {column} value {raw!r}') from error
    if number.is_nan():
        return None
    if not number.is_finite():
        raise ValueError(f'{case}: task {task_id} has non-finite {column} value {raw!r}')
    if number < 0:
        raise ValueError(f'{case}: task {task_id} has negative {column} value {raw!r}')
    # task_statistics.csv timestamps are in microseconds; store in picoseconds.
    return int(number * 10**6)


def import_tasks(db, root, config, reporter):
    case = config['id']
    source, files = task_sources(root, config)
    path = files[0]
    required = (
        'taskId', 'sourceNode', 'destNode', 'dataSize(Byte)', 'opType',
        'taskStartTime(us)', 'taskCompletesTime(us)',
    )
    statement = 'INSERT INTO tasks VALUES(?,?,?,?,?,?)'

    def records():
        rows = iter(reporter.rows(path))
        first = next(rows, None)
        if first is None:
            raise ValueError(f'{case}: {path} is empty or has no task rows')
        missing = [column for column in required if column not in first]
        if missing:
            raise ValueError(f'{case}: {path} is missing required columns: {", ".join(missing)}')
        for row in chain((first,), rows):
            try:
                task_id = int(row['taskId'])
            except (TypeError, ValueError) as error:
                raise ValueError(f'{case}: {path} contains an invalid taskId: {row.get("taskId")!r}') from error
            start = task_time_us(row.get('taskStartTime(us)'), case, task_id, 'taskStartTime(us)')
            end = task_time_us(row.get('taskCompletesTime(us)'), case, task_id, 'taskCompletesTime(us)')
            if end is not None and start is None:
                raise ValueError(f'{case}: task {task_id} has a completion time but no start time in {path}')
            if start is not None and end is not None and end < start:
                raise ValueError(f'{case}: task {task_id} completes before it starts in {path}')
            node_raw = row.get('sourceNode')
            try:
                node = int(node_raw) if node_raw is not None and node_raw.strip() else -1
            except (TypeError, ValueError) as error:
                raise ValueError(f'{case}: task {task_id} has invalid sourceNode {node_raw!r} in {path}') from error
            yield case, task_id, start, end, row.get('opType') or 'unknown', node

    batch = []
    try:
        for record in records():
            batch.append(record)
            if len(batch) >= BATCH:
                db.executemany(statement, batch)
                batch.clear()
                db.commit()
        if batch:
            db.executemany(statement, batch)
        db.commit()
    except sqlite3.IntegrityError as error:
        raise ValueError(
            f'{case}: duplicate taskId or conflicting task rows in {path}: {error}'
        ) from error

    if db.execute(
        'SELECT 1 FROM tasks WHERE case_id=? AND (start_ps<0 OR end_ps<0 OR end_ps<start_ps) LIMIT 1',
        (case,),
    ).fetchone():
        raise ValueError(f'{case}: invalid task timestamps in {path}')
    if db.execute(
        'SELECT 1 FROM tasks t LEFT JOIN workload w USING(task_id) WHERE t.case_id=? AND w.task_id IS NULL LIMIT 1',
        (case,),
    ).fetchone():
        raise ValueError(f'{case}: task id is absent from workload: {path}')
    completed, started = db.execute(
        'SELECT SUM(start_ps IS NOT NULL AND end_ps IS NOT NULL), SUM(start_ps IS NOT NULL) '
        'FROM tasks WHERE case_id=?',
        (case,),
    ).fetchone()
    if not completed:
        raise ValueError(
            f'{case}: no usable completed tasks in {path}; finite taskStartTime(us) and '
            'taskCompletesTime(us) values are required'
        )
    total = db.execute('SELECT COUNT(*) FROM workload').fetchone()[0]
    return dict(
        totalTasks=total,
        completedTasks=completed or 0,
        pendingTasks=total - (started or 0),
        taskSource=source,
    )


def completion_buckets(db,case,step_ps,end,reporter,retain=True):
    # ceil(timestamp / step) ensures no future completion is shown at the left boundary.
    reporter.stage('task-buckets',f'生成 {step_ps / 10**9:g} ms 任务进度',case)
    for field,label in (('start_ps','started'),('end_ps','completed')):
        db.execute(f'''INSERT INTO completion_bins SELECT case_id,((CAST({field}/? AS INTEGER)+({field}%? > 0))*?),
            {"COUNT(*)" if label=="started" else "0"},{"COUNT(*)" if label=="completed" else "0"},
            {"SUM(start_ps IS NOT NULL)" if label=="completed" else "0"}
            FROM tasks WHERE case_id=? AND {field} IS NOT NULL GROUP BY 1,2
            ON CONFLICT(case_id,time_ps) DO UPDATE SET started=started+excluded.started,
            completed=completed+excluded.completed,ended_started=ended_started+excluded.ended_started''',(step_ps,step_ps,step_ps,case))
    rows=iter(db.execute('SELECT time_ps,started,completed,ended_started FROM completion_bins WHERE case_id=? ORDER BY time_ps',(case,)))
    row=next(rows,None); started=completed=ended=0; result=[]; batch=[]
    end_ps=int(round(end*PS))
    ticks=range(0,end_ps+step_ps,step_ps)
    for when in ticks:
        boundary=min(when,end_ps)
        while row is not None and row[0]<=boundary:
            started+=row[1];completed+=row[2];ended+=row[3];row=next(rows,None)
        # At the last partial boundary, the ceil bin extends past the run.
        if when>=end_ps and end_ps%step_ps:
            started,completed,ended=db.execute('SELECT SUM(start_ps<=?),SUM(end_ps<=?),SUM(start_ps IS NOT NULL AND end_ps<=?) FROM tasks WHERE case_id=?',(end_ps,end_ps,end_ps,case)).fetchone()
            started,completed,ended=started or 0,completed or 0,ended or 0
        batch.append((case,boundary,started,completed,started-ended))
        if retain:
            result.append(dict(time=boundary/PS,started=started,completed=completed,running=started-ended))
        if len(batch)>=2000:
            db.executemany('INSERT INTO playback_progress VALUES(?,?,?,?,?)',batch);batch.clear()
        if when>=end_ps: break
    if batch:
        db.executemany('INSERT INTO playback_progress VALUES(?,?,?,?,?)',batch)
    return result


