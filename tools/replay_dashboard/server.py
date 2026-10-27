"""FastAPI replay server with non-blocking preprocessing and explicit smoke input."""
import argparse
import asyncio
from contextlib import asynccontextmanager, suppress
import json
import math
import multiprocessing
import os
import sys
from pathlib import Path
from queue import Empty
import sqlite3
import time
from typing import Optional

from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from jobs import import_worker
from prepare import decode_frame
from task_store import task_sources
from queries import interval, progress_rows, completion_trend, global_trend as query_global, link_trend

HERE = Path(__file__).resolve().parent


@asynccontextmanager
async def lifespan(application):
    manifest_value = os.environ.get('REPLAY_MANIFEST')
    if not manifest_value:
        raise RuntimeError('Specify actual experiment logs with --manifest, or explicitly use --smoke')
    manifest = Path(manifest_value).resolve()
    if not manifest.is_file():
        raise RuntimeError(f'Experiment manifest not found: {manifest}')
    cache = Path(os.environ.get('REPLAY_CACHE', HERE/'cache/replay.sqlite')).resolve()
    application.state.cache = cache
    application.state.bootstrap = None
    application.state.started = time.monotonic()
    application.state.fatal_error = None
    application.state.progress = dict(status='queued',phase='queued',phaseLabel='等待处理',file='',rows=0)
    # Optional throughput rescale for bandwidth/data-size scaled simulations.
    # Only displayed Gbps/capacity values change; utilization stays unchanged.
    try:
        application.state.scale = float(json.loads(manifest.read_text(encoding='utf-8')).get('throughputScale', 1))
    except (json.JSONDecodeError, OSError):
        application.state.scale = 1
    if not math.isfinite(application.state.scale) or application.state.scale <= 0:
        raise ValueError('throughputScale must be positive and finite')
    context = multiprocessing.get_context('spawn')
    messages = context.Queue(maxsize=64)
    process = context.Process(target=import_worker,args=(str(manifest),str(cache),messages,os.environ.get('REPLAY_ALLOW_SYNTHETIC')=='1'),daemon=True)
    process.start()

    async def monitor():
        dead_polls = 0
        while True:
            try:
                while True:
                    state = messages.get_nowait()
                    if state['status']=='ready':
                        with sqlite3.connect(cache) as db:
                            application.state.bootstrap = json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
                    application.state.progress = state
            except Empty:
                pass
            except Exception as error:
                application.state.progress = dict(status='error',phaseLabel='缓存读取失败',error=str(error))
            status = application.state.progress['status']
            if status == 'error':
                application.state.fatal_error = application.state.progress.get('error', 'Replay preparation failed')
                server = getattr(application.state, 'uvicorn_server', None)
                if server is not None:
                    server.should_exit = True
                return
            if status == 'ready':
                return
            if not process.is_alive():
                dead_polls += 1
                if dead_polls >= 10:
                    error = f'Importer exited with code {process.exitcode}'
                    application.state.progress = dict(status='error',phaseLabel='处理进程退出',error=error)
                    application.state.fatal_error = error
                    server = getattr(application.state, 'uvicorn_server', None)
                    if server is not None:
                        server.should_exit = True
                    return
            await asyncio.sleep(.2)
    watcher = asyncio.create_task(monitor())
    try:
        yield
    finally:
        watcher.cancel()
        with suppress(asyncio.CancelledError):
            await watcher
        if process.is_alive():
            process.terminate()
        await asyncio.to_thread(process.join,2)
        if process.is_alive():
            process.kill()
            await asyncio.to_thread(process.join,1)
        messages.close()


app = FastAPI(title='TE / TOE Replay v8', lifespan=lifespan)
app.add_middleware(GZipMiddleware, minimum_size=1000)
app.mount('/static', StaticFiles(directory=HERE/'static'),name='static')


def ready():
    state = app.state.progress
    if state['status']!='ready' or app.state.bootstrap is None:
        raise HTTPException(503 if state['status']=='error' else 202, detail=state)
    return app.state.bootstrap


def connection():
    ready()
    return sqlite3.connect(f'file:{app.state.cache}?mode=ro', uri=True)


@app.get('/')
def index():
    return FileResponse(HERE/'static/index.html')


@app.get('/api/status')
def status():
    return {**app.state.progress,'elapsedSeconds':round(time.monotonic()-app.state.started,1)}


@app.get('/api/health')
def health():
    info = app.state.bootstrap
    return dict(status=app.state.progress['status'],synthetic=info['synthetic'] if info else None)


@app.get('/api/bootstrap')
def bootstrap():
    info = ready()
    scale = getattr(app.state, 'scale', 1)
    if scale != 1:
        info = dict(info)
        info['rateScaleGbps'] = (info.get('rateScaleGbps') or 1) * scale
        info['throughputScale'] = scale
        info['cases'] = [dict(c,
                          storageThroughputGbps=(c['storageThroughputGbps']*scale
                              if c.get('storageThroughputGbps') is not None else None),
                          topologyStates=[dict(s,edges=[dict(e,capacity=e['capacity']*scale)
                              for e in s['edges']]) for s in c['topologyStates']],
                          finalEdges=[dict(e,capacity=e['capacity']*scale) for e in c['finalEdges']])
                         for c in info['cases']]
    return info


@app.get('/api/frames')
def frames(start:float=Query(0,ge=0),end:float=Query(.5,ge=0)):
    info = ready()
    if not math.isfinite(start) or not math.isfinite(end) or end<=start or end-start>2:
        raise HTTPException(400,'Request a positive interval of at most 2 simulation seconds')
    scale = getattr(app.state, 'scale', 1)
    with connection() as db:
        cases = {}; progress = {}
        for c in info['cases']:
            case_frames = [decode_frame(r[0]) for r in db.execute('SELECT data FROM frames WHERE case_id=? AND t1>? AND t0<? ORDER BY t0',(c['id'],start,end))]
            if scale != 1:
                for frame in case_frames:
                    if frame['global'].get('storageDecodeGbps') is not None:
                        frame['global']['storageDecodeGbps'] *= scale
                    for edge in frame.get('edges', []):
                        if edge.get('gbps') is not None:
                            edge['gbps'] *= scale
                        if edge.get('capacity') is not None:
                            edge['capacity'] *= scale
            cases[c['id']] = case_frames
            progress[c['id']] = progress_rows(db,c['id'],start,end)
    return dict(start=start,end=end,cases=cases,progress=progress)


@app.get('/api/completion')
def completion(start:Optional[float]=Query(None,ge=0),end:Optional[float]=Query(None,ge=0),points:int=Query(800,ge=2,le=2000)):
    info = ready()
    try: start,end = interval(info,start,end)
    except ValueError as error: raise HTTPException(400,str(error))
    with connection() as db:
        return dict(windowSeconds=info['windowSeconds'],start=start,end=end,
                    cases={c['id']:completion_trend(db,c['id'],start,end,points) for c in info['cases']})


@app.get('/api/paths')
def paths(src:int,dst:int,start:float=Query(0,ge=0),end:float=Query(.5,ge=0)):
    info=ready()
    if src==dst or src not in info['topology']['leaves'] or dst not in info['topology']['leaves']:
        raise HTTPException(400,'Choose two different input switches')
    if not math.isfinite(start) or not math.isfinite(end) or end<=start or end-start>2:
        raise HTTPException(400,'Path detail requests are limited to 2 simulation seconds')
    scale = getattr(app.state, 'scale', 1)
    result={}
    with connection() as db:
        for case in info['cases']:
            windows={}
            for sample,t0,t1,sn,dn,mid,n in db.execute('SELECT sample,t0,t1,sn,dn,mid,bytes FROM node_paths WHERE case_id=? AND src=? AND dst=? AND t0>=? AND t0<? ORDER BY t0',(case['id'],src,dst,max(0,start-info['windowSeconds']-1e-9),end)):
                if t1<=start: continue
                windows.setdefault(str(sample),[]).append(dict(srcNode=sn,dstNode=dn,src=src,dst=dst,transit=mid,payloadBytes=n,gbps=n*8/(t1-t0)/1e9*scale))
            result[case['id']]=windows
    return dict(src=src,dst=dst,start=start,end=end,cases=result)


@app.get('/api/trend')
def trend(src:int,dst:int,start:Optional[float]=Query(None,ge=0),end:Optional[float]=Query(None,ge=0),points:int=Query(800,ge=2,le=2000)):
    info = ready()
    if src not in info['topology']['leaves'] or dst not in info['topology']['leaves'] or src==dst:
        raise HTTPException(400,'Choose two different switches from the input topology')
    scale = getattr(app.state, 'scale', 1)
    try: start,end = interval(info,start,end)
    except ValueError as error: raise HTTPException(400,str(error))
    result = {}
    with connection() as db:
        for case in info['cases']:
            result[case['id']] = link_trend(db,case['id'],src,dst,start,end,points,scale)
    return dict(src=src,dst=dst,cases=result)


@app.get('/api/global-trend')
def global_trend(start:Optional[float]=Query(None,ge=0),end:Optional[float]=Query(None,ge=0),points:int=Query(800,ge=2,le=2000)):
    info = ready()
    try: start,end = interval(info,start,end)
    except ValueError as error: raise HTTPException(400,str(error))
    scale = getattr(app.state,'scale',1)
    with connection() as db:
        return dict(start=start,end=end,cases={c['id']:query_global(db,c['id'],start,end,points,scale) for c in info['cases']})


if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument('--manifest',type=Path,help='Actual-log experiment.json (or set REPLAY_MANIFEST)')
    inputs.add_argument('--smoke',action='store_true',help='Explicitly select bundled synthetic smoke logs')
    parser.add_argument('--host',default='127.0.0.1')
    parser.add_argument('--port',type=int,default=8000)
    parser.add_argument('--cache',type=Path)
    args = parser.parse_args()
    if args.smoke:
        args.manifest = HERE/'smoke10ms/experiment.json'
        os.environ['REPLAY_ALLOW_SYNTHETIC']='1'
        os.environ['REPLAY_CACHE']=str(HERE/'cache/smoke-v8.sqlite')
        print('SMOKE: synthetic fixture, not ns-3 experiment results.',flush=True)
    if args.manifest is None and 'REPLAY_MANIFEST' not in os.environ:
        parser.error('provide --manifest for actual logs, or explicitly use --smoke')
    if args.manifest:
        os.environ['REPLAY_MANIFEST']=str(args.manifest.resolve())
    if args.cache:
        os.environ['REPLAY_CACHE']=str(args.cache.resolve())
    manifest_path = Path(os.environ['REPLAY_MANIFEST']).resolve()
    try:
        manifest_config = json.loads(manifest_path.read_text(encoding='utf-8'))
        cases = manifest_config.get('cases')
        if not isinstance(cases, list) or not cases:
            raise ValueError('Experiment manifest must define at least one case')
        for case in cases:
            task_sources(manifest_path.parent, case)
    except Exception as error:
        print(f'ERROR: Replay requires a usable output/task_statistics.csv for every case: {error}', file=sys.stderr, flush=True)
        raise SystemExit(2)
    import uvicorn
    server = uvicorn.Server(uvicorn.Config(app, host=args.host, port=args.port))
    app.state.uvicorn_server = server
    server.run()
    fatal_error = getattr(app.state, 'fatal_error', None)
    if fatal_error:
        print(f'ERROR: Replay preparation failed: {fatal_error}', file=sys.stderr, flush=True)
        raise SystemExit(1)
