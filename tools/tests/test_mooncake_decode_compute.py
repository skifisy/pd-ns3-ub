"""Fixed Decode timing, optional FLOPs, and folded traffic regressions."""

import copy
import csv
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "generate_trace"
SPEC = importlib.util.spec_from_file_location("mooncake_model_v7", TOOLS / "mooncake_trace_to_dag_v15.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
CONFIG = json.loads((TOOLS / "mooncake_pd_config_v15_glm51.json").read_text())
CONFIG["compute_model"]["decode_mode"] = "glm51_flops"


def state(request=0, context=1024, token=1, group=0):
    return {"req": {"request_id": request, "input_length": context}, "token": token,
            "d_tp_group": group, "d_tp_group_id": f"DG0:TP{group}"}


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


class DecodeArithmeticTest(unittest.TestCase):
    def model(self, cfg=None):
        return MODULE.Model(copy.deepcopy(CONFIG if cfg is None else cfg))

    def test_query_and_context_counts_at_topk_boundary(self):
        model = self.model()
        states = [state(i, context, token=0) for i, context in enumerate((1, 2047, 2048))]
        summary, durations, layers = model.decode_compute(states, include_layers=True)
        self.assertEqual(summary["new_tokens"], 3)
        self.assertEqual(summary["cached_context_tokens"], 4096)
        self.assertEqual(summary["visible_context_tokens"], 4099)
        self.assertEqual(summary["indexer_pairs_per_indexed_layer"], 4099)
        self.assertEqual(summary["sparse_pairs_per_layer"], 4098)
        self.assertEqual(len(layers), 78)
        self.assertEqual(sum(durations), summary["compute_us"])
        # History changes scores/attention, never the new-token projections or FFN.
        longer = [state(i, context + 5000, token=0) for i, context in enumerate((1, 2047, 2048))]
        other = model.decode_compute(longer)[0]
        for field in ("mla_projection_flops", "indexer_projection_flops", "dense_ffn_flops",
                      "router_flops", "routed_expert_flops", "shared_expert_flops"):
            self.assertEqual(summary[field], other[field])
        self.assertGreater(other["indexer_score_flops"], summary["indexer_score_flops"])
        self.assertEqual(other["sparse_pairs_per_layer"], 3 * 2048)

    def test_completed_steps_extend_context_without_extra_queries(self):
        model = self.model()
        first = model.decode_compute([state(context=1024, token=0)])[0]
        later = model.decode_compute([state(context=1024, token=10)])[0]
        self.assertEqual(later["cached_context_tokens"] - first["cached_context_tokens"], 10)
        self.assertEqual(later["indexer_pairs_per_indexed_layer"] - first["indexer_pairs_per_indexed_layer"], 10)
        self.assertEqual(first["new_tokens"], later["new_tokens"])
        self.assertEqual(first["mla_projection_flops"], later["mla_projection_flops"])

    def test_same_group_batch_scales_but_different_groups_run_in_parallel(self):
        model = self.model()
        one = model.decode_compute([state()])[0]
        together = model.decode_compute([state(), state(1)])[0]
        apart = model.decode_compute([state(), state(1, group=1)])[0]
        self.assertAlmostEqual(together["compute_time_us"], 2 * one["compute_time_us"])
        self.assertAlmostEqual(together["attention_time_us"], 2 * apart["attention_time_us"])
        self.assertEqual(together["routed_expert_flops"], apart["routed_expert_flops"])
        self.assertEqual(together["routed_expert_time_us"], apart["routed_expert_time_us"])
        self.assertLess(apart["compute_time_us"], together["compute_time_us"])

    def test_decode_tp_efficiency_and_peak_scaling(self):
        states = [state()]
        base = self.model().decode_compute(states)[0]
        cfg = copy.deepcopy(CONFIG)
        cfg["parallel"]["decode_tp"] = 4
        wider = self.model(cfg).decode_compute(states)[0]
        self.assertAlmostEqual(wider["attention_time_us"] * 2, base["attention_time_us"])
        self.assertEqual(wider["routed_expert_time_us"], base["routed_expert_time_us"])
        cfg = copy.deepcopy(CONFIG)
        cfg["compute_model"]["decode_efficiency"] = 0.5
        slower_model = self.model(cfg)
        slower = slower_model.decode_compute(states)[0]
        self.assertAlmostEqual(slower["compute_time_us"], 2 * base["compute_time_us"])
        self.assertEqual(slower_model.prefill_efficiency, self.model().prefill_efficiency)
        cfg = copy.deepcopy(CONFIG)
        cfg["compute_model"]["npu_peak_ops_per_sec"] *= 2
        faster = self.model(cfg).decode_compute(states)[0]
        self.assertAlmostEqual(2 * faster["compute_time_us"], base["compute_time_us"])

    def test_full_model_layer_count_and_routed_experts(self):
        cfg = copy.deepcopy(CONFIG)
        cfg["compute_model"]["architecture"]["first_k_dense_replace"] = 78
        dense = self.model(cfg).decode_compute([state()])[0]
        self.assertEqual(dense["routed_expert_flops"], 0)
        cfg["model"]["num_layers"] = 39
        cfg["compute_model"]["architecture"]["num_hidden_layers"] = 39
        cfg["compute_model"]["architecture"]["first_k_dense_replace"] = 39
        cfg["compute_model"]["indexer_layers"] = list(range(39))
        half = self.model(cfg).decode_compute([state()])[0]
        self.assertAlmostEqual(dense["compute_time_us"], 2 * half["compute_time_us"])
        self.assertEqual(dense["mla_projection_flops"], 2 * half["mla_projection_flops"])
        model = self.model()
        summary, _, layers = model.decode_compute([state()], include_layers=True)
        self.assertEqual([row["layer_type"] for row in layers[:3]], ["dense"] * 3)
        self.assertTrue(all(row["routed_expert_flops"] == 0 for row in layers[:3]))
        self.assertTrue(all(row["routed_expert_flops"] > 0 for row in layers[3:]))
        self.assertGreater(summary["routed_expert_flops"], 0)

    def test_merged_duration_matches_cumulative_layer_rounding(self):
        model = self.model()
        states = [state(0, 4096, group=0), state(1, 8192, group=1), state(2, 1234, token=9, group=0)]
        merged, _, no_rows = model.decode_compute(states)
        layered, durations, rows = model.decode_compute(states, include_layers=True)
        self.assertEqual(merged, layered)
        self.assertFalse(no_rows)
        self.assertEqual(sum(durations), merged["compute_us"])
        self.assertEqual(sum(row["layer_compute_us"] for row in rows), merged["compute_us"])
        self.assertEqual(math.fsum(row["layer_compute_time_us"] for row in rows), merged["compute_time_us"])
        self.assertTrue(all(value >= 0 for value in durations))
        # One MAC is counted as two operations by the shared Prefill coefficients.
        cfg = copy.deepcopy(CONFIG)
        cfg["timing"]["decode_us_per_token"] = 999999
        cfg["timing"]["first_decode_step_us"] = 888888
        self.assertEqual(self.model(cfg).decode_compute(states)[0], merged)

    def test_legacy_and_configuration_validation(self):
        cfg = copy.deepcopy(CONFIG)
        cfg["compute_model"]["mode"] = "legacy"
        cfg["compute_model"]["decode_mode"] = "legacy"
        result, durations, _ = self.model(cfg).decode_compute([state()], include_layers=True)
        self.assertEqual(result["compute_us"], 10000)
        self.assertEqual(sum(durations), 10000)
        self.assertNotIn("attention_flops", result)
        for field, value in (("decode_efficiency", 0), ("decode_shared_expert_tp_degree", 3)):
            cfg = copy.deepcopy(CONFIG)
            cfg["compute_model"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.model(cfg)


class FixedDecodeTimingTest(unittest.TestCase):
    def config(self):
        # Use the shipped policy, rather than CONFIG's explicit FLOPs override.
        return json.loads((TOOLS / "mooncake_pd_config_v15_glm51.json").read_text())

    def test_fixed_decode_does_not_disable_prefill_flops(self):
        cfg = self.config()
        model = MODULE.Model(cfg)
        self.assertEqual(model.prefill_compute_mode, "glm51_flops")
        self.assertEqual(model.decode_compute_mode, "legacy")
        for members in ([state(token=0)], [state(token=9)],
                        [state(i, context=16384, token=i % 2, group=i % 3) for i in range(64)]):
            summary, durations, _ = model.decode_compute(members, include_layers=True)
            self.assertEqual(summary["compute_us"], 10000)
            self.assertEqual(sum(durations), 10000)
        cfg["compute_model"].pop("decode_mode")
        self.assertEqual(MODULE.Model(cfg).decode_compute([state()])[0]["compute_us"], 10000)

    def test_distinct_first_and_resident_timings_take_max_not_sum(self):
        cfg = self.config()
        cfg["timing"].update(first_decode_step_us=15000, decode_us_per_token=7000)
        model = MODULE.Model(cfg)
        for members, expected in (([state(token=0)], 15000), ([state(token=5)], 7000),
                                  ([state(token=0), state(1, token=5)], 15000)):
            self.assertEqual(model.decode_compute(members)[0]["compute_us"], expected)
        cfg["compute_model"]["decode_mode"] = "invalid"
        with self.assertRaisesRegex(ValueError, "decode_mode"):
            MODULE.Model(cfg)

    def test_fixed_cli_override_works_with_prefill_flops(self):
        with tempfile.TemporaryDirectory(prefix="decode-fixed-cli-") as folder:
            root = Path(folder)
            trace = root / "trace.jsonl"
            trace.write_text(json.dumps(dict(request_id=0, timestamp=0, input_length=1,
                                             output_length=2, hash_ids=["a"])) + "\n")
            args = [TOOLS / "mooncake_trace_to_dag_v15.py", trace, "--config",
                    TOOLS / "mooncake_pd_config_v15_glm51.json", "--decode-ms-per-token", "7",
                    "--out-dir", root, "--no-progress"]
            DecodeTraceTest.run_command(args)
            rows = read_csv(root / "decode_compute.csv")
            self.assertEqual([row["compute_us"] for row in rows], ["7000", "7000"])
            summary = json.loads((root / "summary.json").read_text())
            self.assertEqual(summary["decode_first_step_us"], 7000)
            self.assertEqual(summary["decode_compute_mode"], "legacy")
            self.assertIn("glm51", summary["prefill_compute_model"])


class DecodeTraceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="decode-compute-test-")
        cls.root = Path(cls.temp.name)
        cfg = copy.deepcopy(CONFIG)
        # Keep the prior Decode step busy long enough for another Prefill
        # member to become ready, without simulating packets or wall-clock waits.
        cfg["compute_model"]["decode_efficiency"] = 1e-6
        cls.config = cls.root / "config.json"
        cls.config.write_text(json.dumps(cfg))
        cls.trace = cls.root / "trace.jsonl"
        requests = [dict(request_id=0, timestamp=0, input_length=512, output_length=1, hash_ids=["cached"]),
                    dict(request_id=1, timestamp=1, input_length=1536, output_length=3, hash_ids=["cached", "a", "a2"]),
                    dict(request_id=2, timestamp=1, input_length=2048, output_length=3, hash_ids=["cached", "b", "b2", "b3"])]
        cls.trace.write_text("".join(json.dumps(row) + "\n" for row in requests))
        cls.run_command([TOOLS / "mooncake_trace_to_dag_v15.py", cls.trace, "--config", cls.config,
                         "--warmup-requests", "1", "--out-dir", cls.root, "--no-progress",
                         "--decode-compute-detail", "layer"])
        cls.run_command([TOOLS / "mooncake_dag_to_traffic_v15.py", cls.root / "task_dag.csv",
                         "--out-dir", cls.root, "--mapping-output", cls.root / "mapping.csv",
                         "--summary-output", cls.root / "traffic_summary.json", "--no-progress"])

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @staticmethod
    def run_command(args, expected=0):
        result = subprocess.run([sys.executable, *map(str, args)], text=True, capture_output=True, timeout=30)
        if result.returncode != expected:
            raise AssertionError(f"exit {result.returncode}: {result.stdout}\n{result.stderr}")
        return result

    def test_mixed_then_resident_steps_and_compute_detail(self):
        batches = read_csv(self.root / "decode_compute.csv")
        self.assertEqual([row["compute_granularity"] for row in batches], ["step", "step", "step", "step"])
        self.assertEqual([row["new_request_count"] for row in batches], ["1", "1", "0", "0"])
        self.assertEqual([row["request_count"] for row in batches], ["1", "2", "2", "1"])
        layers = read_csv(self.root / "decode_layer_compute.csv")
        self.assertEqual(len(layers), 4 * 78)
        mapping = read_csv(self.root / "mapping.csv")
        for batch in batches:
            compute = [row for row in mapping if row["batch_id"] == batch["batch_id"]
                       and row["task_type"] in {"DECODE_LAYER", "DECODE_STEP_COMPUTE"}]
            self.assertFalse(compute)  # Compute survives in DAG/detail, not traffic.
            dag_compute = [row for row in read_csv(self.root / "task_dag.csv")
                           if row["batch_id"] == batch["batch_id"] and row["task_type"] == "DECODE_STEP_COMPUTE"]
            self.assertEqual(len(dag_compute), 1)
            self.assertEqual(int(dag_compute[0]["duration_us"]), int(batch["compute_us"]))
            detail = [row for row in layers if row["batch_id"] == batch["batch_id"]]
            self.assertEqual(sum(int(row["layer_compute_us"]) for row in detail), int(batch["compute_us"]))
        self.assertTrue(all(not row["request_id"] for row in mapping if row["task_type"] == "DECODE_STEP_COMPUTE"))
        self.assertFalse(any(row["task_type"] == "STORAGE_TO_D" and row["batch_id"] == batches[-1]["batch_id"]
                             for row in mapping))
        summary = json.loads((self.root / "traffic_summary.json").read_text())
        self.assertEqual(summary["decode_step_compute_phases"], 0)
        self.assertEqual(summary["folded_decode_compute_tasks"], 4)
        self.assertEqual(summary.get("decode_layer_compute_phases", 0), 0)
        self.assertEqual(summary["decode_compute_phases"], 0)
        dag_summary = json.loads((self.root / "summary.json").read_text())
        self.assertEqual(dag_summary["decode_compute_model"], "glm51-mla-dsa-moe-decode-flops-v1")
        self.assertEqual(dag_summary["decode_merged_steps"], 4)
        self.assertIn("decode_compute.csv", dag_summary["outputs"])

    def test_fixed_duration_cli_conflicts_with_flops(self):
        result = self.run_command([TOOLS / "mooncake_trace_to_dag_v15.py", self.trace,
                                   "--config", self.config, "--decode-ms-per-token", "10"], expected=1)
        self.assertIn("conflicts", result.stderr)

    def test_each_request_writes_only_at_its_own_last_token(self):
        dag = read_csv(self.root / "task_dag.csv")
        batches = read_csv(self.root / "batches_debug.csv")
        computes = {row["batch_id"]: row for row in batches if row["batch_type"] == "DECODE"}
        writes = [row for row in dag if row["task_type"] == "DECODE_WRITE"]
        self.assertEqual(len(writes), 2)  # Two requests, not six generated tokens.
        self.assertFalse(any(row["task_type"] == "D_WRITES_DONE" for row in dag))
        model = MODULE.Model(copy.deepcopy(CONFIG))
        mapping = read_csv(self.root / "mapping.csv")
        for write in writes:
            request = write["request_id"]
            self.assertEqual(write["decode_token_index"], "2")
            self.assertEqual(write["block_index"], "-1")
            self.assertEqual(int(write["bytes"]), 3 * model.kv_bytes_per_token)
            last = computes[write["batch_id"]]
            self.assertEqual(json.loads(last["decode_token_indices"])[request], 2)
            self.assertEqual(write["depends_on"], last["compute_done_task_id"])
            done = next(row for row in dag if row["task_type"] == "D_DONE" and row["request_id"] == request)
            self.assertEqual(done["depends_on"], write["task_id"])
            physical = [row for row in mapping if row["task_type"] == "DECODE_WRITE" and row["request_id"] == request]
            self.assertEqual(len(physical), 2)
            self.assertTrue(all(row["dag_task_id"] == write["task_id"] for row in physical))
        # One request finishes while the other still has a token to compute.
        first_write = min(writes, key=lambda row: int(row["batch_id"]))
        remaining = [row for row in computes.values() if int(row["batch_id"]) > int(first_write["batch_id"])]
        self.assertEqual(len(remaining), 1)
        self.assertEqual(json.loads(remaining[0]["request_ids"]), [3 - int(first_write["request_id"])])
        summary = json.loads((self.root / "summary.json").read_text())
        self.assertEqual(summary["task_counts"]["DECODE_WRITE"], 2)
        self.assertIn("once per request", summary["decode_output_write_policy"])

    def test_default_detail_is_one_row_per_batch(self):
        output = self.root / "no-layer-detail"
        self.run_command([TOOLS / "mooncake_trace_to_dag_v15.py", self.trace, "--config", self.config,
                          "--warmup-requests", "1", "--out-dir", output, "--no-progress"])
        self.assertFalse((output / "decode_layer_compute.csv").exists())
        self.assertEqual(len(read_csv(output / "decode_compute.csv")), 4)
        self.assertEqual(read_csv(self.root / "decode_compute.csv"), read_csv(output / "decode_compute.csv"))


class DecodeFinalWriteBoundaryTest(unittest.TestCase):
    def run_case(self, root, tokens, kv_bytes, divisor=1):
        cfg = copy.deepcopy(CONFIG)
        cfg["compute_model"]["mode"] = "legacy"
        cfg["compute_model"]["decode_mode"] = "legacy"
        cfg["model"].update(num_layers=3, kv_bytes_per_token=kv_bytes)
        config = root / "config.json"
        config.write_text(json.dumps(cfg))
        trace = root / "trace.jsonl"
        trace.write_text(json.dumps(dict(request_id=9, timestamp=0, input_length=1,
                                        output_length=tokens, hash_ids=["input"])) + "\n")
        DecodeTraceTest.run_command([TOOLS / "mooncake_trace_to_dag_v15.py", trace,
                                    "--config", config, "--out-dir", root, "--dag-detail", "full", "--no-progress"])
        DecodeTraceTest.run_command([TOOLS / "mooncake_dag_to_traffic_v15.py", root / "task_dag.csv",
                                    "--out-dir", root, "--mapping-output", root / "mapping.csv",
                                    "--data-size-divisor", divisor, "--no-progress"])
        return MODULE.Model(cfg)

    def check_payload(self, root, model, tokens, divisor):
        dag = read_csv(root / "task_dag.csv")
        writes = [row for row in dag if row["task_type"] == "DECODE_WRITE"]
        self.assertEqual(len(writes), 1)
        write = writes[0]
        self.assertEqual(int(write["bytes"]), tokens * model.kv_bytes_per_token)
        self.assertEqual(write["storage_key"], "decode:9:output")
        self.assertEqual(write["kv_extent_kind"], "all_output_tokens_all_layers")
        self.assertEqual(write["decode_token_index"], str(tokens - 1))
        self.assertEqual(write["block_index"], "-1")
        self.assertFalse(any(row["task_type"] == "D_WRITES_DONE" for row in dag))
        self.assertEqual(sum(row["task_type"] == "DECODE_STEP_COMPUTE" for row in dag), tokens)
        done = next(row for row in dag if row["task_type"] == "D_DONE")
        self.assertEqual(done["depends_on"], write["task_id"])
        self.assertEqual(done["forecast_finish_us"], write["forecast_finish_us"])
        mapping = [row for row in read_csv(root / "mapping.csv") if row["task_type"] == "DECODE_WRITE"]
        traffic = {row["taskId"]: row for row in read_csv(root / "traffic.csv")}
        debug = read_csv(root / "traffic_debug.csv")
        self.assertTrue(all(row["request_id"] == "9" for row in debug if row["opType"] == "URMA_WRITE"
                            and row["taskId"] in {m["taskId"] for m in mapping}))
        sizes = []
        members = json.loads(write["tp_members"])
        self.assertEqual(len({tuple(member[3:5]) for member in members}), 1)
        for rank in range(model.d_tp_degree):
            expected = tokens * sum(MODULE.split_exact(model.layer_bytes(model.kv_bytes_per_token, layer),
                                                       model.d_tp_degree, rank)[1]
                                    for layer in range(model.num_layers))
            member = next(member for member in members if member[0] == rank)
            self.assertEqual(member[-1], expected)
            chunks = []
            remaining = expected
            while remaining:
                chunk = min(remaining, (1 << 32) - 1)
                chunks.append((chunk + divisor - 1) // divisor)
                remaining -= chunk
            rows = [row for row in mapping if int(row["tp_rank"]) == rank]
            self.assertEqual([int(traffic[row["taskId"]]["dataSize(Byte)"]) for row in rows], chunks)
            self.assertTrue(all(row["delay"] == f"{tokens * 10000}us" for row in rows))
            read_phases = {row["phaseId"] for row in read_csv(root / "mapping.csv")
                           if row["task_type"] == "STORAGE_TO_D"}
            self.assertTrue(all(set(row["dependOnPhases"].split()) == read_phases for row in rows))
            sizes.extend(chunks)
        self.assertEqual(len(mapping), len(sizes))

    def test_long_output_crosses_block_boundary_without_write_history(self):
        with tempfile.TemporaryDirectory(prefix="decode-final-long-") as folder:
            root = Path(folder)
            model = self.run_case(root, tokens=513, kv_bytes=157)
            self.check_payload(root, model, 513, 1)
            self.assertEqual(len([row for row in read_csv(root / "mapping.csv")
                                  if row["task_type"] == "DECODE_WRITE"]), 2)

    def test_large_rank_payload_is_split_then_scaled(self):
        with tempfile.TemporaryDirectory(prefix="decode-final-u32-") as folder:
            root = Path(folder)
            model = self.run_case(root, tokens=3, kv_bytes=(1 << 32) + 13, divisor=100000)
            self.check_payload(root, model, 3, 100000)
            self.assertEqual(len([row for row in read_csv(root / "mapping.csv")
                                  if row["task_type"] == "DECODE_WRITE"]), 4)


if __name__ == "__main__":
    unittest.main()
