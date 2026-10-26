"""FastAPI replay server with non-blocking preprocessing and explicit smoke input."""
import argparse
import asyncio
from contextlib import asynccontextmanager, suppress
import json
import multiprocessing
import os
from pathlib import Path
from queue import Empty
import sqlite3
import time

from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from jobs import import_worker
from prepare import decode_frame

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
    application.state.progress = dict(status='queued',phase='queued',phaseLabel='等待处理',file='',rows=0)
    # Optional throughput rescale for bandwidth/data-size scaled simulations.
    # Only displayed Gbps/capacity values change; utilization stays unchanged.
    try:
        application.state.scale = float(json.loads(manifest.read_text(encoding='utf-8')).get('throughputScale', 1) or 1)
    except (json.JSONDecodeError, OSError):
        application.state.scale = 1
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
            if application.state.progress['status'] in ('ready','error'):
                return
            if not process.is_alive():
                dead_polls += 1
                if dead_polls>=10:
                    application.state.progress = dict(status='error',phaseLabel='处理进程退出',error=f'Importer exited with code {process.exitcode}')
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


app = FastAPI(title='TE / TOE Replay v6', lifespan=lifespan)
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
    return info


@app.get('/api/frames')
def frames(start:float=Query(0,ge=0),end:float=Query(20,ge=0)):
    info = ready()
    if end<=start or end-start>30:
        raise HTTPException(400,'Request a positive interval of at most 30 simulation seconds')
    scale = getattr(app.state, 'scale', 1)
    with connection() as db:
        cases = {}
        for c in info['cases']:
            case_frames = [decode_frame(r[0]) for r in db.execute('SELECT data FROM frames WHERE case_id=? AND t1>? AND t0<? ORDER BY t0',(c['id'],start,end))]
            if scale != 1:
                for frame in case_frames:
                    for edge in frame.get('edges', []):
                        if edge.get('gbps') is not None:
                            edge['gbps'] *= scale
                        if edge.get('capacity') is not None:
                            edge['capacity'] *= scale
            cases[c['id']] = case_frames
    return dict(start=start,end=end,cases=cases)


@app.get('/api/completion')
def completion():
    return {'windowSeconds':.5,'cases':{c['id']:c['completionBins'] for c in ready()['cases']}}


@app.get('/api/paths')
def paths(src:int,dst:int,start:float=Query(0,ge=0),end:float=Query(5,ge=0)):
    info=ready()
    if src==dst or src not in info['topology']['leaves'] or dst not in info['topology']['leaves']:
        raise HTTPException(400,'Choose two different input switches')
    if end<=start or end-start>10:
        raise HTTPException(400,'Path detail requests are limited to 10 simulation seconds')
    scale = getattr(app.state, 'scale', 1)
    result={}
    with connection() as db:
        for case in info['cases']:
            windows={}
            for sample,t0,t1,sn,dn,mid,n in db.execute('SELECT sample,t0,t1,sn,dn,mid,bytes FROM node_paths WHERE case_id=? AND src=? AND dst=? AND t0>=? AND t0<? ORDER BY t0',(case['id'],src,dst,max(0,start-float(case['metadata'].get('sample_interval_ns',500000000))/1e9),end)):
                if t1<=start: continue
                windows.setdefault(str(sample),[]).append(dict(srcNode=sn,dstNode=dn,src=src,dst=dst,transit=mid,payloadBytes=n,gbps=n*8/(t1-t0)/1e9*scale))
            result[case['id']]=windows
    return dict(src=src,dst=dst,start=start,end=end,cases=result)


@app.get('/api/trend')
def trend(src:int,dst:int):
    info = ready()
    if src not in info['topology']['leaves'] or dst not in info['topology']['leaves'] or src==dst:
        raise HTTPException(400,'Choose two different switches from the input topology')
    scale = getattr(app.state, 'scale', 1)
    result = {}
    with connection() as db:
        for case in info['cases']:
            rows = db.execute('SELECT g.t0,l.capacity,l.gbps,l.util,l.queue FROM global_samples g LEFT JOIN link_trends l ON l.case_id=g.case_id AND l.t0=g.t0 AND l.src=? AND l.dst=? WHERE g.case_id=? ORDER BY g.t0',(src,dst,case['id']))
            result[case['id']] = [dict(time=t,capacity=(cap if cap is not None else 0)*scale,gbps=(rate or 0)*scale,utilization=u,queueMB=q) for t,cap,rate,u,q in rows]
    return dict(src=src,dst=dst,cases=result)


@app.get('/api/global-trend')
def global_trend():
    info = ready()
    with connection() as db:
        return {'cases':{c['id']:[dict(time=t,end=end,maxUtilization=u,hotLinks=h,measuredLinks=n) for t,end,u,h,n in db.execute('SELECT t0,t1,max_util,hot,measured FROM global_samples WHERE case_id=? ORDER BY t0',(c['id'],))] for c in info['cases']}}


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
        args.manifest = HERE/'smoke/experiment.json'
        os.environ['REPLAY_ALLOW_SYNTHETIC']='1'
        os.environ['REPLAY_CACHE']=str(HERE/'cache/smoke.sqlite')
        print('SMOKE: synthetic fixture, not ns-3 experiment results.',flush=True)
    if args.manifest is None and 'REPLAY_MANIFEST' not in os.environ:
        parser.error('provide --manifest for actual logs, or explicitly use --smoke')
    if args.manifest:
        os.environ['REPLAY_MANIFEST']=str(args.manifest.resolve())
    if args.cache:
        os.environ['REPLAY_CACHE']=str(args.cache.resolve())
    import uvicorn
    uvicorn.run(app,host=args.host,port=args.port)
