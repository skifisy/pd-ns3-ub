#!/usr/bin/env python3
"""Serve prepared Leaf-pair data; viewing never rescans CSV files."""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import sqlite3

from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

try:
    from .prepare import SCHEMA
    from .queries import series
except ImportError:
    from prepare import SCHEMA
    from queries import series

HERE = Path(__file__).resolve().parent


def create_app(data):
    cache = Path(data).resolve()
    if cache.is_dir():
        cache = cache / "leaf-pairs.sqlite"
    if not cache.is_file():
        raise ValueError(f"Cache not found: {cache}; run prepare.py first")

    @contextmanager
    def connection():
        db = sqlite3.connect(f"{cache.as_uri()}?mode=ro", uri=True)
        try:
            yield db
        finally:
            db.close()

    with connection() as db:
        schema = db.execute("SELECT value FROM metadata WHERE key='schema'").fetchone()
        if schema != (str(SCHEMA),):
            raise ValueError("Incompatible cache; rerun prepare.py")
        info = json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
    # Keep the open cache identity stable; restart after replacing the prepared file.
    identity = (cache.stat().st_ino, cache.stat().st_size, cache.stat().st_mtime_ns)
    app = FastAPI(title="P–S / D–S 流量分析")
    app.add_middleware(GZipMiddleware, minimum_size=1000)
    app.mount("/static", StaticFiles(directory=HERE / "static"), name="static")
    vendor = HERE.parent / "replay_dashboard" / "static" / "vendor"
    app.mount("/vendor", StaticFiles(directory=vendor), name="vendor")

    @app.get("/")
    def index():
        return FileResponse(HERE / "static" / "index.html")

    @app.get("/api/bootstrap")
    def bootstrap():
        return info

    @app.get("/api/series")
    def query(start: float, end: float, points: int = Query(400, ge=16, le=2000),
              metric: str = "payload", pairs: str | None = None):
        if (cache.stat().st_ino, cache.stat().st_size, cache.stat().st_mtime_ns) != identity:
            raise HTTPException(409, "Cache changed; restart the server")
        try:
            with connection() as db:
                return series(db, info, start, end, points, metric, pairs.split(",") if pairs is not None else None)
        except ValueError as error:
            raise HTTPException(400, str(error)) from error

    return app


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", required=True, help="Prepared directory or leaf-pairs.sqlite")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    import uvicorn
    uvicorn.run(create_app(args.data), host=args.host, port=args.port)
