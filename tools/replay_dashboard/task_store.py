"""Bounded task import and exact disk-backed cohort statistics (no task arrays)."""
import gzip
import math
import re
from decimal import Decimal
from pathlib import Path
from topology import csv_rows

BATCH = 20000
PS = 10**12


def picoseconds(value, scale=1000):
    return int(Decimal(str(value)) * scale)


def existing(path):
    path = Path(path)
    return path if path.exists() else Path(str(path) + '.gz')


def task_sources(root, config):
    path = (root / config['path']).resolve()
    if config.get('task_events'):
        return 'events', [existing(root / config['task_events'])]
    if config.get('task_statistics'):
        return 'statistics', [existing(root / config['task_statistics'])]
    events = existing(path / 'replay_task_events.csv')
    # An optional, header-only replay file must not hide the native task trace.
    if events.exists() and next(csv_rows(events),None) is not None:
        return 'events', [events]
    directory = (root / config['task_trace_directory']).resolve() if config.get('task_trace_directory') else path.parent / 'runlog'
    files = sorted(directory.glob('TaskTrace_node_*.tr*'))
    if not files:
        raise ValueError(f'{config["id"]}: no task data; specify task_events, task_statistics, or task_trace_directory')
    return 'native', files


def create_tables(db):
    db.executescript('''
        CREATE TABLE workload(task_id INTEGER PRIMARY KEY,src INTEGER,dst INTEGER,size INTEGER,op TEXT);
        CREATE TABLE tasks(case_id TEXT,task_id INTEGER,start_ps INTEGER,end_ps INTEGER,kind TEXT,node INTEGER,
                           PRIMARY KEY(case_id,task_id)) WITHOUT ROWID;
        CREATE TABLE completion_bins(case_id TEXT,time_ps INTEGER,started INTEGER,completed INTEGER,ended_started INTEGER,
                                     PRIMARY KEY(case_id,time_ps)) WITHOUT ROWID;
        CREATE TABLE playback_progress(case_id TEXT,time_ps INTEGER,started INTEGER,completed INTEGER,running INTEGER,
                                       PRIMARY KEY(case_id,time_ps)) WITHOUT ROWID;
        CREATE TABLE cohort(task_id INTEGER PRIMARY KEY);
        CREATE TABLE fct_values(case_id TEXT,fct_ps INTEGER,task_id INTEGER,
                                PRIMARY KEY(case_id,fct_ps,task_id)) WITHOUT ROWID;
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


def import_tasks(db, root, config, reporter):
    case = config['id']
    source, files = task_sources(root, config)
    statement = '''INSERT INTO tasks VALUES(?,?,?,?,?,?) ON CONFLICT(case_id,task_id) DO UPDATE SET
        start_ps=CASE WHEN tasks.start_ps IS NULL THEN excluded.start_ps WHEN excluded.start_ps IS NULL THEN tasks.start_ps ELSE MIN(tasks.start_ps,excluded.start_ps) END,
        end_ps=CASE WHEN tasks.end_ps IS NULL THEN excluded.end_ps WHEN excluded.end_ps IS NULL THEN tasks.end_ps ELSE MAX(tasks.end_ps,excluded.end_ps) END,
        node=CASE WHEN tasks.node IS NULL OR (tasks.node>=0 AND excluded.node>=0 AND tasks.node!=excluded.node) THEN NULL ELSE tasks.node END'''
    def records():
        for path in files:
            if source == 'native':
                pattern = re.compile(r'^\[([\d.eE+-]+)us\].*?\b(MEM Task|WQE) (?!Segment )(Starts|Completes)\b.*?taskId:\s*(\d+)')
                node = int(re.search(r'node_(\d+)',path.name)[1])
                for line in reporter.lines(path):
                    # Per-segment Sends/Completes are not task start/end events.
                    # Skip them before the strict task-id format check.
                    if 'Segment' in line:
                        continue
                    match = pattern.search(line)
                    if not match:
                        if 'taskId:' in line:
                            raise ValueError(f'Unrecognized task trace row: {line[:160]}')
                        continue
                    when, kind, event, tid = match.groups()
                    when = picoseconds(when, 10**6)
                    yield case,int(tid),when if event=='Starts' else None,when if event=='Completes' else None,kind,node
            else:
                for row in reporter.rows(path):
                    if source == 'events':
                        if row.get('case_id',case) != case or row['event'] not in ('start','complete'):
                            raise ValueError(f'{case}: invalid task event')
                        when = picoseconds(row['time_ns'])
                        yield case,int(row['task_id']),when if row['event']=='start' else None,when if row['event']=='complete' else None,row.get('task_kind','unknown'),int(row.get('node_id',-1))
                    else:
                        # Explicit schema avoids guessing units from column names.
                        yield case,int(row['task_id']),picoseconds(row['start_ns']) if row['start_ns'] else None,picoseconds(row['end_ns']) if row['end_ns'] else None,row.get('task_kind','unknown'),int(row.get('node_id',-1))
    batch=[]
    for record in records():
        batch.append(record)
        if len(batch)>=BATCH:
            db.executemany(statement,batch);batch.clear();db.commit()
    if batch: db.executemany(statement,batch)
    db.commit()
    if db.execute('SELECT 1 FROM tasks WHERE case_id=? AND node IS NULL LIMIT 1',(case,)).fetchone():
        raise ValueError(f'{case}: task id reused by multiple source nodes')
    if db.execute('SELECT 1 FROM tasks WHERE case_id=? AND (start_ps<0 OR end_ps<0 OR end_ps<start_ps) LIMIT 1',(case,)).fetchone():
        raise ValueError(f'{case}: invalid task timestamps')
    if db.execute('SELECT 1 FROM tasks t LEFT JOIN workload w USING(task_id) WHERE t.case_id=? AND w.task_id IS NULL LIMIT 1',(case,)).fetchone():
        raise ValueError(f'{case}: task id is absent from workload')
    completed, started, ended_started = db.execute('SELECT SUM(end_ps IS NOT NULL),SUM(start_ps IS NOT NULL),SUM(end_ps IS NOT NULL AND start_ps IS NOT NULL) FROM tasks WHERE case_id=?',(case,)).fetchone()
    total=db.execute('SELECT COUNT(*) FROM workload').fetchone()[0]
    return dict(totalTasks=total,completedTasks=completed or 0,pendingTasks=total-(started or 0),taskSource=source)


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


def common_quantiles(db,cases,config,reporter):
    baseline=next((c for c in cases if c['mode']=='ecmp'),None)
    if baseline is None: raise ValueError('An ECMP baseline is required')
    reporter.stage('cohort','磁盘匹配共同完成任务集')
    db.execute('INSERT INTO cohort SELECT task_id FROM tasks WHERE start_ps IS NOT NULL AND end_ps IS NOT NULL GROUP BY task_id HAVING COUNT(*)=?',(len(cases),))
    settings=config.get('statistics',{})
    clauses=[]; args=[baseline['id']]
    for key,operator in (('baseline_start_seconds','>='),('baseline_end_seconds','<')):
        if key in settings:
            clauses.append(f't.start_ps {operator} ?');args.append(picoseconds(settings[key],PS))
    if 'op_types' in settings:
        db.execute('CREATE TEMP TABLE selected_ops(op TEXT PRIMARY KEY)')
        db.executemany('INSERT INTO selected_ops VALUES(?)',[(v,) for v in settings['op_types']])
        clauses.append('w.op IN (SELECT op FROM selected_ops)')
    if 'task_ids' in settings:
        db.execute('CREATE TEMP TABLE selected_ids(task_id INTEGER PRIMARY KEY)')
        db.executemany('INSERT INTO selected_ids VALUES(?)',[(int(v),) for v in settings['task_ids']])
        clauses.append('t.task_id IN (SELECT task_id FROM selected_ids)')
    if clauses:
        db.execute('DELETE FROM cohort WHERE task_id NOT IN (SELECT t.task_id FROM tasks t JOIN workload w USING(task_id) WHERE t.case_id=? AND '+ ' AND '.join(clauses)+')',args)
    n=db.execute('SELECT COUNT(*) FROM cohort').fetchone()[0]
    values={}
    for case in cases:
        cid=case['id'];reporter.stage('fct-sort','磁盘排序并计算精确 FCT 分位数',cid)
        db.execute('INSERT INTO fct_values SELECT case_id,end_ps-start_ps,t.task_id FROM tasks t JOIN cohort c USING(task_id) WHERE case_id=?',(cid,))
        ranks={}
        for label,q in (('p50',.5),('p90',.9),('p99',.99)):
            x=(n-1)*q if n else 0;ranks[label]=(int(x),min(int(x)+1,n-1),x-int(x))
        wanted={r for lo,hi,_ in ranks.values() for r in (lo,hi)} if n else set()
        found={};max_rank=max(wanted,default=-1)
        for rank,(value,) in enumerate(db.execute('SELECT fct_ps FROM fct_values WHERE case_id=? ORDER BY fct_ps,task_id',(cid,))):
            if rank in wanted:found[rank]=value
            if rank%200000==0:reporter.update(rows=rank+1)
            if rank>=max_rank:break
        values[cid]={label:((found[lo]*(1-frac)+found[hi]*frac)/PS if n else None) for label,(lo,hi,frac) in ranks.items()}
        db.commit()
    for case in cases:
        case['quantiles']={}
        for label in ('p50','p90','p99'):
            base=values[baseline['id']][label];value=values[case['id']][label]
            case['quantiles'][label]=dict(seconds=value,improvement=(base-value)/base*100 if base and value is not None else None)
        case['p99']=case['quantiles']['p99']['seconds'];case['improvement']=case['quantiles']['p99']['improvement']
    return n,baseline
