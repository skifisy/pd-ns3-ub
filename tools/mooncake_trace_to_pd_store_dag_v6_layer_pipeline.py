#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Mooncake trace -> Store-mediated P/D Task DAG (v6 layer-pipelined)

New P/D model
-------------
P nodes: 0..11
D nodes: 12..15
No P/D affinity groups.

For each request:
1. HIT prefix blocks:
      Storage -> P        (PREFIX_READ)
2. After ALL HIT reads finish, P computes MISS KV layer-by-layer.
3. As soon as layer i finishes computing, P immediately sends that layer's
   full-prefix KV to Storage (PREFIX_STORE_WRITE).  Computation of layer i+1
   does NOT wait for layer i network transfer, so compute and network overlap.
4. After ALL layer writes finish:
      wait storage commit/access delay
5. Storage -> D transfers the FULL prefix KV
      (STORAGE_TO_D; unchanged, still block-level in this v6)
6. After all Storage->D flows finish:
      Decode on D
7. After Decode:
      D -> Storage        (DECODE_WRITE)

This is an event-driven DAG representation.
Dependency-driven tasks intentionally have no fixed start_time.
Their real start time must be determined at ns-3 runtime from predecessor
completion events.

Cache semantics
---------------
Hit/miss is still classified from trace history:
- infinite cache
- cold start unless warm-up is used
- same-timestamp requests see the same cache snapshot
"""

import argparse
import csv
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def stable_hash_int(*parts) -> int:
    text = "|".join(str(x) for x in parts)
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big", signed=False)


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def load_trace(path, max_requests=None):
    reqs = []
    with open(path, "r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue

            obj = json.loads(line)
            reqs.append({
                "request_id": len(reqs),
                "line_no": line_no,
                "timestamp_ms": int(obj.get("timestamp", 0)),
                "input_length": int(obj.get("input_length", 0)),
                "output_length": int(obj.get("output_length", 0)),
                "hash_ids": list(obj.get("hash_ids", [])),
            })

            if max_requests is not None and len(reqs) >= max_requests:
                break

    if not reqs:
        raise ValueError("Trace is empty.")

    reqs.sort(key=lambda r: (r["timestamp_ms"], r["request_id"]))
    return reqs


def effective_block_tokens(input_length, num_hashes, block_index, block_tokens):
    if num_hashes <= 0:
        return 0
    if block_index < num_hashes - 1:
        return block_tokens

    remain = input_length - block_tokens * (num_hashes - 1)
    if remain <= 0:
        return block_tokens
    return min(remain, block_tokens)


def join_deps(ids):
    return ";".join(str(x) for x in ids)


# ---------------------------------------------------------------------------
# Model / placement
# ---------------------------------------------------------------------------

class Model:
    def __init__(self, cfg):
        model = cfg["model"]
        timing = cfg["timing"]
        compute = cfg["compute"]
        storage = cfg["storage"]
        delivery = cfg["kv_delivery"]

        self.kv_bytes_per_token = int(model["kv_bytes_per_token"])
        self.block_tokens = int(model["block_tokens"])
        self.num_layers = int(model.get("num_layers", 78))

        pipeline = delivery.get("layer_pipeline", {})
        self.layer_pipeline_enabled = bool(pipeline.get("enabled", True))
        self.split_prefix_store_write_by_layer = bool(
            pipeline.get("split_prefix_store_write_by_layer", True)
        )
        self.storage_to_d_streaming = bool(
            pipeline.get("storage_to_d_streaming", False)
        )

        self.prefill_us_per_token = float(timing["prefill_us_per_token"])
        self.decode_us_per_token = float(timing["decode_us_per_token"])
        self.storage_latency_us = int(timing["storage_latency_us"])

        self.p_nodes = list(compute["p_nodes"])
        self.d_nodes = list(compute["d_nodes"])
        self.server_ports = list(compute["server_ports"])
        self.p_hash_blocks = int(compute["p_placement"]["hash_blocks"])
        self.p_exclude_hash_ids = set(
            compute["p_placement"].get("exclude_hash_ids", [])
        )

        self.storage_nodes = list(storage["nodes"])
        self.storage_ports = list(storage["ports"])

        self.rewrite_full_prefix = bool(delivery["rewrite_full_prefix"])
        self.storage_to_d_full_prefix = bool(
            delivery["storage_to_d_full_prefix"]
        )

        if not self.p_nodes or not self.d_nodes:
            raise ValueError("compute.p_nodes and compute.d_nodes must be non-empty")
        if not self.server_ports:
            raise ValueError("compute.server_ports must be non-empty")
        if not self.storage_nodes or not self.storage_ports:
            raise ValueError("storage nodes/ports must be non-empty")
        if self.num_layers <= 0:
            raise ValueError("model.num_layers must be > 0")
        if not self.layer_pipeline_enabled:
            raise ValueError("v6 requires kv_delivery.layer_pipeline.enabled=true")
        if not self.split_prefix_store_write_by_layer:
            raise ValueError(
                "v6 requires split_prefix_store_write_by_layer=true"
            )
        if self.storage_to_d_streaming:
            raise ValueError(
                "this v6 keeps Storage->D block-level; set storage_to_d_streaming=false"
            )

        if not self.storage_to_d_full_prefix:
            raise ValueError(
                "v6 implements full-prefix Storage->D transfer"
            )

    def prefill_duration_us(self, tokens):
        return int(round(tokens * self.prefill_us_per_token))

    def layer_compute_schedule_us(self, tokens):
        """
        Split the old whole-model Prefill duration across layers while preserving
        the exact total duration after integer rounding.

        Returns a list of (layer_duration_us, cumulative_ready_us).
        Layer i can start its network write at cumulative_ready_us after the
        Prefill base time.  It does not wait for layer i-1's network transfer.
        """
        total = self.prefill_duration_us(tokens)
        out = []
        prev = 0
        for layer in range(self.num_layers):
            ready = int(round(total * (layer + 1) / self.num_layers))
            out.append((ready - prev, ready))
            prev = ready
        return out

    def layer_bytes(self, total_bytes, layer_index):
        """Split bytes exactly across num_layers; all pieces sum to total_bytes."""
        q, r = divmod(int(total_bytes), self.num_layers)
        return q + (1 if layer_index < r else 0)

    def decode_duration_us(self, tokens):
        return int(round(tokens * self.decode_us_per_token))

    def prefix_key(self, req):
        ids = [
            h for h in req["hash_ids"]
            if h not in self.p_exclude_hash_ids
        ][:self.p_hash_blocks]
        return tuple(ids) if ids else ("request", req["request_id"])

    def choose_p(self, req):
        key = self.prefix_key(req)
        idx = stable_hash_int("p-node", *key) % len(self.p_nodes)
        return self.p_nodes[idx], key

    def choose_d(self, req):
        # No affinity: independently spread requests over all D nodes.
        idx = stable_hash_int("d-node", req["request_id"]) % len(self.d_nodes)
        return self.d_nodes[idx]

    def choose_storage(self, tag):
        idx = stable_hash_int("storage", tag) % len(self.storage_nodes)
        return self.storage_nodes[idx]

    def choose_storage_port(self, tag):
        idx = stable_hash_int(
            "storage-port", tag
        ) % len(self.storage_ports)
        return self.storage_ports[idx]

    def choose_server_port(self, direction, request_id, tag):
        idx = stable_hash_int(
            direction, request_id, tag
        ) % len(self.server_ports)
        return self.server_ports[idx]


# ---------------------------------------------------------------------------
# DAG
# ---------------------------------------------------------------------------

TASK_FIELDS = [
    "task_id",
    "request_id",
    "phase",
    "task_class",          # NETWORK / COMPUTE / TIMER
    "task_type",
    "release_time_us",     # absolute release; blank if dependency driven
    "duration_us",         # COMPUTE/TIMER only
    "src_node",
    "src_port",
    "dst_node",
    "dst_port",
    "bytes",
    "hash_id",
    "block_index",
    "block_tokens",
    "layer_index",
    "layer_count",
    "layer_ready_offset_us",
    "p_node",
    "d_node",
    "depends_on",
    "comment",
]


def make_task(
    task_id,
    request_id,
    phase,
    task_class,
    task_type,
    release_time_us="",
    duration_us=0,
    src_node="",
    src_port="",
    dst_node="",
    dst_port="",
    num_bytes=0,
    hash_id="",
    block_index="",
    block_tokens=0,
    layer_index="",
    layer_count="",
    layer_ready_offset_us=0,
    p_node="",
    d_node="",
    depends_on=None,
    comment="",
):
    return {
        "task_id": task_id,
        "request_id": request_id,
        "phase": phase,
        "task_class": task_class,
        "task_type": task_type,
        "release_time_us": release_time_us,
        "duration_us": duration_us,
        "src_node": src_node,
        "src_port": src_port,
        "dst_node": dst_node,
        "dst_port": dst_port,
        "bytes": int(num_bytes),
        "hash_id": hash_id,
        "block_index": block_index,
        "block_tokens": block_tokens,
        "layer_index": layer_index,
        "layer_count": layer_count,
        "layer_ready_offset_us": int(layer_ready_offset_us),
        "p_node": p_node,
        "d_node": d_node,
        "depends_on": join_deps(depends_on or []),
        "comment": comment,
    }


def classify_prefix(req, seen_snapshot, model):
    blocks = []
    cached_tokens = 0
    miss_tokens = 0

    for idx, h in enumerate(req["hash_ids"]):
        tok = effective_block_tokens(
            req["input_length"],
            len(req["hash_ids"]),
            idx,
            model.block_tokens,
        )
        hit = h in seen_snapshot

        blocks.append({
            "block_index": idx,
            "hash_id": h,
            "tokens": tok,
            "hit": hit,
        })

        if hit:
            cached_tokens += tok
        else:
            miss_tokens += tok

    return blocks, cached_tokens, miss_tokens


def build_formal_request_tasks(
    req,
    model,
    seen_snapshot,
    next_task_id,
):
    """
    Build one formal request DAG.

    Requested serialized prefix path:
        HIT Storage->P
          -> MISS Prefill
          -> FULL PREFIX P->Storage
          -> storage commit/access delay
          -> FULL PREFIX Storage->D
          -> Decode
          -> D->Storage decode write
    """
    arrival_us = req["timestamp_ms"] * 1000
    p_node, prefix_key = model.choose_p(req)
    d_node = model.choose_d(req)

    blocks, cached_tokens, miss_tokens = classify_prefix(
        req, seen_snapshot, model
    )

    tasks = []
    read_ids = []
    full_write_ids = []
    storage_to_d_ids = []
    new_hashes = [b["hash_id"] for b in blocks if not b["hit"]]

    # ------------------------------------------------------------------
    # Phase 1: HIT blocks Storage -> P
    # ------------------------------------------------------------------
    for b in blocks:
        if not b["hit"]:
            continue

        h = b["hash_id"]
        tok = b["tokens"]
        nbytes = tok * model.kv_bytes_per_token

        s_node = model.choose_storage(h)
        s_port = model.choose_storage_port(h)
        p_port = model.choose_server_port(
            "prefix-read-p", req["request_id"], h
        )

        tid = next_task_id
        next_task_id += 1
        read_ids.append(tid)

        tasks.append(make_task(
            task_id=tid,
            request_id=req["request_id"],
            phase="FORMAL",
            task_class="NETWORK",
            task_type="PREFIX_READ",
            release_time_us=arrival_us + model.storage_latency_us,
            src_node=s_node,
            src_port=s_port,
            dst_node=p_node,
            dst_port=p_port,
            num_bytes=nbytes,
            hash_id=h,
            block_index=b["block_index"],
            block_tokens=tok,
            p_node=p_node,
            d_node=d_node,
            depends_on=[],
            comment="HIT: Storage -> P. Prefill waits for all HIT reads.",
        ))

    # ------------------------------------------------------------------
    # Phase 2/3: Layer-pipelined Prefill + P -> Storage writes
    #
    # Important runtime semantics:
    #   - all HIT reads are still a barrier before Prefill starts;
    #   - layer i becomes ready after cumulative layer compute time;
    #   - its network write starts immediately at that point;
    #   - compute of layer i+1 is independent of completion of layer i write.
    #
    # Therefore each full-prefix block is split into num_layers network flows.
    # For the current GLM-5.1 numbers:
    #   2.4375 GiB / 78 = 32 MiB per full 512-token block per layer.
    # ------------------------------------------------------------------
    layer_schedule = model.layer_compute_schedule_us(miss_tokens)
    prefill_layer_ids = []

    # Formal COMPUTE tasks make the intermediate DAG describe the intended
    # sequential layer compute timeline.  The native traffic converter later
    # folds these compute times into per-layer network-task delays.
    if miss_tokens > 0:
        prev_compute_id = None
        for layer_idx, (layer_duration_us, layer_ready_us) in enumerate(layer_schedule):
            tid = next_task_id
            next_task_id += 1
            prefill_layer_ids.append(tid)

            if layer_idx == 0:
                deps = list(read_ids)
                release = arrival_us if not read_ids else ""
            else:
                deps = [prev_compute_id]
                release = ""

            tasks.append(make_task(
                task_id=tid,
                request_id=req["request_id"],
                phase="FORMAL",
                task_class="COMPUTE",
                task_type="PREFILL_LAYER",
                release_time_us=release,
                duration_us=layer_duration_us,
                src_node=p_node,
                dst_node=p_node,
                block_tokens=miss_tokens,
                layer_index=layer_idx,
                layer_count=model.num_layers,
                layer_ready_offset_us=layer_ready_us,
                p_node=p_node,
                d_node=d_node,
                depends_on=deps,
                comment=(
                    f"Prefill layer {layer_idx}/{model.num_layers - 1}; "
                    f"MISS tokens={miss_tokens}; cumulative ready={layer_ready_us}us."
                ),
            ))
            prev_compute_id = tid

    # P writes only newly generated MISS KV.
    #
    # HIT blocks already exist in Storage and do not need rewrite.
    # Therefore:
    #
    #   Storage -> P : read HIT KV
    #   P compute    : generate MISS KV
    #   P -> Storage : write MISS KV only
    #
    # This avoids unnecessary KV rewrite traffic.

    for b in blocks:

        if model.rewrite_full_prefix:
            write_block = True
        else:
            write_block = not b["hit"]

        if not write_block:
            continue
        h = b["hash_id"]
        tok = b["tokens"]
        block_total_bytes = tok * model.kv_bytes_per_token

        s_node = model.choose_storage(h)
        s_port = model.choose_storage_port(h)

        for layer_idx in range(model.num_layers):
            layer_bytes = model.layer_bytes(block_total_bytes, layer_idx)
            if layer_bytes <= 0:
                continue

            layer_ready_us = layer_schedule[layer_idx][1]
            formal_deps = [prefill_layer_ids[layer_idx]]

            # No-read (cold) requests can use an absolute release timestamp.
            # Partial/all-hit requests remain dependency-driven on the read phase.
            release_time_us = (
                arrival_us + layer_ready_us if not read_ids else ""
            )

            p_port = model.choose_server_port(
                "miss-kv-write-p",
                req["request_id"],
                h
            )

            tid = next_task_id
            next_task_id += 1
            full_write_ids.append(tid)

            tasks.append(make_task(
                task_id=tid,
                request_id=req["request_id"],
                phase="FORMAL",
                task_class="NETWORK",
                task_type="PREFIX_STORE_WRITE",
                release_time_us=release_time_us,
                src_node=p_node,
                src_port=p_port,
                dst_node=s_node,
                dst_port=s_port,
                num_bytes=layer_bytes,
                hash_id=h,
                block_index=b["block_index"],
                block_tokens=tok,
                layer_index=layer_idx,
                layer_count=model.num_layers,
                layer_ready_offset_us=layer_ready_us,
                p_node=p_node,
                d_node=d_node,
                depends_on=formal_deps,
                comment=(
                    f"P -> Storage layer-pipelined MISS KV write; "
                    f"layer={layer_idx}/{model.num_layers - 1}, "
                    f"ready_offset={layer_ready_us}us."
                )
            ))

    # ------------------------------------------------------------------
    # Phase 4: Storage commit/access barrier.
    # All prefix writes must finish, then wait 150 us before Storage->D.
    # ------------------------------------------------------------------
    store_commit_id = None
    if full_write_ids:
        store_commit_id = next_task_id
        next_task_id += 1

        tasks.append(make_task(
            task_id=store_commit_id,
            request_id=req["request_id"],
            phase="FORMAL",
            task_class="TIMER",
            task_type="PREFIX_STORE_COMMIT",
            duration_us=model.storage_latency_us,
            p_node=p_node,
            d_node=d_node,
            depends_on=full_write_ids,
            comment=(
                "All prefix writes finished; model fixed Storage commit/access delay."
            ),
        ))

    # ------------------------------------------------------------------
    # Phase 5: Storage -> D full prefix
    # ------------------------------------------------------------------
    storage_to_d_deps = [store_commit_id] if store_commit_id is not None else []

    for b in blocks:
        h = b["hash_id"]
        tok = b["tokens"]
        nbytes = tok * model.kv_bytes_per_token

        s_node = model.choose_storage(h)
        s_port = model.choose_storage_port(h)
        d_port = model.choose_server_port(
            "storage-to-d", req["request_id"], h
        )

        tid = next_task_id
        next_task_id += 1
        storage_to_d_ids.append(tid)

        tasks.append(make_task(
            task_id=tid,
            request_id=req["request_id"],
            phase="FORMAL",
            task_class="NETWORK",
            task_type="STORAGE_TO_D",
            src_node=s_node,
            src_port=s_port,
            dst_node=d_node,
            dst_port=d_port,
            num_bytes=nbytes,
            hash_id=h,
            block_index=b["block_index"],
            block_tokens=tok,
            p_node=p_node,
            d_node=d_node,
            depends_on=storage_to_d_deps,
            comment="Full-prefix Storage -> D after all prefix writes commit.",
        ))

    # ------------------------------------------------------------------
    # Phase 6: Decode on D
    # ------------------------------------------------------------------
    decode_id = None
    if req["output_length"] > 0:
        decode_id = next_task_id
        next_task_id += 1

        tasks.append(make_task(
            task_id=decode_id,
            request_id=req["request_id"],
            phase="FORMAL",
            task_class="COMPUTE",
            task_type="DECODE",
            duration_us=model.decode_duration_us(req["output_length"]),
            src_node=d_node,
            dst_node=d_node,
            block_tokens=req["output_length"],
            p_node=p_node,
            d_node=d_node,
            depends_on=storage_to_d_ids,
            comment="Decode starts after all Storage->D prefix flows finish.",
        ))

        # --------------------------------------------------------------
        # Phase 7: Decode KV persistence D -> Storage
        # --------------------------------------------------------------
        tag = f"decode:{req['request_id']}"
        s_node = model.choose_storage(tag)
        s_port = model.choose_storage_port(tag)
        d_port = model.choose_server_port(
            "decode-write-d", req["request_id"], tag
        )

        tid = next_task_id
        next_task_id += 1

        tasks.append(make_task(
            task_id=tid,
            request_id=req["request_id"],
            phase="FORMAL",
            task_class="NETWORK",
            task_type="DECODE_WRITE",
            src_node=d_node,
            src_port=d_port,
            dst_node=s_node,
            dst_port=s_port,
            num_bytes=req["output_length"] * model.kv_bytes_per_token,
            block_tokens=req["output_length"],
            p_node=p_node,
            d_node=d_node,
            depends_on=[decode_id],
            comment="Decode-generated KV: D -> Storage.",
        ))

    debug = {
        "request_id": req["request_id"],
        "phase": "FORMAL",
        "timestamp_ms": req["timestamp_ms"],
        "input_length": req["input_length"],
        "output_length": req["output_length"],
        "num_hash_blocks": len(req["hash_ids"]),
        "p_node": p_node,
        "d_node": d_node,
        "prefix_key": ",".join(str(x) for x in prefix_key),
        "prefix_hits": sum(1 for b in blocks if b["hit"]),
        "prefix_misses": sum(1 for b in blocks if not b["hit"]),
        "cached_prefix_tokens": cached_tokens,
        "miss_prefix_tokens": miss_tokens,
        "prefill_duration_us": model.prefill_duration_us(miss_tokens),
        "num_layers": model.num_layers,
        "prefill_layer_tasks": len(prefill_layer_ids),
        "prefix_read_tasks": len(read_ids),
        "prefix_store_write_tasks": len(full_write_ids),
        "storage_to_d_tasks": len(storage_to_d_ids),
        "full_prefix_bytes": sum(
            b["tokens"] * model.kv_bytes_per_token for b in blocks
        ),
        "read_gate_tasks": join_deps(read_ids),
        "prefill_layer_task_ids": join_deps(prefill_layer_ids),
        "store_commit_task_id": "" if store_commit_id is None else store_commit_id,
        "writeback_bytes": sum(
            b["tokens"] * model.kv_bytes_per_token
            for b in blocks
            if not b["hit"]
        ),
    }

    return tasks, debug, new_hashes, next_task_id


def build_warmup_debug(req, model, seen_snapshot):
    p_node, prefix_key = model.choose_p(req)
    d_node = model.choose_d(req)
    blocks, cached_tokens, miss_tokens = classify_prefix(
        req, seen_snapshot, model
    )

    return {
        "request_id": req["request_id"],
        "phase": "WARMUP",
        "timestamp_ms": req["timestamp_ms"],
        "input_length": req["input_length"],
        "output_length": req["output_length"],
        "num_hash_blocks": len(req["hash_ids"]),
        "p_node": p_node,
        "d_node": d_node,
        "prefix_key": ",".join(str(x) for x in prefix_key),
        "prefix_hits": sum(1 for b in blocks if b["hit"]),
        "prefix_misses": sum(1 for b in blocks if not b["hit"]),
        "cached_prefix_tokens": cached_tokens,
        "miss_prefix_tokens": miss_tokens,
        "prefill_duration_us": model.prefill_duration_us(miss_tokens),
        "num_layers": model.num_layers,
        "prefill_layer_tasks": 0,
        "prefix_read_tasks": 0,
        "storage_to_d_tasks": 0,
        "full_prefix_bytes": sum(
            b["tokens"] * model.kv_bytes_per_token for b in blocks
        ),
        "read_gate_tasks": "",
        "prefill_layer_task_ids": "",
        "store_commit_task_id": "",
        "prefix_store_write_tasks": 0,
        "writeback_bytes": 0,
    }, [b["hash_id"] for b in blocks if not b["hit"]]


# ---------------------------------------------------------------------------
# Whole trace
# ---------------------------------------------------------------------------

def generate_dag(reqs, model, warmup_requests=0):
    if warmup_requests < 0:
        raise ValueError("--warmup-requests must be >= 0")
    if warmup_requests >= len(reqs):
        raise ValueError("--warmup-requests must be smaller than loaded requests")

    warmup_ids = set(r["request_id"] for r in reqs[:warmup_requests])

    by_ts = defaultdict(list)
    for r in reqs:
        by_ts[r["timestamp_ms"]].append(r)

    seen_blocks = set()
    tasks = []
    request_debug = []
    next_task_id = 0

    for ts in sorted(by_ts):
        snapshot = set(seen_blocks)
        newly_seen = []

        for req in by_ts[ts]:
            if req["request_id"] in warmup_ids:
                debug, new_hashes = build_warmup_debug(
                    req, model, snapshot
                )
            else:
                req_tasks, debug, new_hashes, next_task_id = (
                    build_formal_request_tasks(
                        req, model, snapshot, next_task_id
                    )
                )
                tasks.extend(req_tasks)

            request_debug.append(debug)
            newly_seen.extend(new_hashes)

        # Same-timestamp requests do not see each other's new blocks.
        seen_blocks.update(newly_seen)

    summary = build_summary(tasks, request_debug, warmup_requests)
    return tasks, request_debug, summary


def build_summary(tasks, request_debug, warmup_requests):
    formal = [r for r in request_debug if r["phase"] == "FORMAL"]

    task_count = Counter(t["task_type"] for t in tasks)
    bytes_by_type = Counter()
    p_requests = Counter()
    d_requests = Counter()

    for t in tasks:
        if t["task_class"] == "NETWORK":
            bytes_by_type[t["task_type"]] += t["bytes"]

    for r in formal:
        p_requests[r["p_node"]] += 1
        d_requests[r["d_node"]] += 1

    hits = sum(r["prefix_hits"] for r in formal)
    misses = sum(r["prefix_misses"] for r in formal)
    refs = hits + misses

    return {
        "warmup_requests": warmup_requests,
        "formal_requests": len(formal),
        "formal_prefix_refs": refs,
        "formal_hits": hits,
        "formal_misses": misses,
        "formal_hit_ratio": hits / refs if refs else 0.0,
        "formal_tasks_total": len(tasks),
        "task_count_by_type": dict(task_count),
        "network_bytes_by_type": dict(bytes_by_type),
        "p_requests": dict(sorted(p_requests.items())),
        "d_requests": dict(sorted(d_requests.items())),
        "p_nodes": list(range(0, 12)),
        "d_nodes": list(range(12, 16)),
        "pd_affinity": False,
        "kv_delivery_policy": (
            "Storage->P HIT; P Prefill MISS layer-by-layer; "
            "each ready layer immediately rewrites FULL-prefix layer KV to Storage; "
            "Storage sends FULL prefix to D after all layer writes commit"
        ),
    }


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def write_csv(path, rows, fieldnames=None):
    if not rows:
        return
    if fieldnames is None:
        fieldnames = list(rows[0].keys())

    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)


def print_summary(summary):
    print("=" * 94)
    print("Mooncake trace -> Store-mediated P/D Task DAG (v6 layer-pipelined)")
    print("=" * 94)
    print(f"Warm-up requests                 : {summary['warmup_requests']:,}")
    print(f"Formal requests                  : {summary['formal_requests']:,}")
    print(f"Formal prefix refs               : {summary['formal_prefix_refs']:,}")
    print(f"Formal hits                      : {summary['formal_hits']:,}")
    print(f"Formal misses                    : {summary['formal_misses']:,}")
    print(f"Formal hit ratio                 : {summary['formal_hit_ratio']*100:.2f}%")
    print(f"Formal DAG tasks                 : {summary['formal_tasks_total']:,}")

    print("\nTask count by type:")
    for k, v in summary["task_count_by_type"].items():
        print(f"  {k:<24} {v:,}")

    print("\nRequests per P:")
    for n, v in summary["p_requests"].items():
        print(f"  P {n:<5} {v:,}")

    print("\nRequests per D:")
    for n, v in summary["d_requests"].items():
        print(f"  D {n:<5} {v:,}")

    print("=" * 94)


def main():
    ap = argparse.ArgumentParser(
        description="Mooncake trace -> Store-mediated P/D DAG."
    )
    ap.add_argument("trace")
    ap.add_argument("--config", default="mooncake_pd_store_config_v6_layer_pipeline.json")
    ap.add_argument("--max-requests", type=int, default=None)
    ap.add_argument("--warmup-requests", type=int, default=0)
    ap.add_argument("--out-dir", default="mooncake_pd_store_dag_out")
    args = ap.parse_args()

    cfg = load_json(args.config)
    model = Model(cfg)
    reqs = load_trace(args.trace, args.max_requests)

    tasks, request_debug, summary = generate_dag(
        reqs, model, args.warmup_requests
    )

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    write_csv(out / "task_dag.csv", tasks, TASK_FIELDS)
    write_csv(out / "requests_debug.csv", request_debug)

    with open(out / "summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    print_summary(summary)
    print("\nOutput:")
    print(f"  {out / 'task_dag.csv'}")
    print(f"  {out / 'requests_debug.csv'}")
    print(f"  {out / 'summary.json'}")


if __name__ == "__main__":
    main()
