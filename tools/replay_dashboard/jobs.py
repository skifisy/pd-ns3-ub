"""Isolated importer process; only small progress messages cross the queue."""
import json
from queue import Empty, Full
import sqlite3

from prepare import prepare


def import_worker(manifest, cache, messages, allow_synthetic):
    last = {}
    def send(state):
        nonlocal last
        last = state
        if state.get('status') in ('ready','error'):
            messages.put(state,timeout=5)
            return
        try:
            messages.put_nowait(state)
        except Full:
            try:
                messages.get_nowait()
            except Empty:
                pass
            try:
                messages.put_nowait(state)
            except Full:
                pass
    try:
        prepare(manifest, cache, progress=send)
        with sqlite3.connect(cache) as db:
            info = json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
        if info['synthetic'] and not allow_synthetic:
            raise ValueError('Synthetic input rejected. Explicitly use --smoke for the bundled fixture.')
        print('Replay preparation ready: 100% (reading, statistics and indexes complete)',flush=True)
        send(dict(last,status='ready',phase='ready',phaseLabel='数据就绪',synthetic=info['synthetic']))
    except Exception as error:
        send(dict(last,status='error',phaseLabel='数据处理失败',error=str(error)))
