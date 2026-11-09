#!/usr/bin/env python3
"""v15 Trace -> logical TP batch DAG: batch limits apply PER Attention DP engine.

Open-loop event scheduling. Prefill and Decode optionally use a GLM-5.1
MLA + DSA + MoE FLOPs model (per-DP TP attention, shared EP routed experts);
Prefill uses compute_model.mode; Decode defaults to fixed timing (decode_mode=legacy).
Decode counts one new query per request plus its current KV context. Every
batch uses one logical whole-step COMPUTE after all required input KV is read.
New requests may read all layers after their own Prefill completes, while the
preceding Decode step is still computing. Indexer Key Cache is assumed resident
when FLOPs mode is used;
its network traffic is not emitted. Expert and TP communication are not emitted.
HBM capacity is not modelled. Network forecasts are estimates; execution ordering
is also encoded as DAG edges. The matching V15 converter emits Prefill TIMERs;
Decode computation folds into network dependency delays without TIMER rows.
"""

import argparse
import csv
import hashlib
import json
import math
import os
import shutil
import sys
import tempfile
import subprocess
import pickle
import time
from collections import ChainMap, Counter, defaultdict, OrderedDict
import heapq
import bisect
import copy
from functools import lru_cache
from operator import itemgetter
from decimal import Decimal, ROUND_CEILING
from pathlib import Path
FIXED_EFFECTIVE_P_TO_D_LENGTH_RATIO = 22.0
FIXED_TOTAL_ACCELERATORS = 1024
FIXED_EP_DEGREE = 256
FIXED_SERVICE_DOMAIN_COUNT = 4
FIXED_P_DOMAIN_COUNT = 3
FIXED_D_DOMAIN_COUNT = 1
SCHEMA = "v14-domain-serial-batch-tp-logical"
MAX_DEPENDENCY_FANIN = 512
VERSION = "v15-decode-fixed-folded-compute-20261109-v10"

FLOP_METRIC_FIELDS = (
    "mla_projection_flops", "indexer_projection_flops", "indexer_score_flops",
    "sparse_attention_flops", "attention_flops", "dense_ffn_flops", "router_flops",
    "routed_expert_flops", "shared_expert_flops",
)
TIME_METRIC_FIELDS = (
    "attention_time_us", "dense_ffn_time_us", "router_time_us",
    "routed_expert_time_us", "shared_expert_time_us",
)

# Public Ascend 950PR 32-AI-subsystem bin from Huawei's 2026 architecture
# white paper: INT8 peak = 919 TOPS.  The 28-subsystem bin is 804 TOPS.
# W8A8 is mapped to the INT8 path for this baseline.  This is peak throughput,
# not sustained application throughput; efficiency defaults to 1.0 (ideal bound).
PUBLIC_NPU_MODEL = "Ascend 950PR 32-AI-subsystem"
PUBLIC_NPU_PRECISION = "INT8"
PUBLIC_NPU_PEAK_OPS_PER_SEC = 919e12
PUBLIC_NPU_PEAK_SOURCE = (
    "Huawei Ascend 950 NPU Architecture White Paper (2026), Table 3-1: "
    "950PR 32-AI-subsystem bin, INT8 919 TOPS; 28-subsystem bin 804 TOPS"
)
class Progress:
    """Throttled standard-library progress bar; status goes only to stderr."""
    def __init__(self, label, total, unit="req", enabled=True, interval=0.5, stream=None):
        if not 0 < float(interval) < float("inf"):
            raise ValueError("progress interval must be finite and positive")
        self.label, self.total, self.unit = label, max(0, int(total)), unit
        self.enabled = bool(enabled)
        self.stream = sys.stderr if stream is None else stream
        self.tty = bool(getattr(self.stream, "isatty", lambda: False)())
        # Redirected logs get occasional complete lines instead of carriage returns.
        self.interval = float(interval) if self.tty else max(5.0, float(interval))
        self.started = time.monotonic()
        self.last_update, self.last_width, self.current, self.detail = 0.0, 0, 0, ""
    @staticmethod
    def duration(seconds):
        if seconds is None:
            return "--:--:--"
        seconds = max(0, int(seconds))
        hours, remainder = divmod(seconds, 3600)
        minutes, seconds = divmod(remainder, 60)
        return f"{hours:02d}:{minutes:02d}:{seconds:02d}"
    def update(self, completed, detail="", force=False, status=""):
        self.current = min(self.total, max(0, int(completed)))
        self.detail = detail
        if not self.enabled:
            return
        now = time.monotonic()
        if not force and now - self.last_update < self.interval:
            return
        elapsed = max(0.0, now - self.started)
        speed = self.current / elapsed if elapsed > 0 else 0.0
        fraction = self.current / self.total if self.total else 1.0
        eta = (self.total - self.current) / speed if speed > 0 else None
        if self.current == self.total:
            eta = 0.0
        filled = min(24, int(fraction * 24))
        bar = "#" * filled + "-" * (24 - filled)
        if self.unit == "bytes":
            scale, suffix = ((1048576, "MiB") if self.total >= 1048576
                             else (1024, "KiB") if self.total >= 1024 else (1, "B"))
            count = f"{self.current / scale:.1f}/{self.total / scale:.1f} {suffix}"
            rate = f"{speed / scale:.1f} {suffix}/s"
        else:
            count = f"{self.current:,}/{self.total:,} {self.unit}"
            rate = f"{speed:.1f} {self.unit}/s"
        line = (f"{self.label:<11} [{bar}] {fraction * 100:6.2f}% | {count}"
                f" | {rate} | elapsed {self.duration(elapsed)} | ETA {self.duration(eta)}")
        if detail:
            line += " | " + detail
        if status:
            line += " | " + status
        if self.tty:
            columns = max(40, shutil.get_terminal_size(fallback=(80, 24)).columns)
            width = min(24, max(8, columns // 10))
            filled = min(width, int(fraction * width))
            bar = "#" * filled + "-" * (width - filled)
            suffix = " | " + status if status else ""
            line = (f"{self.label:<11} [{bar}] {fraction * 100:6.2f}%"
                    f" | {count} | ETA {self.duration(eta)}")
            if len(line + suffix) >= columns:
                line = f"{self.label} {fraction * 100:.2f}% | {count} | ETA {self.duration(eta)}"
            for extra in (detail, rate, "elapsed " + self.duration(elapsed)):
                if extra and len(line + " | " + extra + suffix) < columns:
                    line += " | " + extra
            line = line[:max(0, columns - 1 - len(suffix))] + suffix
            padding = max(0, min(self.last_width, columns - 1) - len(line))
            self.stream.write("\r" + line + " " * padding)
            if status:
                self.stream.write("\n")
            self.last_width = len(line)
        else:
            self.stream.write(line + "\n")
        self.stream.flush()
        self.last_update = now
    def __enter__(self):
        self.update(0, force=True)
        return self
    def __exit__(self, exc_type, exc, traceback):
        # Never fake 100% if processing stopped, including --max-requests reads.
        self.update(self.current, self.detail, force=True,
                    status="done" if exc_type is None else "stopped")
        return False
def stable_hash_int(*parts) -> int:
    text = "|".join(str(x) for x in parts)
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big", signed=False)
def join_deps(ids):
    return ";".join(str(x) for x in ids)
def per_request_prewarm_block_count(num_blocks, ratio, rounding):
    """Return how many leading Blocks to Oracle-prewarm for one request."""
    if num_blocks <= 0 or ratio <= 0:
        return 0
    raw = num_blocks * ratio
    if rounding == "ceil":
        count = math.ceil(raw)
    elif rounding == "floor":
        count = math.floor(raw)
    else:
        raise ValueError(
            "--per-request-prefix-prewarm-rounding must be ceil or floor"
        )
    return min(num_blocks, max(0, int(count)))

def strict_positive_int(value, name):
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return value


def local_batch_limit(settings, default, label):
    """Accept the old key with NEW per-engine scope; never silently clamp BS."""
    explicit = [(key, settings[key]) for key in
                ("max_batch_size_per_dp_engine", "max_batch_size_per_dp_rank") if key in settings]
    if len(explicit) == 2 and explicit[0][1] != explicit[1][1]:
        raise ValueError(f"{label}: per-DP-engine and per-DP-rank batch limits disagree")
    key, value = explicit[0] if explicit else ("max_batch_size", settings.get("max_batch_size", default))
    return strict_positive_int(value, f"{label}.{key}")

class Model:
    def __init__(self, cfg):
        self.choose_storage_endpoint = lru_cache(maxsize=32768, typed=True)(self.choose_storage_endpoint)
        self.config = copy.deepcopy(cfg)
        model = cfg["model"]
        timing = cfg["timing"]
        compute = cfg["compute"]
        storage = cfg["storage"]
        delivery = cfg["kv_delivery"]
        self.kv_bytes_per_token = int(model["kv_bytes_per_token"])
        self.block_tokens = int(model["block_tokens"])
        self.num_layers = int(model.get("num_layers", 78))
        parallel = cfg.get("parallel", {})
        self.p_tp_degree = strict_positive_int(parallel.get("prefill_tp", 8), "parallel.prefill_tp")
        self.d_tp_degree = strict_positive_int(parallel.get("decode_tp", 2), "parallel.decode_tp")
        self.ep_degree = strict_positive_int(parallel.get("ep", model.get("ep_degree", FIXED_EP_DEGREE)), "parallel.ep")
        if "ep_degree" in model and int(model["ep_degree"]) != self.ep_degree:
            raise ValueError("parallel.ep and model.ep_degree disagree")
        self.kv_rank_layout = "canonical-layer-byte-range-tp-sharded-ep-not-kv-sharded"
        self.ep_internal_traffic_model = "not-modelled"
        pipeline = delivery.get("layer_pipeline", {})
        self.layer_pipeline_enabled = bool(pipeline.get("enabled", True))
        self.split_prefix_read_by_layer = bool(
            pipeline.get("split_prefix_read_by_layer", True)
        )
        self.split_prefix_store_write_by_layer = bool(
            pipeline.get("split_prefix_store_write_by_layer", True)
        )
        self.storage_to_d_streaming = bool(
            pipeline.get("storage_to_d_streaming", True)
        )
        self.prefill_us_per_token = float(timing["prefill_us_per_token"])
        self.decode_us_per_token = float(timing["decode_us_per_token"])
        self.first_decode_step_us = int(round(timing.get(
            "first_decode_step_us", self.decode_us_per_token
        )))
        self.storage_latency_us = int(timing["storage_latency_us"])
        # v14 flattens every configured (server, port) pair into an accelerator
        # endpoint and partitions the ordered endpoint list into EP=256 service
        # domains.  The old p_nodes/d_nodes lists are accepted only as a way of
        # supplying one ordered 16-server pool; their former role split is not
        # retained.
        if "server_nodes" in compute:
            compute_server_pool = list(compute["server_nodes"])
        else:
            compute_server_pool = []
            for node in list(compute.get("p_nodes", [])) + list(
                compute.get("d_nodes", [])
            ):
                if node not in compute_server_pool:
                    compute_server_pool.append(node)
        self.compute_server_pool = compute_server_pool
        self.server_ports = list(compute["server_ports"])
        self.compute_endpoints = tuple(
            (node, port)
            for node in self.compute_server_pool
            for port in self.server_ports
        )
        self.total_accelerators = len(self.compute_endpoints)
        self.service_domain_count = self.total_accelerators // self.ep_degree
        self.p_domain_count = int(
            compute.get("p_service_domain_count", FIXED_P_DOMAIN_COUNT)
        )
        self.d_domain_count = int(
            compute.get("d_service_domain_count", FIXED_D_DOMAIN_COUNT)
        )
        # These legacy fields are used only to create a readable prefix key.
        # They never participate in compute placement.
        legacy_p_placement = compute.get("p_placement", {})
        self.p_hash_blocks = int(legacy_p_placement.get("hash_blocks", 4))
        self.p_exclude_hash_ids = set(
            legacy_p_placement.get("exclude_hash_ids", [])
        )
        scheduling = cfg.get("scheduling", {})
        self.ignored_config_settings = []
        if self.storage_to_d_streaming:
            self.ignored_config_settings.append(
                "kv_delivery.layer_pipeline.storage_to_d_streaming ignored; Decode reads all layers before Compute")
        self.storage_to_d_streaming = False
        for key in ("tp_degree", "p_tp_degree", "d_tp_degree"):
            if key in scheduling:
                self.ignored_config_settings.append(f"scheduling.{key} ignored; use parallel.*")
        for key in (
            "p_hbm_capacity_bytes_per_group",
            "d_hbm_capacity_bytes_per_group",
            "hbm_safety_reserve_bytes",
            "pd_affinity_mode",
            "pd_affinity",
            "non_affinity_penalty_us",
        ):
            if key in scheduling:
                self.ignored_config_settings.append(
                    f"scheduling.{key} ignored by ASU-only v14"
                )
        if "kv_rank_layout" in model:
            self.ignored_config_settings.append(
                "model.kv_rank_layout ignored; v14 uses canonical layer byte ranges"
            )
        prefill_scheduler = str(
            scheduling.get("prefill_scheduler", "ttft-aware")
        ).lower()
        if prefill_scheduler in {"cache-aware", "network-aware"}:
            prefill_scheduler = "ttft-aware"
        self.prefill_scheduler = prefill_scheduler
        decode_scheduler = str(
            scheduling.get("decode_scheduler", "first-token-aware")
        ).lower()
        if decode_scheduler in {"kv-ready-load-aware", "ttft-aware",
                                "network-aware"}:
            decode_scheduler = "first-token-aware"
        self.decode_scheduler = decode_scheduler
        self.decode_tp_assignment = "admission-cohort-load-aware-request-pinned"
        if "decode_scheduler" in scheduling:
            self.ignored_config_settings.append(
                "scheduling.decode_scheduler is superseded for D TP placement by "
                "admission-cohort-load-aware-request-pinned; continuous-batch admission remains FIFO"
            )
        self.random_seed = int(scheduling.get("random_seed", 1))
        scheduler = cfg.get("scheduler", {})
        p = scheduler.get("prefill", {})
        d = scheduler.get("decode", {})
        self.p_max_num_seqs = local_batch_limit(p, 16, "scheduler.prefill")
        self.d_max_num_seqs = local_batch_limit(d, 64, "scheduler.decode")
        self.decode_tp_assignment = "per-dp-engine-cohort-load-aware-request-pinned"
        for name in ("max_tokens", "max_batch_tokens"):
            if p.get(name) is not None:
                raise ValueError(f"scheduler.prefill.{name} must be null/absent: v14 has no token cap")
        if scheduler.get("domain_max_concurrent_batches", 1) != 1:
            raise ValueError("this v14 implementation requires domain_max_concurrent_batches=1")
        if scheduler.get("cache_policy", "v13-arrival-snapshot-inflight-wait") != "v13-arrival-snapshot-inflight-wait":
            raise ValueError("only v13-arrival-snapshot-inflight-wait is implemented")
        if p.get("wait_us", 0) != 0:
            raise ValueError("Prefill wait_us must be zero")
        self.p_max_num_batched_tokens = self.d_max_num_batched_tokens = None
        self.p_compute_slots = self.d_compute_slots = None
        self.chunked_prefill_enabled = False
        self.continuous_batching_enabled = True
        self.compute_concurrency = "local-dp-engine-batches-one-coordinated-wave-per-shared-ep-domain"
        for key in ("batch_size", "p_max_num_seqs", "d_max_num_seqs", "p_max_num_batched_tokens",
                    "d_max_num_batched_tokens", "continuous_batching_enabled"):
            if key in scheduling:
                self.ignored_config_settings.append(f"scheduling.{key} ignored; use scheduler.*")
        self.ttft_slo_us = self._optional_positive_int(
            scheduling.get("ttft_slo_us")
        )
        self.tbt_slo_us = self._optional_positive_int(
            scheduling.get("tbt_slo_us")
        )
        self.storage_port_bandwidth_gbps = float(
            scheduling.get(
                "storage_port_bandwidth_gbps",
                storage.get("port_bandwidth_gbps", 200.0),
            )
        )
        self.compute_port_bandwidth_gbps = float(
            scheduling.get(
                "compute_port_bandwidth_gbps",
                scheduling.get(
                    "compute_group_bandwidth_gbps",
                    compute.get("group_bandwidth_gbps", 200.0),
                ),
            )
        )
        self.storage_nodes = list(storage["nodes"])
        self.storage_ports = list(storage["ports"])
        pd_analysis = cfg.get("pd_analysis", {})
        self.pd_window_ms = int(pd_analysis.get("window_ms", 600_000))
        self.p_group_tps_config = self._optional_positive_float(
            pd_analysis.get("p_tps_group")
        )
        self.d_group_tps_config = self._optional_positive_float(
            pd_analysis.get("d_tps_group")
        )
        self.p_tps_per_npu = self._optional_positive_float(
            pd_analysis.get("p_tps_per_npu")
        )
        self.d_tps_per_npu = self._optional_positive_float(
            pd_analysis.get("d_tps_per_npu")
        )
        self.rewrite_full_prefix = bool(
            delivery.get("rewrite_full_prefix", False)
        )
        self.storage_to_d_full_prefix = bool(
            delivery["storage_to_d_full_prefix"]
        )
        if self.ep_degree != FIXED_EP_DEGREE:
            raise ValueError(
                f"v14 experiment fixes model.ep_degree={FIXED_EP_DEGREE}; "
                f"got {self.ep_degree}"
            )
        if self.total_accelerators != FIXED_TOTAL_ACCELERATORS:
            raise ValueError(
                "EP service-domain mode requires exactly "
                f"{FIXED_TOTAL_ACCELERATORS} compute endpoints, calculated as "
                "len(compute server pool) * len(compute.server_ports); got "
                f"{self.total_accelerators}"
            )
        if self.total_accelerators % self.ep_degree != 0:
            raise ValueError("total accelerator count must be divisible by EP degree")
        if self.service_domain_count != FIXED_SERVICE_DOMAIN_COUNT:
            raise ValueError(
                f"expected {FIXED_SERVICE_DOMAIN_COUNT} EP service domains, got "
                f"{self.service_domain_count}"
            )
        if (self.p_domain_count, self.d_domain_count) != (
            FIXED_P_DOMAIN_COUNT, FIXED_D_DOMAIN_COUNT
        ):
            raise ValueError(
                "v14 experiment fixes P:D service-domain counts at "
                f"{FIXED_P_DOMAIN_COUNT}:{FIXED_D_DOMAIN_COUNT}; got "
                f"{self.p_domain_count}:{self.d_domain_count}"
            )
        if self.p_domain_count + self.d_domain_count != self.service_domain_count:
            raise ValueError("P and D service-domain counts must cover all domains")
        if not self.server_ports:
            raise ValueError("compute.server_ports must be non-empty")
        if not self.storage_nodes or not self.storage_ports:
            raise ValueError("storage nodes/ports must be non-empty")
        valid_prefill_schedulers = {
            "round-robin", "random", "least-load", "ttft-aware",
        }
        valid_decode_schedulers = {
            "round-robin", "random", "least-load", "first-token-aware",
        }
        if self.prefill_scheduler not in valid_prefill_schedulers:
            raise ValueError(
                f"unknown prefill_scheduler={self.prefill_scheduler!r}; "
                f"choose one of {sorted(valid_prefill_schedulers)}"
            )
        if self.decode_scheduler not in valid_decode_schedulers:
            raise ValueError(
                f"unknown decode_scheduler={self.decode_scheduler!r}; "
                f"choose one of {sorted(valid_decode_schedulers)}"
            )
        if self.storage_port_bandwidth_gbps <= 0:
            raise ValueError("storage_port_bandwidth_gbps must be > 0")
        if self.compute_port_bandwidth_gbps <= 0:
            raise ValueError("compute_port_bandwidth_gbps must be > 0")
        if self.prefill_us_per_token <= 0:
            raise ValueError("timing.prefill_us_per_token must be > 0")
        if self.decode_us_per_token <= 0:
            raise ValueError("timing.decode_us_per_token must be > 0")
        if self.first_decode_step_us < 0:
            raise ValueError("timing.first_decode_step_us must be >= 0")
        if self.pd_window_ms < 0:
            raise ValueError("pd_analysis.window_ms must be >= 0")
        if self.kv_bytes_per_token <= 0 or self.block_tokens <= 0:
            raise ValueError("KV bytes and block tokens must be positive")
        if self.storage_latency_us < 0:
            raise ValueError("storage latency must be nonnegative")
        if len(set(self.compute_endpoints)) != len(self.compute_endpoints):
            raise ValueError("duplicate compute endpoints")
        if self.num_layers <= 0:
            raise ValueError("model.num_layers must be > 0")
        if not self.layer_pipeline_enabled:
            raise ValueError("v14 requires kv_delivery.layer_pipeline.enabled=true")
        if not self.split_prefix_read_by_layer:
            raise ValueError(
                "v14 requires split_prefix_read_by_layer=true"
            )
        if not self.split_prefix_store_write_by_layer:
            raise ValueError(
                "v14 requires split_prefix_store_write_by_layer=true"
            )
        if not self.storage_to_d_full_prefix:
            raise ValueError(
                "v14 implements full-prefix ASU->D transfer"
            )
        for degree in (self.p_tp_degree, self.d_tp_degree):
            if self.ep_degree % degree:
                raise ValueError("TP degree must divide the 256-NPU domain")
        self.p_dp_degree = self.ep_degree // self.p_tp_degree
        self.d_dp_degree = self.ep_degree // self.d_tp_degree
        self._configure_prefill_compute_model(cfg.get("compute_model", {}))
        self._configure_decode_compute_model(cfg.get("compute_model", {}))
        self.p_domain_slot_bound = self.p_dp_degree * self.p_max_num_seqs
        self.d_domain_slot_bound = self.d_dp_degree * self.d_max_num_seqs
        for name, value in (("prefill_us_per_token", self.prefill_us_per_token),
                            ("decode_us_per_token", self.decode_us_per_token),
                            ("compute bandwidth", self.compute_port_bandwidth_gbps),
                            ("storage bandwidth", self.storage_port_bandwidth_gbps)):
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"{name} must be finite and positive")
        all_domains = self._make_service_domains()
        self.p_groups = all_domains[:self.p_domain_count]
        self.d_groups = all_domains[self.p_domain_count:]
        self.p_nodes = sorted({
            node for group in self.p_groups for node, _ in group["endpoints"]
        })
        self.d_nodes = sorted({
            node for group in self.d_groups for node, _ in group["endpoints"]
        })
        self.p_group_tps, self.p_group_tps_source = self._resolve_group_tps(
            role="P"
        )
        self.d_group_tps, self.d_group_tps_source = self._resolve_group_tps(
            role="D"
        )
    def _configure_prefill_compute_model(self, settings):
        """Configure the optional GLM-5.1 Prefill FLOPs model.

        The default remains the historical linear model so existing v15 configs
        continue to run unchanged.  In GLM-5.1 FLOPs mode, use the public Ascend
        950PR 32-core INT8 peak (919 TOPS) unless the config overrides it.  Since
        public peak is not sustained application throughput, efficiency defaults
        to 1.0 and therefore gives an idealized lower-bound compute time.
        """
        if not isinstance(settings, dict):
            raise ValueError("compute_model must be a JSON object")
        self.prefill_compute_mode = str(settings.get("mode", "legacy")).lower()
        if self.prefill_compute_mode not in {"legacy", "glm51_flops"}:
            raise ValueError("compute_model.mode must be 'legacy' or 'glm51_flops'")
        self.prefill_compute_model_name = (
            "glm51-mla-dsa-moe-flops-v1" if self.prefill_compute_mode == "glm51_flops"
            else "sum-miss-tokens-linear"
        )
        self.prefill_layer_compute_fields = [
            "batch_id", "service_domain", "layer_index", "layer_type", "request_count",
            "request_ids", "local_group_token_counts", "local_group_cached_token_counts",
            "miss_tokens", "cached_tokens",
            "mla_projection_flops", "indexer_projection_flops", "indexer_score_flops",
            "sparse_attention_flops", "attention_flops", "dense_ffn_flops", "router_flops",
            "routed_expert_flops", "shared_expert_flops", "attention_time_us",
            "dense_ffn_time_us", "router_time_us", "routed_expert_time_us",
            "shared_expert_time_us", "layer_compute_time_us", "layer_compute_us",
            "attention_critical_tp_group", "attention_tp_degree",
            "routed_expert_parallel_degree", "shared_expert_tp_degree",
            "expert_load_imbalance_factor", "indexer_key_cache_resident",
        ]
        self.prefill_compute_config = copy.deepcopy(settings)
        self.prefill_compute_flops_totals = Counter()
        if self.prefill_compute_mode == "legacy":
            self.glm51_arch = None
            self.npu_peak_flops = None
            self.prefill_efficiency = {}
            self.expert_load_imbalance_factor = 1.0
            self.shared_expert_tp_degree = self.p_tp_degree
            self.indexer_key_cache_resident = True
            return

        # GLM-5.1 official Hugging Face config defaults.  Values can be
        # overridden under compute_model.architecture if the deployed checkpoint
        # or serving implementation differs from the published configuration.
        arch = {
            "hidden_size": 6144,
            "num_hidden_layers": 78,
            "num_attention_heads": 64,
            "q_lora_rank": 2048,
            "kv_lora_rank": 512,
            "qk_nope_head_dim": 192,
            "qk_rope_head_dim": 64,
            "v_head_dim": 256,
            "index_n_heads": 32,
            "index_head_dim": 128,
            "index_topk": 2048,
            "n_routed_experts": 256,
            "num_experts_per_tok": 8,
            "moe_intermediate_size": 2048,
            "n_shared_experts": 1,
            "intermediate_size": 12288,
            "first_k_dense_replace": 3,
        }
        overrides = settings.get("architecture", {})
        if not isinstance(overrides, dict):
            raise ValueError("compute_model.architecture must be a JSON object")
        arch.update(overrides)
        positive_arch_fields = (
            "hidden_size", "num_hidden_layers", "num_attention_heads", "q_lora_rank",
            "kv_lora_rank", "qk_nope_head_dim", "qk_rope_head_dim", "v_head_dim",
            "index_n_heads", "index_head_dim", "index_topk", "n_routed_experts",
            "num_experts_per_tok", "moe_intermediate_size", "intermediate_size",
        )
        for name in positive_arch_fields:
            arch[name] = strict_positive_int(arch[name], f"compute_model.architecture.{name}")
        arch["n_shared_experts"] = int(arch["n_shared_experts"])
        arch["first_k_dense_replace"] = int(arch["first_k_dense_replace"])
        if arch["n_shared_experts"] < 0:
            raise ValueError("compute_model.architecture.n_shared_experts must be >= 0")
        if not 0 <= arch["first_k_dense_replace"] <= self.num_layers:
            raise ValueError("first_k_dense_replace must be between 0 and model.num_layers")
        if arch["num_hidden_layers"] != self.num_layers:
            raise ValueError(
                "compute_model.architecture.num_hidden_layers must match model.num_layers"
            )
        if arch["num_experts_per_tok"] > arch["n_routed_experts"]:
            raise ValueError("num_experts_per_tok cannot exceed n_routed_experts")
        self.glm51_arch = arch

        # GLM-5.1 config (Transformers 5.4.0) has no explicit indexer-sharing
        # pattern; its default index_topk_freq=1 runs the Indexer on every layer.
        indexer_layers = settings.get("indexer_layers", list(range(self.num_layers)))
        if not isinstance(indexer_layers, list):
            raise ValueError("compute_model.indexer_layers must be a list of layer indices")
        parsed_indexer_layers = sorted(set(int(x) for x in indexer_layers))
        if any(x < 0 or x >= self.num_layers for x in parsed_indexer_layers):
            raise ValueError("compute_model.indexer_layers contains an invalid layer index")
        self.indexer_layers = set(parsed_indexer_layers)

        self.indexer_key_cache_resident = bool(settings.get("indexer_key_cache_resident", True))
        if not self.indexer_key_cache_resident:
            raise ValueError(
                "Indexer Key Cache network traffic is not implemented; set "
                "compute_model.indexer_key_cache_resident=true for this experiment"
            )

        # W8A8 is treated as INT8 for this baseline.  Huawei's public 950PR
        # architecture table lists 919 TOPS for the 32-AI-subsystem bin and
        # 804 TOPS for the 28-subsystem bin.  Users can override this value for
        # another SKU or a measured kernel throughput.
        peak = settings.get("npu_peak_ops_per_sec", settings.get("npu_peak_flops"))
        if peak is None:
            peak = PUBLIC_NPU_PEAK_OPS_PER_SEC
        self.npu_peak_flops_is_public_default = math.isclose(
            float(peak), PUBLIC_NPU_PEAK_OPS_PER_SEC, rel_tol=0.0, abs_tol=0.5
        )
        # Keep the old internal attribute for compatibility; the value is a
        # precision-specific arithmetic operation rate (INT8 TOPS here), with
        # one MAC counted as two operations to match the FLOPs formulas.
        self.npu_peak_ops_per_sec = float(peak)
        self.npu_peak_flops = self.npu_peak_ops_per_sec
        if not math.isfinite(self.npu_peak_ops_per_sec) or self.npu_peak_ops_per_sec <= 0:
            raise ValueError("compute_model.npu_peak_ops_per_sec must be finite and positive")
        self.npu_model = str(settings.get("npu_model", PUBLIC_NPU_MODEL))
        self.npu_precision = str(settings.get("npu_precision", PUBLIC_NPU_PRECISION)).upper()
        self.npu_peak_source = str(settings.get("npu_peak_source", PUBLIC_NPU_PEAK_SOURCE))
        self.npu_peak_unit = str(settings.get(
            "npu_peak_unit", "ops/s (INT8 TOPS; MAC counted as 2 operations)"
        ))

        raw_eff = settings.get("efficiency", {})
        efficiency_names = ("attention", "dense_ffn", "router", "routed_expert", "shared_expert")
        aliases = {"dense_ffn": "dense", "routed_expert": "expert"}
        if isinstance(raw_eff, (int, float)) and not isinstance(raw_eff, bool):
            raw_eff = {name: raw_eff for name in efficiency_names}
        if not isinstance(raw_eff, dict):
            raise ValueError("compute_model.efficiency must be a number or JSON object")
        default_eff = float(raw_eff.get("default", 1.0))
        self.prefill_efficiency = {}
        for name in efficiency_names:
            value = raw_eff.get(name, raw_eff.get(aliases.get(name, ""), default_eff))
            value = float(value)
            if not math.isfinite(value) or not 0 < value <= 1:
                raise ValueError(f"compute_model.efficiency.{name} must be in (0, 1]")
            self.prefill_efficiency[name] = value

        self.expert_load_imbalance_factor = float(settings.get(
            "expert_load_imbalance_factor", settings.get("moe_load_imbalance_factor", 1.0)
        ))
        if not math.isfinite(self.expert_load_imbalance_factor) or self.expert_load_imbalance_factor < 1:
            raise ValueError("expert_load_imbalance_factor must be finite and >= 1")
        self.shared_expert_tp_degree = strict_positive_int(
            settings.get("shared_expert_tp_degree", self.p_tp_degree),
            "compute_model.shared_expert_tp_degree",
        )
        if self.ep_degree % self.shared_expert_tp_degree:
            raise ValueError("shared_expert_tp_degree must divide EP degree")

        d = arch["hidden_size"]
        h = arch["num_attention_heads"]
        self.glm51_flops = {
            # Q-A + Q-B + KV-A + KV-B + output projection.  Q-A is shared
            # with the Indexer query path and is counted only once here.
            "mla_projection_per_token": 2 * (
                d * arch["q_lora_rank"]
                + arch["q_lora_rank"] * h * (arch["qk_nope_head_dim"] + arch["qk_rope_head_dim"])
                + d * (arch["kv_lora_rank"] + arch["qk_rope_head_dim"])
                + arch["kv_lora_rank"] * h * (arch["qk_nope_head_dim"] + arch["v_head_dim"])
                + h * arch["v_head_dim"] * d
            ),
            # Indexer wq_b + wk + weights_proj; q_a is already counted above.
            "indexer_projection_per_token": 2 * (
                arch["q_lora_rank"] * arch["index_n_heads"] * arch["index_head_dim"]
                + d * arch["index_head_dim"]
                + d * arch["index_n_heads"]
            ),
            # Q·K across index heads plus per-head weighting/reduction.
            "indexer_score_per_pair": (
                2 * arch["index_n_heads"] * arch["index_head_dim"]
                + 2 * arch["index_n_heads"]
            ),
            # Sparse MLA QK + AV for one selected query-key pair.
            "sparse_attention_per_pair": (
                2 * h * (arch["qk_nope_head_dim"] + arch["qk_rope_head_dim"])
                + 2 * h * arch["v_head_dim"]
            ),
            "dense_ffn_per_token": 6 * d * arch["intermediate_size"],
            "router_per_token": 2 * d * arch["n_routed_experts"],
            "routed_expert_per_token": (
                6 * d * arch["moe_intermediate_size"] * arch["num_experts_per_tok"]
            ),
            "shared_expert_per_token": (
                6 * d * arch["moe_intermediate_size"] * arch["n_shared_experts"]
            ),
        }

    def _configure_decode_compute_model(self, settings):
        # Decode timing is independent of Prefill arithmetic. Fixed timing is
        # the default; FLOPs remains an explicit opt-in for comparisons.
        self.decode_compute_mode = str(settings.get("decode_mode", "legacy")).lower()
        if self.decode_compute_mode not in {"legacy", "glm51_flops"}:
            raise ValueError("compute_model.decode_mode must be legacy or glm51_flops")
        if self.decode_compute_mode == "glm51_flops" and self.prefill_compute_mode != "glm51_flops":
            raise ValueError("Decode glm51_flops requires compute_model.mode=glm51_flops")
        self.decode_compute_model_name = (
            "glm51-mla-dsa-moe-decode-flops-v1" if self.decode_compute_mode == "glm51_flops"
            else "max-member-step-baseline")
        self.decode_efficiency = dict(self.prefill_efficiency)
        self.decode_shared_expert_tp_degree = self.d_tp_degree
        self.decode_layer_classes = ()
        if self.decode_compute_mode == "legacy":
            return
        raw = settings.get("decode_efficiency", {})
        if isinstance(raw, (int, float)) and not isinstance(raw, bool):
            raw = {name: raw for name in self.prefill_efficiency}
        if not isinstance(raw, dict):
            raise ValueError("compute_model.decode_efficiency must be a number or JSON object")
        for name, fallback in self.prefill_efficiency.items():
            value = float(raw.get(name, raw.get("default", fallback)))
            if not math.isfinite(value) or not 0 < value <= 1:
                raise ValueError(f"compute_model.decode_efficiency.{name} must be in (0, 1]")
            self.decode_efficiency[name] = value
        self.decode_shared_expert_tp_degree = strict_positive_int(
            settings.get("decode_shared_expert_tp_degree",
                         settings.get("shared_expert_tp_degree", self.d_tp_degree)),
            "compute_model.decode_shared_expert_tp_degree")
        if self.ep_degree % self.decode_shared_expert_tp_degree:
            raise ValueError("decode_shared_expert_tp_degree must divide EP degree")
        self.decode_layer_classes = tuple(
            (layer < self.glm51_arch["first_k_dense_replace"], layer in self.indexer_layers)
            for layer in range(self.num_layers))
        self.ignored_config_settings.extend([
            "timing.decode_us_per_token ignored in glm51_flops mode",
            "timing.first_decode_step_us ignored in glm51_flops mode"])

    def decode_compute(self, states, include_layers=False):
        """One new query per member, using H=input_length+completed_steps cached keys.

        Incremental attention/Indexer sees H+1 keys, including the current
        query's own key. History is not projected or run through FFN again.
        The current expanded-KV path and Prefill arithmetic coefficients are
        shared; TP degree and efficiency are Decode-specific.
        """
        if not states:
            raise ValueError("cannot compute an empty Decode batch")
        contexts = [int(s["req"]["input_length"]) + int(s["token"]) for s in states]
        result = {
            "compute_model": self.decode_compute_model_name,
            "request_count": len(states), "new_tokens": len(states),
            "new_request_count": sum(s["token"] == 0 for s in states),
            "cached_context_tokens": sum(contexts),
            "visible_context_tokens": sum(contexts) + len(states),
            "min_cached_context_tokens": min(contexts), "max_cached_context_tokens": max(contexts),
            "layer_count": self.num_layers,
            "attention_tp_degree": self.d_tp_degree,
            "routed_expert_parallel_degree": self.ep_degree,
            "shared_expert_tp_degree": self.decode_shared_expert_tp_degree,
            "expert_load_imbalance_factor": self.expert_load_imbalance_factor,
        }
        if self.decode_compute_mode == "legacy":
            duration = max(self.first_decode_step_us if s["token"] == 0
                           else self.decode_duration_us(1) for s in states)
            result.update(compute_time_us=float(duration), compute_us=duration)
            return result, split_duration(duration, self.num_layers) if include_layers else [], []
        stats = defaultdict(lambda: Counter(request_count=0, miss_tokens=0, cached_tokens=0,
                                            indexer_pairs=0, sparse_pairs=0))
        for state, context in zip(states, contexts):
            group = state.get("d_tp_group_id", state.get("d_tp_group"))
            if group is None:
                raise ValueError("Decode FLOPs model needs a pinned TP group")
            indexer_pairs, sparse_pairs = self._dsa_pair_counts(1, context, self.glm51_arch["index_topk"])
            values = stats[group]
            values["request_count"] += 1
            values["miss_tokens"] += 1
            values["cached_tokens"] += context
            values["indexer_pairs"] += indexer_pairs
            values["sparse_pairs"] += sparse_pairs
        result.update(
            local_group_token_counts=json.dumps({str(k): v["miss_tokens"] for k, v in sorted(stats.items())}, separators=(",", ":")),
            local_group_cached_context_tokens=json.dumps({str(k): v["cached_tokens"] for k, v in sorted(stats.items())}, separators=(",", ":")),
            indexer_pairs_per_indexed_layer=sum(v["indexer_pairs"] for v in stats.values()),
            sparse_pairs_per_layer=sum(v["sparse_pairs"] for v in stats.values()))
        # Only up to four (Dense/MoE, Indexer on/off) metric sets are needed
        # per batch. Resident steps never build 78 repeated metric dicts.
        cache, layer_metrics = {}, []
        for layer, key in enumerate(self.decode_layer_classes):
            if key not in cache:
                cache[key] = self._layer_metrics(stats, layer, self.d_tp_degree,
                                                 self.decode_efficiency, self.decode_shared_expert_tp_degree)
            layer_metrics.append(cache[key])
        classes = Counter(self.decode_layer_classes)
        for field in FLOP_METRIC_FIELDS:
            result[field] = sum(count * cache[key][field] for key, count in classes.items())
        for field in TIME_METRIC_FIELDS:
            result[field] = math.fsum(count * cache[key][field] for key, count in classes.items())
        total = math.fsum(row["layer_compute_time_us"] for row in layer_metrics)
        duration = max(1, int(math.floor(total + 0.5))) if total > 0 else 0
        result.update(compute_time_us=total, compute_us=duration)
        durations, rows = [], []
        if include_layers:
            cumulative, previous = 0.0, 0
            for layer, metrics in enumerate(layer_metrics):
                cumulative += metrics["layer_compute_time_us"]
                rounded = duration if layer == self.num_layers - 1 else int(math.floor(cumulative + 0.5))
                durations.append(rounded - previous)
                previous = rounded
                rows.append(dict(layer_index=layer, **metrics,
                                 layer_compute_us=durations[-1]))
        return result, durations, rows

    @staticmethod
    def _dsa_pair_counts(miss_tokens, cached_tokens, topk):
        """Return (all indexer score pairs, selected sparse-attention pairs).

        The reference Indexer forms a dense [new_queries, cached+new_keys]
        score matrix before applying the causal mask, so its matmul computes
        m*(h+m) pairs.  The later sparse MLA attention uses only causal Top-K
        keys, capped at topk for each query.
        """
        m = int(miss_tokens)
        h = int(cached_tokens)
        if m <= 0:
            return 0, 0
        all_pairs = m * (h + m)
        linear_count = min(m, max(0, int(topk) - h - 1))
        selected_pairs = linear_count * (2 * h + linear_count + 1) // 2
        selected_pairs += (m - linear_count) * int(topk)
        return all_pairs, selected_pairs

    def _prefill_layer_metrics(self, group_stats, layer):
        return self._layer_metrics(group_stats, layer, self.p_tp_degree,
                                   self.prefill_efficiency, self.shared_expert_tp_degree)

    def _layer_metrics(self, group_stats, layer, tp_degree, efficiency, shared_tp_degree):
        """Shared arithmetic model; group miss_tokens counts newly computed queries."""
        a = self.glm51_arch
        c = self.glm51_flops
        is_dense = layer < a["first_k_dense_replace"]
        layer_type = "dense" if is_dense else "moe"
        total_miss = sum(v["miss_tokens"] for v in group_stats.values())
        total_cached = sum(v["cached_tokens"] for v in group_stats.values())
        request_count = sum(v["request_count"] for v in group_stats.values())

        group_flops = {}
        for group_id, values in group_stats.items():
            n = values["miss_tokens"]
            mla_projection = n * c["mla_projection_per_token"]
            indexer_projection = (
                n * c["indexer_projection_per_token"] if layer in self.indexer_layers else 0
            )
            indexer_score = (
                values["indexer_pairs"] * c["indexer_score_per_pair"]
                if layer in self.indexer_layers else 0
            )
            sparse_attention = values["sparse_pairs"] * c["sparse_attention_per_pair"]
            attention = mla_projection + indexer_projection + indexer_score + sparse_attention
            dense_ffn = n * c["dense_ffn_per_token"] if is_dense else 0
            router = n * c["router_per_token"] if not is_dense else 0
            shared = n * c["shared_expert_per_token"] if not is_dense else 0
            group_flops[group_id] = {
                "mla_projection_flops": mla_projection,
                "indexer_projection_flops": indexer_projection,
                "indexer_score_flops": indexer_score,
                "sparse_attention_flops": sparse_attention,
                "attention_flops": attention,
                "dense_ffn_flops": dense_ffn,
                "router_flops": router,
                "shared_expert_flops": shared,
            }

        def max_group_time(field, npu_count, efficiency):
            if not group_flops:
                return 0.0, ""
            times = {
                group_id: values[field] / (npu_count * self.npu_peak_flops * efficiency) * 1e6
                for group_id, values in group_flops.items()
            }
            critical = max(times, key=times.get)
            return times[critical], str(critical)

        attention_time, attention_critical = max_group_time(
            "attention_flops", tp_degree, efficiency["attention"]
        )
        dense_time, _ = max_group_time(
            "dense_ffn_flops", tp_degree, efficiency["dense_ffn"]
        )
        router_time, _ = max_group_time(
            "router_flops", tp_degree, efficiency["router"]
        )
        shared_time, _ = max_group_time(
            "shared_expert_flops", shared_tp_degree, efficiency["shared_expert"]
        )

        routed_expert_flops = (
            total_miss * c["routed_expert_per_token"] if not is_dense else 0
        )
        routed_expert_time = (
            routed_expert_flops
            / (self.ep_degree * self.npu_peak_flops * efficiency["routed_expert"])
            * 1e6 * self.expert_load_imbalance_factor
        ) if routed_expert_flops else 0.0

        totals = Counter()
        for values in group_flops.values():
            totals.update(values)
        layer_time = attention_time + dense_time + router_time + routed_expert_time + shared_time
        return {
            "layer_type": layer_type,
            "request_count": request_count,
            "miss_tokens": total_miss,
            "cached_tokens": total_cached,
            "mla_projection_flops": totals["mla_projection_flops"],
            "indexer_projection_flops": totals["indexer_projection_flops"],
            "indexer_score_flops": totals["indexer_score_flops"],
            "sparse_attention_flops": totals["sparse_attention_flops"],
            "attention_flops": totals["attention_flops"],
            "dense_ffn_flops": totals["dense_ffn_flops"],
            "router_flops": totals["router_flops"],
            "routed_expert_flops": routed_expert_flops,
            "shared_expert_flops": totals["shared_expert_flops"],
            "attention_time_us": attention_time,
            "dense_ffn_time_us": dense_time,
            "router_time_us": router_time,
            "routed_expert_time_us": routed_expert_time,
            "shared_expert_time_us": shared_time,
            "layer_compute_time_us": layer_time,
            "attention_critical_tp_group": attention_critical,
        }

    def prefill_layer_compute(self, states):
        """Return 78 per-layer compute rows and integer-us durations for a P wave."""
        group_stats = defaultdict(lambda: Counter(request_count=0, miss_tokens=0,
                                                   cached_tokens=0, indexer_pairs=0,
                                                   sparse_pairs=0))
        request_ids = []
        for state in states:
            group_id = state.get("p_tp_group_id", state["p_tp_group"])
            debug = state["debug"]
            miss = int(debug["miss_tokens"])
            cached = int(debug["cached_tokens"])
            all_pairs, sparse_pairs = self._dsa_pair_counts(
                miss, cached, self.glm51_arch["index_topk"]
            )
            item = group_stats[group_id]
            item["request_count"] += 1
            item["miss_tokens"] += miss
            item["cached_tokens"] += cached
            item["indexer_pairs"] += all_pairs
            item["sparse_pairs"] += sparse_pairs
            request_ids.append(state["req"]["request_id"])

        rows = []
        for layer in range(self.num_layers):
            metrics = self._prefill_layer_metrics(group_stats, layer)
            rows.append({
                "layer_index": layer,
                **metrics,
                "local_group_token_counts": json.dumps(
                    {str(k): int(v["miss_tokens"]) for k, v in sorted(group_stats.items())},
                    separators=(",", ":")
                ),
                "local_group_cached_token_counts": json.dumps(
                    {str(k): int(v["cached_tokens"]) for k, v in sorted(group_stats.items())},
                    separators=(",", ":")
                ),
                "request_ids": json.dumps(request_ids, ensure_ascii=False, separators=(",", ":")),
                "attention_tp_degree": self.p_tp_degree,
                "routed_expert_parallel_degree": self.ep_degree,
                "shared_expert_tp_degree": self.shared_expert_tp_degree,
                "expert_load_imbalance_factor": self.expert_load_imbalance_factor,
                "indexer_key_cache_resident": self.indexer_key_cache_resident,
            })

        # DAG timestamps are integer microseconds.  Quantize cumulative layer
        # completion times, not each layer independently, so rounding error
        # does not accumulate across 78 layers.
        cumulative = 0.0
        previous = 0
        for row in rows:
            cumulative += row["layer_compute_time_us"]
            rounded = int(math.floor(cumulative + 0.5))
            row["layer_compute_us"] = rounded - previous
            previous = rounded
        if cumulative > 0 and previous == 0:
            rows[-1]["layer_compute_us"] = 1
            previous = 1
        return rows, previous

    def prefill_request_duration_us(self, miss_tokens, cached_tokens):
        """Per-request isolated estimate retained in requests_debug.csv."""
        if self.prefill_compute_mode == "legacy":
            return self.prefill_duration_us(miss_tokens)
        key = (int(miss_tokens), int(cached_tokens))
        cache = getattr(self, "_prefill_request_duration_cache", None)
        if cache is None:
            cache = self._prefill_request_duration_cache = {}
        if key not in cache:
            miss, cached = key
            all_pairs, sparse_pairs = self._dsa_pair_counts(
                miss, cached, self.glm51_arch["index_topk"]
            )
            group_stats = {0: Counter(
                request_count=1, miss_tokens=miss, cached_tokens=cached,
                indexer_pairs=all_pairs, sparse_pairs=sparse_pairs,
            )}
            dense_count = self.glm51_arch["first_k_dense_replace"]
            moe_count = self.num_layers - dense_count
            total_us = 0.0
            if dense_count:
                total_us += dense_count * self._prefill_layer_metrics(group_stats, 0)["layer_compute_time_us"]
            if moe_count:
                total_us += moe_count * self._prefill_layer_metrics(group_stats, dense_count)["layer_compute_time_us"]
            cache[key] = int(math.floor(total_us + 0.5))
        return cache[key]

    @staticmethod
    def _optional_positive_int(value):
        if value is None:
            return None
        parsed = int(value)
        if parsed <= 0:
            raise ValueError("optional integer setting must be > 0")
        return parsed
    @staticmethod
    def _optional_positive_float(value):
        if value is None:
            return None
        parsed = float(value)
        if parsed <= 0:
            raise ValueError("optional throughput setting must be > 0")
        return parsed
    def _resolve_group_tps(self, role):
        if role == "D" and self.decode_compute_mode == "glm51_flops":
            return None, "context-dependent-decode-flops"
        if role == "P":
            explicit = self.p_group_tps_config
            per_npu = self.p_tps_per_npu
            linear_us = self.prefill_us_per_token
            slots = self.p_compute_slots
        else:
            explicit = self.d_group_tps_config
            per_npu = self.d_tps_per_npu
            linear_us = self.decode_us_per_token
            slots = self.d_compute_slots
        if explicit is not None:
            return explicit, "explicit-group-tps"
        if per_npu is not None:
            return None, "per-npu-linear-scaling-disabled-in-domain-serial-v14"
        if linear_us <= 0 or slots is None:
            return None, "unavailable"
        return (
            1_000_000.0 / linear_us * slots,
            "fixed-linear-time-times-compute-slots",
        )
    def _make_service_domains(self):
        """Partition 1024 ordered endpoints into four EP=256 domains."""
        groups = []
        for domain_index in range(self.service_domain_count):
            begin = domain_index * self.ep_degree
            end = begin + self.ep_degree
            endpoints = self.compute_endpoints[begin:end]
            role = "P" if domain_index < self.p_domain_count else "D"
            role_index = (
                domain_index if role == "P"
                else domain_index - self.p_domain_count
            )
            groups.append({
                "id": f"{role}G{role_index}",
                "domain_index": domain_index,
                "endpoints": endpoints,
                "nodes": tuple(sorted({node for node, _ in endpoints})),
                "coordinator_node": endpoints[0][0],
            })
        return groups
    def prefill_duration_us(self, tokens):
        return int(round(tokens * self.prefill_us_per_token))
    def prefill_chunk_count(self, tokens):
        tokens = int(tokens)
        if tokens <= 0:
            return 0
        limit = self.p_max_num_batched_tokens
        if limit is None or tokens <= limit:
            return 1
        if not self.chunked_prefill_enabled:
            raise ValueError(
                f"Prefill MISS tokens={tokens} exceed "
                f"p_max_num_batched_tokens={limit}, while chunked Prefill is disabled"
            )
        return int(math.ceil(tokens / limit))
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
    def first_decode_duration_us(self, output_tokens):
        if output_tokens <= 0:
            return 0
        return min(self.decode_duration_us(output_tokens), self.first_decode_step_us)
    def prefix_key(self, req):
        ids = [
            h for h in req["hash_ids"]
            if h not in self.p_exclude_hash_ids
        ][:self.p_hash_blocks]
        return tuple(ids) if ids else ("request", req["request_id"])
    def choose_storage_endpoint(self, tag):
        """Stable hash placement over the Cartesian set of Storage ports."""
        endpoint_count = len(self.storage_nodes) * len(self.storage_ports)
        idx = stable_hash_int("kv-storage-endpoint", tag) % endpoint_count
        node_idx, port_idx = divmod(idx, len(self.storage_ports))
        return self.storage_nodes[node_idx], self.storage_ports[port_idx]
    def group_physical_bytes(self, logical_bytes, endpoints):
        if not tuple(endpoints):
            raise ValueError("service domain must expose at least one endpoint")
        return int(logical_bytes)
    @staticmethod
    def transfer_duration_us(num_bytes, bandwidth_gbps):
        # 1 Gbit/s == 1000 bit/us. Keep every non-empty flow at >= 1 us.
        if num_bytes <= 0:
            return 0
        return max(1, int(math.ceil(num_bytes * 8 / (bandwidth_gbps * 1000))))
TASK_FIELDS = [
    "task_id", "request_id", "phase", "task_class", "task_type",
    "release_time_us", "duration_us", "src_node", "src_port", "dst_node", "dst_port",
    "bytes", "hash_id", "block_index", "block_tokens", "tp_rank", "kv_rank_layout",
    "layer_index", "layer_count", "layer_ready_offset_us", "decode_token_index",
    "output_block_index", "storage_key", "p_node", "p_port", "p_group", "d_node",
    "d_port", "d_group", "depends_on", "dependency_mode", "dependency_event",
    "network_wave", "storage_read_latency_us", "execution_schema", "comment",
]
DEBUG_FIELDS = [
    "request_id", "phase", "timestamp_us", "service_start_us", "input_length", "output_length",
    "hit_blocks", "miss_blocks", "cached_tokens", "miss_tokens", "p_group", "p_node", "p_port",
    "d_group", "d_node", "d_port", "p_queue_us", "d_queue_us", "prefill_compute_us",
    "decode_compute_us", "p_done_task_id", "d_done_task_id", "oracle_target_blocks",
]
CANDIDATE_FIELDS = [
    "request_id", "role", "group", "worker_node", "worker_port", "compute_queue_us",
    "estimated_read_finish_us", "score", "selected",
]
@lru_cache(maxsize=32768, typed=True)
def hash_key(h):
    return json.dumps(h, ensure_ascii=False, separators=(",", ":"))
def load_trace(path, max_requests=None, show_progress=True, progress_interval=0.5):
    requests, ids = [], set()
    path = Path(path)
    processed_bytes = 0
    with path.open("rb") as f, Progress("Trace read", path.stat().st_size, unit="bytes",
                                        enabled=show_progress, interval=progress_interval) as progress:
        for line_no, raw_line in enumerate(f, 1):
            processed_bytes += len(raw_line)
            line = raw_line.decode("utf-8-sig" if line_no == 1 else "utf-8")
            if not line.strip():
                progress.update(processed_bytes, f"{len(requests):,} requests")
                continue
            try:
                obj = json.loads(line)
                rid = obj.get("request_id", len(requests))
                length, output = obj.get("input_length", 0), obj.get("output_length", 0)
                for name, value in (("request_id", rid), ("input_length", length), ("output_length", output)):
                    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                        raise ValueError(f"{name} must be a nonnegative integer")
                if rid in ids:
                    raise ValueError(f"duplicate request_id {rid}")
                timestamp = Decimal(str(obj.get("timestamp", obj.get("timestamp_ms", 0))))
                if not timestamp.is_finite() or timestamp < 0:
                    raise ValueError("timestamp must be nonnegative milliseconds")
                hashes = obj.get("hash_ids", [])
                if not isinstance(hashes, list) or any(
                    isinstance(h, bool) or not isinstance(h, (int, str)) for h in hashes
                ):
                    raise ValueError("hash_ids must be a list of integer/string hashes")
                ids.add(rid)
                requests.append(dict(request_id=rid, line_no=line_no,
                                     timestamp_us=int((timestamp * 1000).to_integral_value(rounding=ROUND_CEILING)),
                                     input_length=length, output_length=output, hash_ids=hashes))
            except (ValueError, TypeError, ArithmeticError) as exc:
                raise ValueError(f"trace line {line_no}: {exc}") from None
            progress.update(processed_bytes, f"{len(requests):,} requests")
            if max_requests is not None and len(requests) >= max_requests:
                progress.detail += " | request limit reached"
                break
    if not requests:
        raise ValueError("trace is empty")
    requests.sort(key=lambda r: (r["timestamp_us"], r["request_id"]))
    return requests
def blocks_for(req, model, cache):
    """Keep V14's contiguous prefix HIT rule; account for every input token."""
    count = (req["input_length"] + model.block_tokens - 1) // model.block_tokens
    if len(req["hash_ids"]) > count:
        raise ValueError(f"request {req['request_id']}: more hashes than input blocks")
    blocks, continuous = [], True
    for index in range(count):
        known = index < len(req["hash_ids"])
        h = req["hash_ids"][index] if known else f"__unhashed_input__:{req['request_id']}:{index}"
        tokens = min(model.block_tokens, req["input_length"] - index * model.block_tokens)
        entry = cache.get(hash_key(h)) if known else None
        hit = bool(continuous and entry is not None and entry["tokens"] == tokens)
        continuous = hit
        blocks.append(dict(block_index=index, hash_id=h, tokens=tokens, hit=hit,
                           known_hash=known, producer=entry["producer"] if hit else None))
    return blocks

# The legacy field names stay available for downstream adapters. Batch compute
# has request_id="" and request_ids=[...]; request-owned KV retains request_id.
TASK_FIELDS += [
    "batch_id", "batch_size", "batch_type", "request_ids", "batch_step_index",
    "tp_degree", "tp_group_id", "service_domain", "ep_rank", "expert_id",
    "kv_shard_offset_bytes", "kv_shard_bytes", "kv_layer_bytes",
    "kv_extent_kind", "kv_token_offset",
    "kv_token_bytes", "kv_shard_layer_ranges",
    "forecast_start_us", "forecast_finish_us",
]
DEBUG_FIELDS += [
    "p_batch_id", "p_tp_group_id", "d_first_batch_id", "d_tp_group_id",
    "p_dispatch_us", "p_compute_start_us", "p_compute_finish_us",
    "p_forecast_done_us", "d_dispatch_us", "d_compute_finish_us",
    "d_forecast_done_us", "generated_tokens", "decode_step_count",
]
CANDIDATE_FIELDS = [
    "batch_id", "role", "service_domain", "tp_group_id", "tp_endpoints",
    "compute_queue_us", "estimated_read_finish_us", "score", "selected",
    "request_id", "resident_kv_bytes", "remaining_decode_steps",
    "resident_requests", "server_resident_kv_bytes", "placement_policy",
    "request_ids",
]
BATCH_FIELDS = [
    "batch_id", "batch_type", "batch_step_index", "request_ids", "batch_size",
    "service_domain", "tp_group_id", "tp_degree", "tp_endpoints",
    "input_tokens", "miss_tokens", "decode_token_indices", "compute_duration_us",
    "dispatch_us", "forecast_compute_start_us", "forecast_compute_finish_us",
    "begin_task_id", "compute_done_task_id", "previous_domain_compute_done_task_id",
    "request_tp_groups",
    "local_batch_sizes", "dp_engine_count", "local_batch_limit", "batch_size_scope",
]

PREFILL_LAYER_COMPUTE_FIELDS = [
    "batch_id", "service_domain", "layer_index", "layer_type", "request_count",
    "request_ids", "local_group_token_counts", "local_group_cached_token_counts",
    "miss_tokens", "cached_tokens",
    "mla_projection_flops", "indexer_projection_flops", "indexer_score_flops",
    "sparse_attention_flops", "attention_flops", "dense_ffn_flops", "router_flops",
    "routed_expert_flops", "shared_expert_flops", "attention_time_us",
    "dense_ffn_time_us", "router_time_us", "routed_expert_time_us",
    "shared_expert_time_us", "layer_compute_time_us", "layer_compute_us",
    "attention_critical_tp_group", "attention_tp_degree", "routed_expert_parallel_degree",
    "shared_expert_tp_degree", "expert_load_imbalance_factor", "indexer_key_cache_resident",
]
DECODE_COMPUTE_FIELDS = [
    "batch_id", "batch_step_index", "service_domain", "request_ids", "compute_granularity",
    "compute_model", "request_count", "new_request_count", "new_tokens", "layer_count",
    "cached_context_tokens", "visible_context_tokens", "min_cached_context_tokens", "max_cached_context_tokens",
    "local_group_token_counts", "local_group_cached_context_tokens",
    "indexer_pairs_per_indexed_layer", "sparse_pairs_per_layer",
    *FLOP_METRIC_FIELDS, *TIME_METRIC_FIELDS, "compute_time_us", "compute_us",
    "attention_tp_degree", "routed_expert_parallel_degree", "shared_expert_tp_degree",
    "expert_load_imbalance_factor",
]
DECODE_LAYER_COMPUTE_FIELDS = [
    "batch_id", "batch_step_index", "service_domain", "layer_index", "layer_type",
    "new_tokens", "cached_context_tokens", *FLOP_METRIC_FIELDS, *TIME_METRIC_FIELDS,
    "layer_compute_time_us", "layer_compute_us", "attention_critical_tp_group",
]


def split_exact(total, parts, index):
    """Integer contiguous partition; return byte offset and length."""
    q, r = divmod(int(total), parts)
    return index * q + min(index, r), q + int(index < r)


def split_duration(total, layers):
    result, previous = [], 0
    for layer in range(layers):
        current = round(total * (layer + 1) / layers)
        result.append(current - previous)
        previous = current
    return result


TRAFFIC_TASK_FIELDS = [
    "task_id", "request_id", "task_class", "task_type", "depends_on",
    "release_time_us", "duration_us", "src_node", "dst_node", "bytes",
    "execution_schema", "network_wave", "storage_read_latency_us",
]
LOGICAL_EXTRA_FIELDS = [
    "tp_members", "tp_degree", "tp_group_id", "batch_id", "layer_index",
    "block_index", "decode_token_index", "forecast_start_us", "forecast_finish_us",
    "batch_type", "p_node", "d_node",
]
LOGICAL_TRAFFIC_FIELDS = TRAFFIC_TASK_FIELDS + LOGICAL_EXTRA_FIELDS
LOGICAL_FULL_FIELDS = TASK_FIELDS + ["tp_members"]


class DPEngine:
    """Local admission and batch selection; EP compute is a shared resource.

    P releases its local batch at compute completion, as in v14. D keeps
    request residency/pinning across steps and releases after its last compute
    step; outstanding ASU writes still gate request D_DONE, exactly as v14.
    No next wave can admit requests until the domain's compute-free event.
    """
    def __init__(self, index, limit):
        self.index, self.limit = index, limit
        self.residents = []
        self.peak_residents = 0
        self.local_batches = 0

    @property
    def slots(self):
        return self.limit - len(self.residents)

    def admit(self, cohort):
        if len(cohort) > self.slots:
            raise RuntimeError("DP engine admission exceeded its local BS limit")
        self.residents.extend(cohort)
        self.peak_residents = max(self.peak_residents, len(self.residents))

    def select_batch(self):
        if len(self.residents) > self.limit:
            raise RuntimeError("DP engine residency exceeded its local BS limit")
        if self.residents:
            self.local_batches += 1
        return list(self.residents)


class Domain:
    def __init__(self, spec, degree, local_limit):
        self.id = spec["id"]
        self.index = spec["domain_index"]
        self.degree = degree
        endpoints = spec["endpoints"]
        self.groups = [tuple(endpoints[i:i + degree])
                       for i in range(0, len(endpoints), degree)]
        self.engines = [DPEngine(gi, local_limit) for gi in range(len(self.groups))]
        self.peak_residents = 0
        self.available_us = 0
        self.last_compute_done = None
        self.rr = 0
        self.decode_server_rr = 0
        # This list preserves V14's global execution order only. Admission
        # and resident capacity belong to engines, not to this aggregate list.
        self.residents = []
        self.step = 0

    def group_id(self, index):
        return f"{self.id}:TP{index}"

    def local_wave(self, role):
        selected = [state for engine in self.engines for state in engine.select_batch()]
        if len({id(state) for state in selected}) != len(selected):
            raise RuntimeError("a request belongs to more than one DP engine")
        self.peak_residents = max(self.peak_residents, len(selected))
        if role == "D":
            expected = {id(state) for state in self.residents}
            if expected != {id(state) for state in selected}:
                raise RuntimeError("local DP residency and shared EP wave disagree")
            return list(self.residents)
        return selected


class PortCalendar:
    """Allow an earlier-ready flow to use gaps before future reservations."""
    def __init__(self, active=None):
        self.intervals = []
        self.active = active

    def first_free(self, start, duration):
        intervals = self.intervals
        if not intervals or intervals[-1][1] <= start:
            return start
        if start + duration <= intervals[0][0]:
            return start
        if len(intervals) == 1:
            return intervals[0][1]
        index = max(0, bisect.bisect_right(intervals, (start, math.inf)) - 1)
        for i in range(index, len(self.intervals)):
            left, right = self.intervals[i]
            if right <= start:
                continue
            if left >= start + duration:
                break
            start = right
        return start

    def reserve(self, start, finish):
        if finish <= start:
            raise ValueError("port reservation must have positive duration")
        intervals = self.intervals
        if self.active is not None:
            self.active.add(self)
        # Most forecasts append to the busy-time tail. Preserve the general
        # gap-filling path for flows ready before existing future bookings.
        if not intervals or intervals[-1][1] <= start:
            if intervals and intervals[-1][1] == start:
                intervals[-1] = (intervals[-1][0], finish)
            else:
                intervals.append((start, finish))
            return
        index = bisect.bisect_left(intervals, (start, finish))
        # Coalesce touching reservations. The busy-time union is identical,
        # but a run of 100,000 back-to-back flows becomes one search interval.
        # True overlaps remain errors rather than being hidden by the merge.
        if index and self.intervals[index - 1][1] > start:
            raise RuntimeError("overlapping port reservations")
        if index < len(self.intervals) and self.intervals[index][0] < finish:
            raise RuntimeError("overlapping port reservations")
        if index and self.intervals[index - 1][1] == start:
            index -= 1
            start = self.intervals.pop(index)[0]
        if index < len(self.intervals) and self.intervals[index][0] == finish:
            finish = self.intervals.pop(index)[1]
        self.intervals.insert(index, (start, finish))

    def last_finish(self):
        return self.intervals[-1][1] if self.intervals else 0

    def discard_before(self, now):
        index = bisect.bisect_left(self.intervals, (now,))
        if index and self.intervals[index - 1][1] > now:
            index -= 1
        if index:
            del self.intervals[:index]
        if not self.intervals and self.active is not None:
            self.active.discard(self)


class ProcessCsvWriter:
    """Ordered, bounded batches to a separate CSV serialization process.

    Only CSV encoding/I/O is delegated. Parent scheduling, task IDs and
    reservations remain sequential. Pickle input comes only from the parent.
    """
    def __init__(self, path, fields, chunk_rows=4096):
        self.fieldnames = list(fields)
        self.buffer = []
        self.chunk_rows = chunk_rows
        self.proc = subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve()), "--_csv-worker", str(path)],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.closed = False
        # Header is explicit so the same worker supports full or minimal DAGs.
        pickle.dump(self.fieldnames, self.proc.stdin, protocol=pickle.HIGHEST_PROTOCOL)
        self.proc.stdin.flush()

    def writeheader(self):
        # The worker writes exactly the same header with the same csv dialect.
        pass

    def writerow(self, row):
        if set(row) - set(self.fieldnames):
            raise ValueError("unexpected DAG output fields")
        self.write_values(tuple(row.get(name, "") for name in self.fieldnames))

    def write_values(self, values):
        self.buffer.append(values)
        if len(self.buffer) >= self.chunk_rows:
            self.flush()

    def flush(self):
        if not self.buffer:
            return
        if self.proc.poll() is not None:
            detail = self.proc.stderr.read().decode("utf-8", "replace").strip()
            raise OSError("CSV writer process stopped: " + detail)
        try:
            pickle.dump(self.buffer, self.proc.stdin, protocol=pickle.HIGHEST_PROTOCOL)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            self.proc.wait()
            detail = self.proc.stderr.read().decode("utf-8", "replace").strip()
            raise OSError("CSV writer process failed: " + detail) from None
        self.buffer = []

    def close(self):
        if self.closed:
            return
        self.flush()
        try:
            pickle.dump(None, self.proc.stdin, protocol=pickle.HIGHEST_PROTOCOL)
            self.proc.stdin.close()
        except (BrokenPipeError, OSError):
            self.proc.wait()
            detail = self.proc.stderr.read().decode("utf-8", "replace").strip()
            raise OSError("CSV writer process failed: " + detail) from None
        status = self.proc.wait()
        detail = self.proc.stderr.read().decode("utf-8", "replace").strip()
        self.proc.stderr.close()
        self.closed = True
        if status:
            raise OSError("CSV writer process failed: " + detail)

    def abort(self):
        if self.closed:
            return
        if self.proc.poll() is None:
            self.proc.terminate()
        self.proc.wait()
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.stderr.close()
        self.closed = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        if exc_type is not None:
            self.abort()
        else:
            try:
                self.close()
            except BaseException:
                self.abort()
                raise
        return False


def csv_worker(path):
    # Internal subprocess entry point. No model, cache or DAG scheduling runs
    # here. A chunk is bounded by ProcessCsvWriter.chunk_rows in the parent.
    with Path(path).open("w", encoding="utf-8", newline="", buffering=1024 * 1024) as stream:
        writer = csv.writer(stream)
        writer.writerow(pickle.load(sys.stdin.buffer))
        while True:
            rows = pickle.load(sys.stdin.buffer)
            if rows is None:
                break
            writer.writerows(rows)


def multiple_cpus_available():
    try:
        count = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        count = os.cpu_count() or 1
    if count < 2:
        return False
    # Respect a directly visible cgroup CPU quota as well as affinity.
    try:
        quota, period = Path("/sys/fs/cgroup/cpu.max").read_text().split()
        if quota != "max" and int(quota) < 2 * int(period):
            return False
    except (OSError, ValueError):
        try:
            quota = int(Path("/sys/fs/cgroup/cpu/cpu.cfs_quota_us").read_text())
            period = int(Path("/sys/fs/cgroup/cpu/cpu.cfs_period_us").read_text())
            if quota >= 0 and quota < 2 * period:
                return False
        except (OSError, ValueError):
            pass
    return True


class DagWriter:
    """Streaming rows plus open-loop endpoint forecasts (not ns-3 results)."""
    def __init__(self, model, writer, on_progress=None):
        self.model, self.writer = model, writer
        self.on_progress = on_progress
        output_fields = list(writer.fieldnames) if isinstance(writer, (csv.DictWriter, ProcessCsvWriter)) else TASK_FIELDS
        # Always use logical group builders, including diagnostic full mode.
        self.light_output = True
        internal_fields = list(dict.fromkeys(output_fields + ["src_port", "dst_port"]))
        self.empty_row = dict.fromkeys(internal_fields, "")
        self.row_templates = OrderedDict()
        self.row_values = itemgetter(*output_fields)
        # Preserve csv's dialect/quoting/newline behavior; skip DictWriter's
        # per-column Python generator when the complete task schema matches.
        self.fast_write = (writer.write_values if isinstance(writer, ProcessCsvWriter) else
                           writer.writer.writerow if isinstance(writer, csv.DictWriter)
                           and list(writer.fieldnames) in (LOGICAL_FULL_FIELDS, LOGICAL_TRAFFIC_FIELDS) else None)
        bandwidth = min(model.storage_port_bandwidth_gbps, model.compute_port_bandwidth_gbps)
        self.flow_duration = lru_cache(maxsize=4096)(
            lambda n: model.transfer_duration_us(n, bandwidth))
        self.next_id = 0
        self.finish = {}
        self.persistent = set()
        self.previous_retained = set()
        self.emitted = []
        self.active_calendars = set()
        self.ingress, self.egress, self.storage = (
            defaultdict(lambda: PortCalendar(self.active_calendars)) for _ in range(3))
        self.counts, self.bytes = Counter(), Counter()
        self.physical_counts = Counter()

    def add(self, base, kind, stage, deps=(), release="", duration=0, **fields):
        if isinstance(deps, (list, tuple)) and len(deps) <= 1:
            deps = [deps[0]] if deps and deps[0] is not None else []
        else:
            deps = sorted(set(d for d in deps if d is not None))
        for dep in deps:
            if dep not in self.finish:
                raise RuntimeError(f"unresolved/forward predecessor in {stage}: {deps}")
        # Long prefixes can otherwise create a megabyte-sized depends_on CSV
        # cell. Hierarchical ALL barriers preserve readiness and stay readable
        # by csv.DictReader with its default 128-KiB field limit.
        while len(deps) > MAX_DEPENDENCY_FANIN:
            deps = [self.add(base, "TIMER", "DEPENDENCY_BARRIER",
                    deps[i:i + MAX_DEPENDENCY_FANIN],
                    comment=f"Bounded ALL fan-in for {stage}")
                    for i in range(0, len(deps), MAX_DEPENDENCY_FANIN)]
        # Keep only 256 immutable batch/request base templates. References
        # accompany id keys so object-ID reuse can never reuse stale metadata.
        key = id(base)
        cached = self.row_templates.get(key)
        if cached is None or cached[0] is not base:
            template = self.empty_row.copy()
            template.update({k: v for k, v in base.items() if k in self.empty_row} if self.light_output else base)
            self.row_templates[key] = (base, template)
            if len(self.row_templates) > 256:
                self.row_templates.popitem(last=False)
        else:
            template = cached[1]
        row = template.copy()
        row.update(task_id=self.next_id, task_class=kind, task_type=stage,
                   release_time_us=release, duration_us=int(duration), bytes=0,
                   storage_read_latency_us=0,
                   depends_on=str(deps[0]) if len(deps) == 1 else join_deps(deps))
        row.update({k: v for k, v in fields.items() if k in self.empty_row} if self.light_output else fields)
        ready = int(release or 0)
        if len(deps) == 1:
            ready = max(ready, self.finish[deps[0]])
        elif deps:
            ready = max(ready, max(self.finish[d] for d in deps))
        if kind == "NETWORK":
            src = (row["src_node"], row["src_port"])
            dst = (row["dst_node"], row["dst_port"])
            read = stage in {"PREFIX_READ", "STORAGE_TO_D"}
            compute, storage = (dst, src) if read else (src, dst)
            calendar = self.ingress if read else self.egress
            ready += int(row["storage_read_latency_us"])
            flow_duration = self.flow_duration(row["bytes"])
            compute_calendar, storage_calendar = calendar[compute], self.storage[storage]
            start = ready
            while True:
                candidate = max(compute_calendar.first_free(start, flow_duration),
                                storage_calendar.first_free(start, flow_duration))
                if candidate == start:
                    break
                start = candidate
            finish = start + flow_duration
            compute_calendar.reserve(start, finish)
            storage_calendar.reserve(start, finish)
        else:
            start, finish = ready, ready + int(duration)
        row.update(forecast_start_us=start, forecast_finish_us=finish)
        if self.fast_write is None:
            self.writer.writerow(row)
        else:
            # Unknown fields still fail as in DictWriter(extrasaction="raise").
            if len(row) != len(self.empty_row):
                raise ValueError("unexpected DAG output fields")
            self.fast_write(self.row_values(row))
        self.finish[self.next_id] = finish
        self.emitted.append(self.next_id)
        if stage == "INPUT_BLOCK_READY":
            self.persistent.add(self.next_id)
        self.counts[stage] += 1
        if kind == "NETWORK":
            self.bytes[stage] += row["bytes"]
        tid = self.next_id
        self.next_id += 1
        if self.on_progress is not None and self.next_id % 512 == 0:
            self.on_progress(stage, base.get("batch_id", ""), base.get("batch_size", ""))
        return tid


    def add_group(self, base, kind, stage, deps, common, members, duration=0):
        """One logical ALL-completion node, exact ordered rank forecasts.

        tp_members entries: [rank, src_node, src_port, dst_node, dst_port, bytes].
        Every downstream reference waits for ALL nonempty members. This relies
        on v14's layer barriers and whole-group KV publication semantics.
        """
        if not members:
            return []
        deps = sorted(set(d for d in deps if d is not None))
        if len(deps) > MAX_DEPENDENCY_FANIN:
            deps = [self.add(base, "TIMER", "DEPENDENCY_BARRIER",
                            deps[i:i + MAX_DEPENDENCY_FANIN])
                    for i in range(0, len(deps), MAX_DEPENDENCY_FANIN)]
        for dep in deps:
            if dep not in self.finish:
                raise RuntimeError(f"unresolved/forward predecessor in {stage}: {deps}")
        row = self.empty_row.copy()
        row.update({k: v for k, v in base.items() if k in row})
        row.update({k: v for k, v in common.items() if k in row})
        row.update(task_id=self.next_id, task_class=kind, task_type=stage,
                   release_time_us="", duration_us=int(duration), bytes=0,
                   depends_on=join_deps(deps))
        ready = max([0] + [self.finish[d] for d in deps])
        if kind == "NETWORK":
            ready += int(row.get("storage_read_latency_us") or 0)
            read = stage in {"PREFIX_READ", "STORAGE_TO_D"}
            calendars = self.ingress if read else self.egress
            earliest, latest, total = None, ready, 0
            for rank, src_node, src_port, dst_node, dst_port, nbytes in members:
                src = (src_node, src_port)
                dst = (dst_node, dst_port)
                if nbytes <= 0:
                    raise ValueError("logical NETWORK members need positive bytes")
                compute, storage = (dst, src) if read else (src, dst)
                compute_calendar = calendars[compute]
                storage_calendar = self.storage[storage]
                flow_duration = self.flow_duration(nbytes)
                start = ready
                while True:
                    candidate = max(compute_calendar.first_free(start, flow_duration),
                                    storage_calendar.first_free(start, flow_duration))
                    if candidate == start:
                        break
                    start = candidate
                finish = start + flow_duration
                compute_calendar.reserve(start, finish)
                storage_calendar.reserve(start, finish)
                earliest = start if earliest is None else min(earliest, start)
                latest = max(latest, finish)
                total += nbytes
            row.update(src_node=members[0][1], dst_node=members[0][3], bytes=total,
                       tp_members=json.dumps(members, separators=(",", ":")),
                       forecast_start_us=earliest, forecast_finish_us=latest)
            self.bytes[stage] += total
        elif kind == "COMPUTE":
            # Same ready time and duration for all ranks: their ALL completion
            # is exactly one logical computation with the original duration.
            row.update(forecast_start_us=ready, forecast_finish_us=ready + int(duration))
        else:
            raise ValueError("logical TP groups support NETWORK or COMPUTE only")
        if self.fast_write is None:
            self.writer.writerow({key: row[key] for key in self.writer.fieldnames})
        else:
            self.fast_write(self.row_values(row))
        self.finish[self.next_id] = row["forecast_finish_us"]
        self.emitted.append(self.next_id)
        self.counts[stage] += 1
        self.physical_counts[stage] += len(members)
        tid = self.next_id
        self.next_id += 1
        if self.on_progress is not None and self.next_id % 512 == 0:
            self.on_progress(stage, base.get("batch_id", ""), base.get("batch_size", ""))
        return [tid]

    def prune(self, retained):
        """Keep only cross-batch dependencies, not every emitted token/layer."""
        for tid in (self.previous_retained | set(self.emitted)) - retained - self.persistent:
            self.finish.pop(tid, None)
        self.previous_retained = retained
        self.emitted = []

    def prune_calendars(self, now):
        # Placement queries create many empty endpoint calendars. They have
        # no reservations to retire and need not be scanned on every step.
        for calendar in tuple(self.active_calendars):
            calendar.discard_before(now)


class Placement:
    """Use V14 placement scores; enforce independent local engine capacities.

    P candidates are finite resources. Network scores are estimates only.
    One D admission cohort shares a group; each member keeps it until retirement.
    """
    def __init__(self, model, dag, candidate_writer):
        self.model, self.dag, self.writer = model, dag, candidate_writer
        self.rr = {"P": 0, "D": 0}
        self.p_assignments = Counter()
        self.d_assignments = Counter()

    def assign_prefill_requests(self, admitted, domains, now, batch_id):
        if not admitted:
            raise ValueError("cannot place an empty Prefill wave")
        cap = self.model.p_max_num_seqs
        domain, gi = self.choose_batch(admitted[:cap], domains, "P", now, batch_id)
        if any(engine.residents for engine in domain.engines):
            raise RuntimeError("previous Prefill local batches have not released")
        used = set()
        for offset in range(0, len(admitted), cap):
            cohort = admitted[offset:offset + cap]
            if offset:
                _, gi = self.choose_batch(cohort, [domain], "P", now, batch_id,
                                          exclude_groups=used)
            used.add(gi)
            domain.engines[gi].admit(cohort)
            for state in cohort:
                state.update(p_tp_group=gi, p_service_domain=domain.id,
                             p_tp_group_id=domain.group_id(gi), p_tp_endpoints=domain.groups[gi])
                self.p_assignments[domain.group_id(gi)] += 1
        return domain

    def assign_decode_requests(self, admitted, domain, now, batch_id):
        """Route FIFO cohorts to load-selected engines with sufficient slots.

        The local limit is never multiplied into a single engine's batch size.
        Existing requests never migrate. Recompute V14 load scores after each
        new cohort so later placements see all earlier admissions.
        """
        offset = 0
        while offset < len(admitted):
            room = max(engine.slots for engine in domain.engines)
            if room <= 0:
                raise RuntimeError("Decode admission exceeded all local engine capacities")
            count = min(room, len(admitted) - offset)
            cohort = admitted[offset:offset + count]
            self._assign_decode_cohort(cohort, domain, now, batch_id)
            gi = cohort[0]["d_tp_group"]
            domain.engines[gi].admit(cohort)
            domain.residents.extend(cohort)
            offset += count

    def _assign_decode_cohort(self, admitted, domain, now, batch_id):
        """Place all newly admitted requests together, once, without KV migration.

        Lexicographic load: resident KV bytes, remaining decode steps,
        resident request count, predicted input-read completion, server KV
        load, rotating server/group tie-breaks. Forecasts are open-loop estimates.
        Equal-load groups therefore spread across servers before reusing a
        loaded server. No write/calendar reservation is invented here.
        """
        if not admitted:
            return
        kv_load, work_load, counts, server_kv = Counter(), Counter(), Counter(), Counter()
        group_nodes = [tuple(sorted({ep[0] for ep in group})) for group in domain.groups]
        servers = sorted({node for nodes in group_nodes for node in nodes})
        server_indices = {node: index for index, node in enumerate(servers)}

        def account(state):
            gi = state["d_tp_group"]
            if state["d_service_domain"] != domain.id or not 0 <= gi < len(domain.groups):
                raise RuntimeError("Decode request is pinned outside its service domain")
            kv = (state["req"]["input_length"] + state["token"]) * self.model.kv_bytes_per_token
            kv_load[gi] += kv
            work_load[gi] += state["req"]["output_length"] - state["token"]
            counts[gi] += 1
            # A spanning group charges the same group footprint to every
            # participating server; this is a placement score, not HBM usage.
            for node in group_nodes[gi]:
                server_kv[node] += kv

        for state in domain.residents:
            account(state)
        degree = domain.degree
        bandwidth = min(self.model.storage_port_bandwidth_gbps,
                        self.model.compute_port_bandwidth_gbps)
        loads = [0] * degree
        peer, has_reads = now, False
        for state in admitted:
            if state["token"] != 0 or "d_tp_group" in state:
                raise RuntimeError("Only an unbound new Decode request may select a TP group")
            for block in state["blocks"]:
                has_reads = True
                storage = self.model.choose_storage_endpoint(block["hash_id"])
                peer = max(peer, self.dag.storage[storage].first_free(now, 1))
                for rank in range(degree):
                    _, size = split_exact(block["tokens"] * self.model.kv_bytes_per_token, degree, rank)
                    loads[rank] += self.model.transfer_duration_us(size, bandwidth)
        access = self.model.storage_latency_us if has_reads else 0
        candidates = []
        for gi, endpoints in enumerate(domain.groups):
            if domain.engines[gi].slots < len(admitted):
                continue
            estimated = max(self.dag.ingress[ep].first_free(peer + access, max(1, loads[rank]))
                            + loads[rank] for rank, ep in enumerate(endpoints))
            host_load = max(server_kv[node] for node in group_nodes[gi])
            server_rank = min((server_indices[node] - domain.decode_server_rr) % len(servers)
                              for node in group_nodes[gi])
            score = (kv_load[gi], work_load[gi], counts[gi], estimated,
                     host_load, server_rank, (gi - domain.rr) % len(domain.groups))
            candidates.append((score, gi, estimated, host_load))
        best = min(candidates, key=lambda item: item[0])
        request_ids = json.dumps([s["req"]["request_id"] for s in admitted], separators=(",", ":"))
        for item in candidates:
            score, gi, estimated, host_load = item
            self.writer.writerow(dict(batch_id=batch_id, role="D", request_ids=request_ids,
                request_id=admitted[0]["req"]["request_id"] if len(admitted) == 1 else "",
                service_domain=domain.id, tp_group_id=domain.group_id(gi),
                tp_endpoints=json.dumps(domain.groups[gi]), compute_queue_us=0,
                estimated_read_finish_us=estimated, score=json.dumps(score), selected=int(item is best),
                resident_kv_bytes=kv_load[gi], remaining_decode_steps=work_load[gi],
                resident_requests=counts[gi], server_resident_kv_bytes=host_load,
                placement_policy=self.model.decode_tp_assignment))
        _, gi, _, _ = best
        for state in admitted:
            state.update(d_tp_group=gi, d_service_domain=domain.id,
                         d_tp_group_id=domain.group_id(gi), d_tp_endpoints=domain.groups[gi])
            self.d_assignments[domain.group_id(gi)] += 1
        domain.rr = (gi + 1) % len(domain.groups)
        domain.decode_server_rr = (server_indices[group_nodes[gi][-1]] + 1) % len(servers)

    def choose_batch(self, states, domains, role, now, batch_id, pinned=None, exclude_groups=()):
        policy = self.model.prefill_scheduler if role == "P" else self.model.decode_scheduler
        candidates = []
        degree = self.model.p_tp_degree if role == "P" else self.model.d_tp_degree
        # Logical traffic and producer readiness do not depend on which TP
        # group is scored. Compute them once, not once per 32/128 candidates.
        loads = [0] * degree
        peer = now
        has_reads = False
        for state in states:
            if role == "D" and state["token"] != 0:
                continue
            for block in state["blocks"]:
                if role == "P" and not block["hit"]:
                    continue
                has_reads = True
                storage = self.model.choose_storage_endpoint(block["hash_id"])
                peer = max(peer, self.dag.storage[storage].first_free(now, 1))
                entry = block.get("producer_entry")
                if role == "P" and entry and entry["producer"] is not None:
                    peer = max(peer, self.dag.finish[entry["producer"]])
                for rank in range(degree):
                    _, n = split_exact(block["tokens"] * self.model.kv_bytes_per_token, degree, rank)
                    loads[rank] += self.model.transfer_duration_us(n, min(
                        self.model.storage_port_bandwidth_gbps,
                        self.model.compute_port_bandwidth_gbps))
        access = self.model.storage_latency_us if has_reads else 0
        for di, domain in enumerate(domains):
            if domain.available_us > now:
                continue
            indices = [pinned] if pinned is not None else range(len(domain.groups))
            for gi in indices:
                if gi in exclude_groups:
                    continue
                endpoints = domain.groups[gi]
                estimated = max(self.dag.ingress[ep].first_free(peer + access, max(1, loads[r])) + loads[r]
                                for r, ep in enumerate(endpoints))
                rr_rank = ((domain.index - self.rr[role]) % FIXED_SERVICE_DOMAIN_COUNT,
                           (gi - domain.rr) % len(domain.groups))
                if policy == "round-robin":
                    score = rr_rank
                elif policy == "random":
                    score = (stable_hash_int(role, self.model.random_seed, batch_id, domain.id, gi),)
                elif policy == "least-load":
                    score = (max(self.dag.ingress[ep].last_finish() + self.dag.egress[ep].last_finish() for ep in endpoints),)
                else:
                    score = (estimated,)
                candidates.append((score, rr_rank, domain, gi, estimated))
        if not candidates:
            raise RuntimeError("no available domain candidate")
        best = min(candidates, key=lambda c: (c[0], c[1]))
        for item in candidates:
            score, _, domain, gi, estimated = item
            self.writer.writerow(dict(batch_id=batch_id, role=role, service_domain=domain.id,
                tp_group_id=domain.group_id(gi), tp_endpoints=json.dumps(domain.groups[gi]),
                compute_queue_us=0, estimated_read_finish_us=estimated,
                score=json.dumps(score), selected=int(item is best)))
        _, _, domain, gi, _ = best
        self.rr[role] = (domain.index + 1) % FIXED_SERVICE_DOMAIN_COUNT
        domain.rr = (gi + 1) % len(domain.groups)
        return domain, gi


class BatchBuilder:
    def __init__(self, model, dag, writer, prefill_layer_writer=None,
                 decode_writer=None, decode_layer_writer=None):
        self.model, self.dag, self.writer = model, dag, writer
        self.prefill_layer_writer = prefill_layer_writer
        self.prefill_layer_compute_rows = 0
        self.prefill_compute_totals = Counter()
        self.decode_writer, self.decode_layer_writer = decode_writer, decode_layer_writer
        self.decode_compute_totals = Counter()
        self.decode_layer_compute_rows = 0
        self.decode_merged_steps, self.decode_layered_steps = 0, 0
        self.decode_input_read_steps = 0
        self.batch_id = 0
        self.sizes = {"P": [], "D": []}
        self.token_shards = {}
        self.owned_base = None
        self.owned_rows = {}

    def base(self, states, domain, gi, role, step):
        endpoints = domain.groups[gi] if gi is not None else ()
        return dict(phase="FORMAL", request_id="", request_ids=json.dumps([s["req"]["request_id"] for s in states]),
            batch_id=self.batch_id, batch_size=len(states), batch_type="PREFILL" if role == "P" else "DECODE",
            batch_step_index=step, tp_degree=domain.degree,
            tp_group_id=domain.group_id(gi) if gi is not None else "MIXED",
            service_domain=domain.id, kv_rank_layout=self.model.kv_rank_layout,
            layer_count=self.model.num_layers, dependency_mode="ALL", dependency_event="COMPLETE",
            execution_schema=SCHEMA,
            **({"p_group": domain.id, "p_node": endpoints[0][0] if endpoints else "",
                "p_port": endpoints[0][1] if endpoints else ""} if role == "P"
               else {"d_group": domain.id, "d_node": endpoints[0][0] if endpoints else "",
                     "d_port": endpoints[0][1] if endpoints else ""}))

    def owned(self, base, state):
        # Batch metadata is immutable. Reuse the request-owned base across
        # its block/layer/rank rows; DagWriter copies it before row updates.
        if self.owned_base is not base:
            self.owned_base = base
            self.owned_rows = {}
        rid = state["req"]["request_id"]
        owned = self.owned_rows.get(rid)
        if owned is None:
            owned = dict(base, request_id=rid, request_ids=json.dumps([rid]))
            if base["batch_type"] == "PREFILL":
                if state["p_service_domain"] != base["service_domain"]:
                    raise RuntimeError("Prefill request is outside its EP domain")
                endpoint = state["p_tp_endpoints"][0]
                owned.update(tp_group_id=state["p_tp_group_id"], p_node=endpoint[0], p_port=endpoint[1])
            elif base["batch_type"] == "DECODE":
                if state["d_service_domain"] != base["service_domain"]:
                    raise RuntimeError("Decode batch contains a request from a different domain")
                endpoint = state["d_tp_endpoints"][0]
                owned.update(tp_group_id=state["d_tp_group_id"], d_node=endpoint[0], d_port=endpoint[1])
            self.owned_rows[rid] = owned
        return owned

    def network(self, base, state, domain, gi, stage, block, layer, deps, wave, token="", key=None):
        read = stage in {"PREFIX_READ", "STORAGE_TO_D"}
        key = block["hash_id"] if key is None else key
        storage = self.model.choose_storage_endpoint(key)
        # Canonical address is (key, layer, byte range). P/D may have different
        # TP degrees without inventing incompatible per-rank storage objects.
        layer_bytes = self.model.layer_bytes(block["tokens"] * self.model.kv_bytes_per_token, layer)
        common = dict(network_wave=wave, layer_index=layer,
            block_index=block["block_index"], decode_token_index=token,
            storage_read_latency_us=self.model.storage_latency_us if read else 0)
        members = []
        q, r = divmod(layer_bytes, domain.degree)
        for rank, worker in enumerate(domain.groups[gi]):
            nbytes = q + int(rank < r)
            if nbytes == 0:
                continue
            src, dst = (storage, worker) if read else (worker, storage)
            members.append([rank, src[0], src[1], dst[0], dst[1], nbytes])
        return self.dag.add_group(self.owned(base, state), "NETWORK", stage, deps, common, members)

    def decode_request_write(self, base, state, domain, gi, compute_done):
        """Gather all output-token KV once, after the request's last Compute.

        Preserve the per-token layer/rank integer partition before multiplying
        by output length. Repartitioning the aggregate could move rounding
        bytes between ranks. Input KV is already stored and is not written again.
        The request's complete output is one storage object, shared by its ranks.
        """
        tokens = state["req"]["output_length"]
        if state["token"] != tokens or tokens <= 0:
            raise RuntimeError("Decode output write requires all output tokens computed")
        degree = domain.degree
        if degree not in self.token_shards:
            self.token_shards[degree] = [sum(
                split_exact(self.model.layer_bytes(self.model.kv_bytes_per_token, layer), degree, rank)[1]
                for layer in range(self.model.num_layers)) for rank in range(degree)]
        key = f"decode:{state['req']['request_id']}:output"
        storage = self.model.choose_storage_endpoint(key)
        common = dict(dst_node=storage[0], dst_port=storage[1], storage_read_latency_us=0,
            layer_index=-1, block_index=-1, decode_token_index=tokens - 1,
            storage_key=key, kv_extent_kind="all_output_tokens_all_layers",
            kv_token_offset=0, kv_token_bytes=self.model.kv_bytes_per_token,
            network_wave=f"D:{base['batch_id']}:{self.model.num_layers}")
        members = []
        for rank, worker in enumerate(domain.groups[gi]):
            nbytes = self.token_shards[degree][rank] * tokens
            if nbytes:
                members.append([rank, worker[0], worker[1], storage[0], storage[1], nbytes])
        return self.dag.add_group(self.owned(base, state), "NETWORK", "DECODE_WRITE",
                                  [compute_done], common, members)

    def compute_layer(self, base, domain, gi, role, layer, deps, duration):
        start = self.dag.add(base, "TIMER", f"{role}_LAYER_START", deps, layer_index=layer)
        ranks = self.dag.add_group(base, "COMPUTE", "PREFILL_LAYER" if role == "P" else "DECODE_LAYER",
                 [start], {"layer_index": layer}, [{} for rank in range(domain.degree)],
                 duration=duration)
        ready = self.dag.add(base, "TIMER", f"{role}_LAYER_READY", ranks, layer_index=layer)
        return start, ready

    def record(self, base, states, domain, gi, role, now, duration, begin, free, previous, first_start):
        request_groups = {str(s["req"]["request_id"]): s["p_tp_group_id"] if role == "P"
                          else s["d_tp_group_id"] for s in states}
        endpoints = sorted({ep for s in states for ep in s["p_tp_endpoints" if role == "P" else "d_tp_endpoints"]})
        local_sizes = Counter(request_groups.values())
        local_limit = self.model.p_max_num_seqs if role == "P" else self.model.d_max_num_seqs
        if any(size > local_limit for size in local_sizes.values()):
            raise RuntimeError("EP wave contains an oversized local DP batch")
        self.writer.writerow(dict(batch_id=self.batch_id, batch_type=base["batch_type"],
            batch_step_index=base["batch_step_index"], request_ids=base["request_ids"],
            batch_size=len(states), service_domain=domain.id, tp_group_id=base["tp_group_id"],
            tp_degree=domain.degree, tp_endpoints=json.dumps(endpoints),
            local_batch_sizes=json.dumps(local_sizes, separators=(",", ":")),
            dp_engine_count=len(domain.engines), local_batch_limit=local_limit,
            batch_size_scope="aggregate_of_local_DP_batches_in_shared_EP_wave",
            request_tp_groups=json.dumps(request_groups, separators=(",", ":")),
            input_tokens=sum(s["req"]["input_length"] for s in states),
            miss_tokens=sum(s["debug"]["miss_tokens"] for s in states) if role == "P" else 0,
            decode_token_indices=json.dumps({str(s["req"]["request_id"]): s["token"] for s in states}) if role == "D" else "",
            compute_duration_us=duration, dispatch_us=now,
            forecast_compute_start_us=self.dag.finish[first_start],
            forecast_compute_finish_us=self.dag.finish[free], begin_task_id=begin,
            compute_done_task_id=free, previous_domain_compute_done_task_id=previous if previous is not None else ""))
        domain.available_us = self.dag.finish[free]
        domain.last_compute_done = free
        self.sizes[role].append(len(states))
        self.batch_id += 1

    def prefill(self, states, domain, now):
        groups = {state["p_tp_group"] for state in states}
        gi = next(iter(groups)) if len(groups) == 1 else None
        base = self.base(states, domain, gi, "P", "")
        # Batch-level TIMER tasks need a valid ns-3 execution context even
        # when the batch spans more than one TP group (gi is then None).
        if not base["p_node"]:
            if not states or not states[0]["p_tp_endpoints"]:
                raise RuntimeError("Prefill batch has no representative TP endpoint for TIMER tasks")
            base["p_node"], base["p_port"] = states[0]["p_tp_endpoints"][0]
        previous = domain.last_compute_done
        begin = self.dag.add(base, "TIMER", "P_BATCH_BEGIN", [previous], release=now,
                             comment="Immediate dispatch; domain compute predecessor enforced.")
        all_reads = defaultdict(list)
        all_writes = defaultdict(list)
        by_block = defaultdict(list)

        def reads(layer, gate):
            current = []
            for state in states:
                rid = state["req"]["request_id"]
                for block in state["blocks"]:
                    if not block["hit"]:
                        continue
                    entry = block.get("producer_entry")
                    producer = entry["producer"] if entry else None
                    ids = self.network(base, state, domain, state["p_tp_group"], "PREFIX_READ", block, layer,
                        [gate, producer], f"P:{self.batch_id}:{layer}")
                    current.extend(ids)
                    all_reads[rid].extend(ids)
            return current

        if self.model.prefill_compute_mode == "glm51_flops":
            layer_rows, duration = self.model.prefill_layer_compute(states)
            durations = [row["layer_compute_us"] for row in layer_rows]
            for row in layer_rows:
                output_row = dict(
                    batch_id=self.batch_id,
                    service_domain=domain.id,
                    **row,
                )
                if self.prefill_layer_writer is not None:
                    self.prefill_layer_writer.writerow(output_row)
                self.prefill_layer_compute_rows += 1
                for key in (
                    "mla_projection_flops", "indexer_projection_flops", "indexer_score_flops",
                    "sparse_attention_flops", "attention_flops", "dense_ffn_flops",
                    "router_flops", "routed_expert_flops", "shared_expert_flops",
                    "layer_compute_us",
                ):
                    self.prefill_compute_totals[key] += row.get(key, 0)
        else:
            duration = self.model.prefill_duration_us(
                sum(s["debug"]["miss_tokens"] for s in states)
            )
            durations = split_duration(duration, self.model.num_layers)
        current_reads = reads(0, begin)
        ready, first_start = begin, None
        for layer in range(self.model.num_layers):
            start, ready = self.compute_layer(base, domain, gi, "P", layer, [ready] + current_reads, durations[layer])
            if first_start is None:
                first_start = start
            for state in states:
                rid = state["req"]["request_id"]
                for block in state["blocks"]:
                    if not block["hit"]:
                        ids = self.network(base, state, domain, state["p_tp_group"], "PREFIX_STORE_WRITE", block, layer,
                            [ready], f"P:{self.batch_id}:{layer + 1}")
                        all_writes[rid].extend(ids)
                        by_block[(rid, block["block_index"])].extend(ids)
            current_reads = reads(layer + 1, start) if layer + 1 < self.model.num_layers else []
        free = self.dag.add(base, "TIMER", "P_COMPUTE_DONE", [ready])
        for state in states:
            rid = state["req"]["request_id"]
            done = self.dag.add(self.owned(base, state), "TIMER", "P_DONE",
                                [free] + all_reads[rid] + all_writes[rid])
            state["p_done"] = done
            state["p_dispatched"] = True
            for block in state["blocks"]:
                entry = state["produced"].get(hash_key(block["hash_id"]))
                if entry is not None and entry["owner"] == rid and entry["producer"] is None:
                    entry["producer"] = self.dag.add(self.owned(base, state), "TIMER", "INPUT_BLOCK_READY",
                        by_block[(rid, block["block_index"])], hash_id=block["hash_id"], block_index=block["block_index"],
                        comment="All layers and all TP shards published; canonical ASU object.")
            endpoint = state["p_tp_endpoints"][0]
            state["debug"].update(p_group=domain.id, p_node=endpoint[0], p_port=endpoint[1],
                p_batch_id=self.batch_id, p_tp_group_id=state["p_tp_group_id"], p_dispatch_us=now,
                service_start_us=self.dag.finish[first_start],
                p_compute_start_us=self.dag.finish[first_start], p_compute_finish_us=self.dag.finish[free],
                p_forecast_done_us=self.dag.finish[done], p_done_task_id=done,
                p_queue_us=now - state["req"]["timestamp_us"])
        self.record(base, states, domain, gi, "P", now, duration, begin, free, previous, first_start)
        for engine in domain.engines:
            engine.residents.clear()
        return free

    def decode_step(self, states, domain, now):
        groups = {s["d_tp_group"] for s in states}
        gi = next(iter(groups)) if len(groups) == 1 else None
        base = self.base(states, domain, gi, "D", domain.step)
        # Keep a representative endpoint for DAG/debug compatibility; Decode
        # Compute is folded into network delays by the traffic converter.
        if not base["d_node"]:
            if not states or not states[0]["d_tp_endpoints"]:
                raise RuntimeError("Decode batch has no representative TP endpoint")
            base["d_node"], base["d_port"] = states[0]["d_tp_endpoints"][0]
        previous = domain.last_compute_done
        deps = [previous] + [s["p_done"] if s["token"] == 0 else s["last_step"] for s in states]
        begin = self.dag.add(base, "TIMER", "D_BATCH_BEGIN", deps, release=now)
        all_resident = all(s["token"] > 0 for s in states)
        metrics, _, layer_rows = self.model.decode_compute(
            states, include_layers=self.decode_layer_writer is not None)
        duration = metrics["compute_us"]
        common = dict(batch_id=self.batch_id, batch_step_index=domain.step, service_domain=domain.id)
        if self.decode_writer is not None:
            self.decode_writer.writerow(dict(common, request_ids=base["request_ids"],
                                             compute_granularity="step", **metrics))
        if self.decode_layer_writer is not None:
            for row in layer_rows:
                self.decode_layer_writer.writerow(dict(
                    common, layer_index=row["layer_index"], layer_type=row["layer_type"],
                    new_tokens=row["miss_tokens"], cached_context_tokens=row["cached_tokens"],
                    **{key: row[key] for key in (*FLOP_METRIC_FIELDS, *TIME_METRIC_FIELDS,
                       "layer_compute_time_us", "layer_compute_us", "attention_critical_tp_group")}))
                self.decode_layer_compute_rows += 1
        for key in (*FLOP_METRIC_FIELDS, *TIME_METRIC_FIELDS, "compute_us"):
            self.decode_compute_totals[key] += metrics.get(key, 0)
        input_reads = []
        if not all_resident:
            # All physical block/layer/TP flows remain distinct, but none
            # waits for Decode computation or for another layer's Read.
            for layer in range(self.model.num_layers):
                for state in states:
                    if state["token"] != 0:
                        continue
                    for block in state["blocks"]:
                        input_reads.extend(self.network(
                            base, state, domain, state["d_tp_group"], "STORAGE_TO_D", block,
                            layer, [state["p_done"]], f"D:{self.batch_id}:{layer}", token=0))
            self.decode_input_read_steps += bool(input_reads)
        first_start = (self.dag.add(base, "TIMER", "D_STEP_START", [begin] + input_reads,
                                    layer_index=-1,
                                    comment="All input KV ready and preceding Decode Compute finished.")
                       if input_reads else begin)
        ready = self.dag.add(base, "COMPUTE", "DECODE_STEP_COMPUTE", [first_start],
                             duration=duration, layer_index=-1,
                             comment="One whole-model Decode step after all input KV Reads.")
        self.decode_merged_steps += 1
        free = self.dag.add(base, "TIMER", "D_COMPUTE_DONE", [ready])
        self.record(base, states, domain, gi, "D", now, duration, begin, free, previous, first_start)
        domain.step += 1
        for state in states:
            debug = state["debug"]
            if state["token"] == 0:
                debug.update(d_first_batch_id=base["batch_id"], d_group=domain.id,
                    d_node=state["d_tp_endpoints"][0][0], d_port=state["d_tp_endpoints"][0][1],
                    d_tp_group_id=state["d_tp_group_id"], d_dispatch_us=now,
                    d_queue_us=now - debug["p_forecast_done_us"])
            state["last_step"] = free
            state["token"] += 1
            debug["decode_compute_us"] += duration
            debug["generated_tokens"] = state["token"]
            debug["decode_step_count"] += 1
            debug["d_compute_finish_us"] = self.dag.finish[free]
            if state["token"] == state["req"]["output_length"]:
                writes = self.decode_request_write(base, state, domain, state["d_tp_group"], free)
                # Compute resources are already free; request completion alone
                # waits for all output TP ranks (and converter uint32 chunks).
                done = self.dag.add(self.owned(base, state), "TIMER", "D_DONE", writes)
                state["d_done"] = done
                debug.update(d_done_task_id=done, d_forecast_done_us=self.dag.finish[done])
        domain.residents = [s for s in domain.residents if s["token"] < s["req"]["output_length"]]
        for engine in domain.engines:
            engine.residents = [s for s in engine.residents
                                if s["token"] < s["req"]["output_length"]]
        return free


def _generate(reqs, model, out_dir, warmup=0, prewarm_ratio=0, rounding="ceil", progress=None,
              csv_writer_process=False, dag_detail="traffic", decode_compute_detail="none"):
    if dag_detail not in {"full", "traffic"}:
        raise ValueError("dag_detail must be full or traffic")
    if decode_compute_detail not in {"none", "layer"}:
        raise ValueError("decode_compute_detail must be none or layer")
    if not 0 <= warmup < len(reqs):
        raise ValueError("warmup requests must be nonnegative and smaller than the trace")
    if not math.isfinite(prewarm_ratio) or not 0 <= prewarm_ratio <= 1:
        raise ValueError("prewarm ratio must be finite and within [0,1]")
    if rounding not in {"ceil", "floor"}:
        raise ValueError("prewarm rounding must be ceil or floor")
    reqs = sorted(reqs, key=lambda r: (r["timestamp_us"], r["request_id"]))
    warmup_ids = {r["request_id"] for r in reqs[:warmup]}
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    cache, states, waiting_p, waiting_d = {}, {}, [], []
    domains_p = [Domain(g, model.p_tp_degree, model.p_max_num_seqs) for g in model.p_groups]
    domains_d = [Domain(g, model.d_tp_degree, model.d_max_num_seqs) for g in model.d_groups]
    events, event_id = [], 0

    def event(at, kind, payload=None):
        nonlocal event_id
        heapq.heappush(events, (int(at), event_id, kind, payload))
        event_id += 1

    by_ts = defaultdict(list)
    for req in reqs:
        by_ts[req["timestamp_us"]].append(req)
    for ts in sorted(by_ts):
        event(ts, "ARRIVAL", by_ts[ts])
    totals = Counter(requests=len(reqs))
    paths = ["task_dag.csv", "requests_debug.csv", "scheduler_candidates.csv", "batches_debug.csv",
             "prefill_layer_compute.csv", "decode_compute.csv"]
    from contextlib import ExitStack
    with ExitStack() as stack:
        writers = []
        field_sets = [
            LOGICAL_FULL_FIELDS if dag_detail == "full" else LOGICAL_TRAFFIC_FIELDS,
            DEBUG_FIELDS, CANDIDATE_FIELDS, BATCH_FIELDS, PREFILL_LAYER_COMPUTE_FIELDS, DECODE_COMPUTE_FIELDS,
        ]
        if decode_compute_detail == "layer":
            paths.append("decode_layer_compute.csv")
            field_sets.append(DECODE_LAYER_COMPUTE_FIELDS)
        for index, (name, names) in enumerate(zip(paths, field_sets)):
            if index == 0 and csv_writer_process:
                writers.append(stack.enter_context(ProcessCsvWriter(out_dir / name, names)))
            else:
                stream = stack.enter_context((out_dir / name).open(
                    "w", encoding="utf-8", newline="", buffering=1024 * 1024))
                writers.append(csv.DictWriter(stream, names))
        for writer in writers:
            writer.writeheader()
        tw, dw, cw, bw, pw, dcw, *detail_writers = writers
        dag = DagWriter(model, tw)
        placement = Placement(model, dag, cw)
        builder = BatchBuilder(model, dag, bw, pw, dcw, detail_writers[0] if detail_writers else None)

        def heartbeat(stage="", batch_id="", batch_size="", force=False):
            if progress is not None:
                detail = (f"{dag.next_id:,} tasks | warmup {totals['warmup_requests']:,}"
                          f" | P batches {len(builder.sizes['P']):,} | D steps {len(builder.sizes['D']):,}")
                if stage:
                    detail += f" | {stage} batch={batch_id} size={batch_size}"
                progress.update(totals["warmup_requests"] + totals["completed_formal_requests"],
                                detail, force=force)

        dag.on_progress = heartbeat

        def arrivals(group):
            # Same-timestamp requests see the same pre-insert cache. Oracle
            # injection is request-local during this snapshot, as in v13.
            pending = {}
            for req in group:
                rid = req["request_id"]
                is_warmup = rid in warmup_ids
                count = (req["input_length"] + model.block_tokens - 1) // model.block_tokens
                oracle_count = 0 if is_warmup else per_request_prewarm_block_count(
                    min(count, len(req["hash_ids"])), prewarm_ratio, rounding)
                injected = {}
                for index, h in enumerate(req["hash_ids"][:oracle_count]):
                    key = hash_key(h)
                    if key not in cache:
                        injected[key] = dict(tokens=min(model.block_tokens, req["input_length"] - index * model.block_tokens),
                                             producer=None, owner=None)
                visible = ChainMap(injected, cache)
                blocks = blocks_for(req, model, visible)
                for block in blocks:
                    if block["hit"]:
                        block["producer_entry"] = visible[hash_key(block["hash_id"])]
                debug = {name: "" for name in DEBUG_FIELDS}
                debug.update(request_id=rid, phase="WARMUP" if is_warmup else "FORMAL",
                    timestamp_us=req["timestamp_us"], input_length=req["input_length"], output_length=req["output_length"],
                    hit_blocks=sum(b["hit"] for b in blocks), miss_blocks=sum(not b["hit"] for b in blocks),
                    cached_tokens=sum(b["tokens"] for b in blocks if b["hit"]),
                    miss_tokens=sum(b["tokens"] for b in blocks if not b["hit"]), oracle_target_blocks=oracle_count,
                    decode_compute_us=0, generated_tokens=0, decode_step_count=0)
                if is_warmup:
                    for block in blocks:
                        if block["known_hash"]:
                            pending.setdefault(hash_key(block["hash_id"]), dict(tokens=block["tokens"], producer=None, owner=None))
                    debug.update(service_start_us=req["timestamp_us"], p_queue_us=0, d_queue_us=0)
                    dw.writerow(debug)
                    totals["warmup_requests"] += 1
                    continue
                produced = {}
                for key, entry in injected.items():
                    pending.setdefault(key, entry)
                for block in blocks:
                    if not block["hit"] and block["known_hash"]:
                        key = hash_key(block["hash_id"])
                        if key not in cache:
                            entry = pending.setdefault(key, dict(tokens=block["tokens"], producer=None, owner=rid))
                            if entry["owner"] == rid:
                                produced[key] = entry
                debug["prefill_compute_us"] = model.prefill_request_duration_us(
                    debug["miss_tokens"], debug["cached_tokens"]
                )
                state = dict(req=req, blocks=blocks, produced=produced, debug=debug, p_dispatched=False,
                             p_done=None, d_done=None, token=0, last_step=None, reported=False)
                states[rid] = state
                waiting_p.append(state)
                totals["formal_requests"] += 1
                totals["hit_blocks"] += debug["hit_blocks"]
                totals["miss_blocks"] += debug["miss_blocks"]
            for key, entry in pending.items():
                cache.setdefault(key, entry)

        def eligible(state):
            # A cache consumer cannot join the same P batch as its unresolved
            # producer: that would introduce a cycle in the shared compute.
            return all(not b["hit"] or b["producer_entry"]["owner"] is None
                       or b["producer_entry"]["producer"] is not None for b in state["blocks"])

        def retire(state):
            if not state["reported"]:
                dw.writerow(state["debug"])
                state["reported"] = True
                totals["completed_formal_requests"] += 1
                states.pop(state["req"]["request_id"], None)

        def retained_ids():
            ids = {d.last_compute_done for d in domains_p + domains_d}
            for state in states.values():
                if not state["reported"]:
                    ids.update([state["p_done"], state["last_step"], state["d_done"]])
            return ids - {None}

        while events:
            now = events[0][0]
            dag.prune_calendars(now)
            # Ingest every external arrival/completion at this time before
            # dispatch. Equal timestamps never depend on input line ordering.
            while events and events[0][0] == now:
                _, _, kind, payload = heapq.heappop(events)
                if kind == "ARRIVAL":
                    arrivals(payload)
                elif kind == "P_READY":
                    if payload["req"]["output_length"]:
                        waiting_d.append(payload)
                    else:
                        base = dict(phase="FORMAL", request_id=payload["req"]["request_id"],
                            request_ids=json.dumps([payload["req"]["request_id"]]), execution_schema=SCHEMA,
                            dependency_mode="ALL", dependency_event="COMPLETE")
                        done = dag.add(base, "TIMER", "D_DONE", [payload["p_done"]],
                                       comment="Zero output tokens; no Decode admission or KV read.")
                        payload["d_done"] = done
                        payload["debug"].update(d_done_task_id=done, d_forecast_done_us=dag.finish[done], d_queue_us=0)
                        retire(payload)
                elif kind == "REQUEST_DONE":
                    retire(payload)
                # RESOURCE_FREE simply provides a scheduling wakeup.
            while waiting_p and any(d.available_us <= now for d in domains_p):
                batch = []
                for state in waiting_p:
                    if eligible(state):
                        batch.append(state)
                        if len(batch) == model.p_domain_slot_bound:
                            break
                if not batch:
                    break
                domain = placement.assign_prefill_requests(batch, domains_p, now, builder.batch_id)
                batch = domain.local_wave("P")
                heartbeat("PREFILL_BUILD", builder.batch_id, len(batch), force=True)
                free = builder.prefill(batch, domain, now)
                selected = {id(s) for s in batch}
                waiting_p = [s for s in waiting_p if id(s) not in selected]
                event(dag.finish[free], "RESOURCE_FREE")
                for state in batch:
                    event(dag.finish[state["p_done"]], "P_READY", state)
            for domain in domains_d:
                if domain.available_us > now:
                    continue
                # Each engine independently owns at most BS resident requests.
                # The shared EP coordinator does not add another global BS cap.
                slots = sum(engine.slots for engine in domain.engines)
                admitted = waiting_d[:slots]
                waiting_d = waiting_d[slots:]
                placement.assign_decode_requests(admitted, domain, now, builder.batch_id)
                batch = domain.local_wave("D")
                if not batch:
                    continue
                heartbeat("DECODE_BUILD", builder.batch_id, len(batch))
                free = builder.decode_step(batch, domain, now)
                event(dag.finish[free], "RESOURCE_FREE")
                for state in batch:
                    if state["d_done"] is not None:
                        event(dag.finish[state["d_done"]], "REQUEST_DONE", state)
            dag.prune(retained_ids())
            heartbeat()
        if waiting_p or waiting_d or any(d.residents for d in domains_d):
            raise RuntimeError("scheduler stopped with unresolved requests")
        if totals["completed_formal_requests"] != totals["formal_requests"]:
            raise RuntimeError("not all formal requests completed")
        summary = dict(totals)
        summary.update(execution_schema=SCHEMA, dag_rows=dag.next_id, task_counts=dict(dag.counts),
            implementation_revision=VERSION, max_dependency_fanin=MAX_DEPENDENCY_FANIN,
            base_implementation_revision="v14.7-cohort-pinned-decode-tp-20261020",
            physical_tp_task_counts=dict(dag.physical_counts),
            network_rows=sum(dag.counts.get(k, 0) for k in dag.bytes),
            expanded_network_flows=sum(dag.physical_counts.get(k, 0) for k in dag.bytes),
            tp_representation="logical ALL-completion rows; exact rank members and ordered endpoint forecasts",
            bytes_by_type=dict(dag.bytes), total_network_bytes=sum(dag.bytes.values()),
            compute_concurrency=model.compute_concurrency, total_accelerators=model.total_accelerators,
            p_service_domains=model.p_domain_count, d_service_domains=model.d_domain_count,
            prefill_tp=model.p_tp_degree, decode_tp=model.d_tp_degree, ep=model.ep_degree,
            batch_limit_scope="per-attention-DP-engine",
            prefill_dp_degree=model.p_dp_degree, decode_dp_degree=model.d_dp_degree,
            prefill_domain_slot_upper_bound=model.p_domain_slot_bound,
            decode_domain_slot_upper_bound=model.d_domain_slot_bound,
            peak_residents_by_domain={d.id: d.peak_residents for d in domains_p + domains_d},
            peak_residents_by_engine={d.group_id(e.index): e.peak_residents
                                      for d in domains_p + domains_d for e in d.engines},
            local_batches_by_engine={d.group_id(e.index): e.local_batches
                                     for d in domains_p + domains_d for e in d.engines},
            capacity_limitations="slot upper bounds only; actual occupancy is trace/schedule-dependent; HBM capacity, memory admission and expert capacity are not modelled",
            residency_definition="generator-scheduled local compute/admission slots; excludes ingress waiting queues and final-write-only draining requests; not ns-3 measured concurrency or HBM occupancy",
            dp_compute_model=(
                "Prefill: " + model.prefill_compute_model_name + "; Decode: " + model.decode_compute_model_name
            ),
            prefill_max_batch=model.p_max_num_seqs, decode_max_batch=model.d_max_num_seqs,
            prefill_max_batch_tokens=None, prefill_wait_us=0,
            prefill_batches=len(builder.sizes["P"]), decode_batches=len(builder.sizes["D"]),
            avg_prefill_batch=sum(builder.sizes["P"]) / len(builder.sizes["P"]) if builder.sizes["P"] else 0,
            avg_decode_batch=sum(builder.sizes["D"]) / len(builder.sizes["D"]) if builder.sizes["D"] else 0,
            prefill_compute_model=model.prefill_compute_model_name,
            decode_compute_model=model.decode_compute_model_name,
            decode_compute_mode=model.decode_compute_mode,
            decode_first_step_us=model.first_decode_step_us,
            decode_per_token_us=model.decode_us_per_token,
            decode_compute_rows=len(builder.sizes["D"]),
            decode_layer_compute_rows=builder.decode_layer_compute_rows,
            decode_compute_detail=decode_compute_detail,
            decode_merged_steps=builder.decode_merged_steps,
            decode_layered_steps=builder.decode_layered_steps,
            decode_input_read_steps=builder.decode_input_read_steps,
            decode_input_read_policy="all layers read after own Prefill; whole-step Compute waits all batch input Reads and preceding Compute",
            decode_output_write_policy="once per request after its final Decode Compute; all output tokens and layers; TP-sharded",
            decode_output_write_bytes_scope="output_length * kv_bytes_per_token; excludes input KV; unscaled bytes",
            decode_output_storage_layout="one request output object keyed decode:<request_id>:output; block_index=-1",
            decode_compute_totals=dict(builder.decode_compute_totals),
            decode_compute_efficiency=model.decode_efficiency,
            decode_shared_expert_tp_degree=model.decode_shared_expert_tp_degree,
            decode_context_convention="H=input_length+completed_decode_steps cached keys; L=H+1 including current query key",
            decode_compute_time_interpretation=(
                "idealized arithmetic throughput only; excludes HBM traffic and TP/EP communication"
                if model.decode_compute_mode == "glm51_flops" else "legacy max-member-step duration"),
            prefill_layer_compute_rows=builder.prefill_layer_compute_rows,
            prefill_compute_flops_totals=dict(builder.prefill_compute_totals),
            prefill_npu_peak_flops=model.npu_peak_flops,
            prefill_npu_peak_ops_per_sec=(model.npu_peak_ops_per_sec if model.prefill_compute_mode == "glm51_flops" else None),
            prefill_npu_model=(model.npu_model if model.prefill_compute_mode == "glm51_flops" else None),
            prefill_npu_precision=(model.npu_precision if model.prefill_compute_mode == "glm51_flops" else None),
            prefill_npu_peak_unit=(model.npu_peak_unit if model.prefill_compute_mode == "glm51_flops" else None),
            prefill_npu_peak_source=(model.npu_peak_source if model.prefill_compute_mode == "glm51_flops" else None),
            prefill_npu_peak_is_public_default=(model.npu_peak_flops_is_public_default if model.prefill_compute_mode == "glm51_flops" else None),
            prefill_compute_time_interpretation=(
                "idealized peak-throughput lower bound; efficiency defaults to 1.0 unless overridden"
                if model.prefill_compute_mode == "glm51_flops" else "legacy linear model"
            ),
            prefill_efficiency_assumption=(
                model.prefill_compute_config.get(
                    "efficiency_assumption",
                    "efficiency values are modeling assumptions; default is ideal peak (1.0)"
                ) if model.prefill_compute_mode == "glm51_flops" else None
            ),
            prefill_model_architecture=model.glm51_arch,
            prefill_compute_efficiency=model.prefill_efficiency,
            prefill_expert_load_imbalance_factor=model.expert_load_imbalance_factor,
            prefill_shared_expert_tp_degree=model.shared_expert_tp_degree,
            indexer_key_cache_resident=model.indexer_key_cache_resident,
            requests_debug_prefill_compute_us_scope=(
                "isolated single-request estimate; batch wall-clock is in batches_debug.csv and prefill_layer_compute.csv"
                if model.prefill_compute_mode == "glm51_flops" else "legacy per-request linear estimate"
            ),
            prefill_flops_excluded_ops=(
                "RMSNorm/LayerNorm, RoPE, softmax, activation/ReLU, Top-K selection, residuals, and TP/EP communication"
                if model.prefill_compute_mode == "glm51_flops" else "not applicable"
            ),
            worker_placement="P: V14 policy selects a domain and bounded local TP cohorts; D: V14 load scores select locally available engines; requests pinned until completion",
            prefill_tp_admissions_by_group=dict(placement.p_assignments),
            decode_tp_assignment=model.decode_tp_assignment,
            decode_tp_admissions_by_group=dict(placement.d_assignments),
            decode_tp_load_score="resident KV bytes; remaining steps; request count; input-read forecast; server KV load; rotating server/group tie-breaks",
            cache_policy="v13-arrival-snapshot-inflight-wait; queued producers reserved",
            same_timestamp_cache="shared pre-insert snapshot; no intra-batch reuse",
            storage_layout="canonical input block/layer ranges; output token rank gathers layer ranges",
            decode_write_granularity="request-token-tp-rank-all-layers",
            decode_write_completion_gate="request D_DONE only; next step waits compute, not writeback",
            tp_internal_traffic_model="not-modelled", ep_internal_traffic_model="not-modelled",
            kv_rank_layout=model.kv_rank_layout, storage_read_latency_us=model.storage_latency_us,
            per_request_oracle_prewarm_ratio=prewarm_ratio,
            ignored_config_settings=model.ignored_config_settings,
            placement_predictions="open-loop endpoint estimates, not ns-3 execution timestamps",
            outputs=paths + ["summary.json", "effective_config.json"])
        (out_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return summary


def generate(reqs, model, out_dir, warmup=0, prewarm_ratio=0, rounding="ceil", progress_every=1000,
             show_progress=True, progress_interval=0.5, csv_writer_process=False, dag_detail="traffic",
             decode_compute_detail="none"):
    destination = Path(out_dir).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".v15-build-", dir=destination) as temporary:
        with Progress("DAG build", len(reqs), enabled=show_progress and progress_every != 0,
                      interval=progress_interval) as progress:
            summary = _generate(reqs, model, temporary, warmup, prewarm_ratio, rounding, progress,
                                csv_writer_process=csv_writer_process, dag_detail=dag_detail,
                                decode_compute_detail=decode_compute_detail)
        effective = copy.deepcopy(model.config)
        effective.pop("v14_effective", None)
        effective.setdefault("kv_delivery", {}).setdefault("layer_pipeline", {})["storage_to_d_streaming"] = False
        for phase, limit in (("prefill", model.p_max_num_seqs), ("decode", model.d_max_num_seqs)):
            settings = effective.setdefault("scheduler", {}).setdefault(phase, {})
            settings["max_batch_size"] = limit
            settings["max_batch_size_per_dp_engine"] = limit
            settings.pop("max_batch_size_per_dp_rank", None)
        effective["v15_effective"] = {k: summary[k] for k in (
            "prefill_tp", "decode_tp", "ep", "prefill_max_batch", "decode_max_batch",
            "prefill_max_batch_tokens", "compute_concurrency", "cache_policy", "decode_tp_assignment",
            "batch_limit_scope", "prefill_dp_degree", "decode_dp_degree",
            "prefill_domain_slot_upper_bound", "decode_domain_slot_upper_bound", "dp_compute_model",
            "prefill_compute_model", "decode_compute_model", "decode_compute_mode",
            "decode_first_step_us", "decode_per_token_us", "prefill_npu_peak_flops",
            "prefill_npu_peak_ops_per_sec", "prefill_npu_model", "prefill_npu_precision", "prefill_npu_peak_unit",
            "prefill_npu_peak_source", "prefill_npu_peak_is_public_default",
            "prefill_compute_time_interpretation", "prefill_efficiency_assumption",
            "prefill_compute_efficiency",
            "decode_compute_efficiency", "decode_shared_expert_tp_degree", "decode_context_convention",
            "decode_input_read_policy",
            "indexer_key_cache_resident")}
        (Path(temporary) / "effective_config.json").write_text(
            json.dumps(effective, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        for name in summary["outputs"]:
            os.replace(Path(temporary) / name, destination / name)
        if decode_compute_detail != "layer":
            (destination / "decode_layer_compute.csv").unlink(missing_ok=True)
    print(f"Wrote {destination / 'task_dag.csv'}: {summary['dag_rows']} tasks, "
          f"{summary['prefill_batches']} P batches, {summary['decode_batches']} D steps")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", action="version", version=VERSION)
    parser.add_argument("trace", help="Mooncake JSONL; timestamp/timestamp_ms are milliseconds")
    parser.add_argument("--config", default=str(Path(__file__).with_name("mooncake_pd_config_v14.json")))
    parser.add_argument("--out-dir", default="mooncake_pd_v15")
    parser.add_argument("--warmup-requests", type=int, default=0,
                        help="warmup defaults to 0 so tiny examples run; use 400 explicitly for v13 convention")
    parser.add_argument("--max-requests", type=int)
    parser.add_argument("--per-request-prefix-prewarm-ratio", type=float, default=0)
    parser.add_argument("--per-request-prefix-prewarm-rounding", choices=("ceil", "floor"), default="ceil")
    parser.add_argument("--prefill-scheduler", choices=("round-robin", "random", "least-load", "ttft-aware"))
    parser.add_argument("--decode-scheduler", choices=("round-robin", "random", "least-load", "first-token-aware"))
    parser.add_argument("--random-seed", type=int)
    parser.add_argument("--prefill-ms-per-token", type=float)
    parser.add_argument("--decode-ms-per-token", type=float,
                        help="override both fixed Decode step durations in milliseconds; "
                             "unavailable with compute_model.decode_mode=glm51_flops")
    parser.add_argument("--decode-compute-detail", choices=("none", "layer"), default="none",
                        help="always write one decode_compute.csv row per batch; layer additionally writes per-layer FLOPs detail")
    parser.add_argument("--prefill-max-batch", type=int, help="Local BS per Prefill DP engine")
    parser.add_argument("--decode-max-batch", type=int, help="Local resident BS per Decode DP engine")
    parser.add_argument("--progress-every", type=int, default=1000, help="0 disables progress; updates per scheduling event")
    parser.add_argument("--no-progress", action="store_true")
    parser.add_argument("--progress-interval", type=float, default=0.5)
    parser.add_argument("--csv-writer-process", choices=("auto", "on", "off"), default="auto",
                        help="auto (default): encode task CSV in a separate process if CPU affinity allows multiple CPUs; off: same-process output")
    parser.add_argument("--dag-detail", choices=("full", "traffic"), default="traffic",
                        help="traffic (default): compact logical TP DAG; full: additional diagnostic columns; both need the logical TP converter")
    args = parser.parse_args()
    try:
        cfg = json.loads(Path(args.config).read_text(encoding="utf-8-sig"))
        for key in ("prefill_scheduler", "decode_scheduler", "random_seed"):
            if getattr(args, key) is not None:
                cfg.setdefault("scheduling", {})[key] = getattr(args, key)
        for phase, value in (("prefill", args.prefill_max_batch), ("decode", args.decode_max_batch)):
            if value is not None:
                settings = cfg.setdefault("scheduler", {}).setdefault(phase, {})
                settings["max_batch_size_per_dp_engine"] = value
                settings.pop("max_batch_size_per_dp_rank", None)
        if args.prefill_ms_per_token is not None:
            cfg["timing"]["prefill_us_per_token"] = args.prefill_ms_per_token * 1000
        if args.decode_ms_per_token is not None:
            if str(cfg.get("compute_model", {}).get("decode_mode", "legacy")).lower() == "glm51_flops":
                raise ValueError("--decode-ms-per-token conflicts with compute_model.decode_mode=glm51_flops")
            cfg["timing"]["decode_us_per_token"] = args.decode_ms_per_token * 1000
            cfg["timing"]["first_decode_step_us"] = args.decode_ms_per_token * 1000
        if args.max_requests is not None and args.max_requests <= 0:
            raise ValueError("max requests must be positive")
        if args.progress_every < 0:
            raise ValueError("progress-every must be nonnegative")
        if not math.isfinite(args.progress_interval) or args.progress_interval <= 0:
            raise ValueError("progress-interval must be finite and positive")
        model = Model(cfg)
        show = not args.no_progress and args.progress_every != 0
        reqs = load_trace(args.trace, args.max_requests, show, args.progress_interval)
        summary = generate(reqs, model, args.out_dir, args.warmup_requests,
            args.per_request_prefix_prewarm_ratio, args.per_request_prefix_prewarm_rounding,
            args.progress_every, show, args.progress_interval,
            csv_writer_process=(args.csv_writer_process == "on" or
                args.csv_writer_process == "auto" and multiple_cpus_available()),
            dag_detail=args.dag_detail, decode_compute_detail=args.decode_compute_detail)
    except (ValueError, KeyError, TypeError, OSError, csv.Error, ArithmeticError, RuntimeError) as exc:
        print(f"Generation failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--_csv-worker":
        csv_worker(sys.argv[2])
    else:
        sys.exit(main())
