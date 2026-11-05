"""Small V15 conversion regressions; no ns-3 build or simulation required."""

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from collections import defaultdict


TOOLS = Path(__file__).resolve().parents[1]
GENERATOR = TOOLS / "mooncake_trace_to_dag_v15.py"
CONVERTER = TOOLS / "mooncake_dag_to_traffic_v15.py"
CONFIG = TOOLS / "mooncake_pd_config_v15_glm51.json"


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def phases(row):
    return set(row["dependOnPhases"].split())


def write_csv(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


class PrefillTrafficTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="prefill-traffic-test-")
        cls.root = Path(cls.temp.name)
        cls.trace = cls.root / "trace.jsonl"
        # Two formal requests share a cached prefix and each produce several
        # new blocks. They enter the same batch on distinct TP groups. Include a
        # zero-output request to exercise the batch-less D_DONE gate as well.
        requests = [
            dict(request_id=0, timestamp=0, input_length=512,
                 output_length=1, hash_ids=["cached"]),
            dict(request_id=1, timestamp=1, input_length=1536,
                 output_length=1, hash_ids=["cached", "new-a", "new-a2"]),
            dict(request_id=2, timestamp=1, input_length=2048,
                 output_length=0, hash_ids=["cached", "new-b", "new-b2", "new-b3"]),
        ]
        cls.trace.write_text("".join(json.dumps(row) + "\n" for row in requests))
        cls.outputs = {}
        for detail, process in (("traffic", "off"), ("full", "off"), ("traffic", "on")):
            name = f"{detail}-{process}"
            output = cls.root / name
            cls.run_command([
                GENERATOR, cls.trace, "--config", CONFIG, "--warmup-requests", "1",
                "--dag-detail", detail, "--csv-writer-process", process,
                "--out-dir", output, "--no-progress",
            ])
            cls.convert(output / "task_dag.csv", output)
            cls.outputs[name] = output

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @staticmethod
    def run_command(args, expected=0):
        result = subprocess.run([sys.executable, *map(str, args)],
                                text=True, capture_output=True, timeout=30)
        if result.returncode != expected:
            raise AssertionError(f"exit {result.returncode}: {result.stdout}\n{result.stderr}")
        return result

    @classmethod
    def convert(cls, source, output, expected=0, extra=(), storage_latency="150"):
        latency_args = (["--storage-read-latency-us", storage_latency]
                        if storage_latency is not None else [])
        return cls.run_command([
            CONVERTER, source, "--out-dir", output,
            "--mapping-output", output / "mapping.csv",
            "--summary-output", output / "summary.json",
            "--timing-policy", "max-phase", "--phase-grouping", "request-wave",
            "--data-size-divisor", "100000", *latency_args,
            "--no-progress", *extra,
        ], expected)

    def test_layer_barriers_and_compute_time(self):
        output = self.outputs["traffic-off"]
        dag = read_csv(output / "task_dag.csv")
        traffic = read_csv(output / "traffic.csv")
        mapping = read_csv(output / "mapping.csv")
        by_id = {row["taskId"]: row for row in traffic}
        compute_duration = {row["task_id"]: int(row["duration_us"])
                            for row in dag if row["task_type"] == "PREFILL_LAYER"}
        self.assertEqual(traffic[0]["opType"], "TIMER")
        self.assertEqual(dag[0]["batch_type"], "PREFILL")
        self.assertEqual(dag[0]["p_node"], "0")
        reads, computes, writes = defaultdict(list), {}, defaultdict(list)
        for row in mapping:
            key = (row["batch_id"], int(row["layer_index"] or -1))
            if row["task_type"] == "PREFIX_READ":
                reads[key].append(row)
            elif row["task_type"] == "PREFILL_LAYER":
                self.assertNotIn(key, computes)
                computes[key] = row
            elif row["task_type"] == "PREFIX_STORE_WRITE":
                writes[key].append(row)

        self.assertEqual(len(computes), 78)
        for key, members in reads.items():
            batch, layer = key
            read_phases = {row["phaseId"] for row in members}
            self.assertEqual(len(read_phases), 1)
            self.assertEqual(len({row["request_id"] for row in members}), 2)
            self.assertEqual({row["delay"] for row in members}, {"150us"})
            compute = computes[key]
            record = by_id[compute["taskId"]]
            self.assertEqual(record["opType"], "TIMER")
            self.assertEqual(record["dataSize(Byte)"], "0")
            self.assertEqual(record["delay"], f"{compute_duration[compute['dag_task_id']]}us")
            self.assertTrue(read_phases <= phases(compute))
            if layer:
                previous = computes[(batch, layer - 1)]
                self.assertIn(previous["phaseId"], phases(compute))
                # Prefetch starts with the preceding layer's computation,
                # rather than waiting for that computation to finish.
                for read in members:
                    self.assertEqual(phases(read), phases(previous))
                    self.assertNotIn(previous["phaseId"], phases(read))
            for write in writes[key]:
                self.assertEqual(phases(write), {compute["phaseId"]})
                self.assertEqual(write["delay"], "0us")
            self.assertEqual(len({row["request_id"] for row in writes[key]}), 2)
            self.assertEqual(len({row["block_index"] for row in writes[key]}), 3)
            self.assertEqual(len({row["phaseId"] for row in writes[key]}), 1)
            self.assertTrue(read_phases.isdisjoint(
                {row["phaseId"] for row in writes[key]}))

        summary = json.loads((output / "summary.json").read_text())
        self.assertEqual(summary["prefill_batches"], 1)
        self.assertEqual(summary["prefill_control_phases"] + summary["prefill_compute_phases"], 79)
        self.assertEqual(summary["decode_compute_phases"], 1)
        self.assertEqual(summary["timer_rows"], 80)
        self.assertEqual(summary["total_rows"], len(traffic))
        self.assertIn("traffic-debug", summary["converter_version"])
        self.assertIn("Write=Batch+Layer", summary["prefill_phase_grouping_policy"])

    def test_output_modes_and_writer_process_agree(self):
        expected = (self.outputs["traffic-off"] / "traffic.csv").read_bytes()
        expected_debug = (self.outputs["traffic-off"] / "traffic_debug.csv").read_bytes()
        for name in ("full-off", "traffic-on"):
            self.assertEqual(expected, (self.outputs[name] / "traffic.csv").read_bytes())
            self.assertEqual(expected_debug, (self.outputs[name] / "traffic_debug.csv").read_bytes())

    def test_debug_rows_match_native_and_request_mapping(self):
        for name, output in self.outputs.items():
            with self.subTest(mode=name):
                traffic = read_csv(output / "traffic.csv")
                debug = read_csv(output / "traffic_debug.csv")
                mapping = read_csv(output / "mapping.csv")
                self.assertEqual(list(debug[0]), list(traffic[0]) + ["request_id"])
                self.assertEqual(len(debug), len(traffic))
                self.assertEqual(len(mapping), len(traffic))
                for native, detail, origin in zip(traffic, debug, mapping):
                    self.assertEqual({k: detail[k] for k in native}, native)
                    self.assertEqual(detail["request_id"], origin["request_id"])
                    if native["opType"] == "TIMER":
                        self.assertEqual(detail["request_id"], "")
                    else:
                        self.assertIn(detail["request_id"], {"1", "2"})

    def test_decode_reads_use_storage_latency_after_final_prefill_write(self):
        output = self.outputs["traffic-off"]
        mapping = read_csv(output / "mapping.csv")
        writes = [row for row in mapping if row["task_type"] == "PREFIX_STORE_WRITE"]
        last_layer = max(int(row["layer_index"]) for row in writes)
        final_writes = {row["phaseId"] for row in writes if int(row["layer_index"]) == last_layer}
        reads = [row for row in mapping if row["task_type"] == "STORAGE_TO_D"]
        self.assertEqual(len({row["layer_index"] for row in reads}), 78)
        self.assertEqual({row["delay"] for row in reads}, {"150us"})
        for row in reads:
            if row["layer_index"] == "0":
                self.assertEqual(phases(row), final_writes)
        summary = json.loads((output / "summary.json").read_text())
        self.assertEqual(summary["decode_batch_forecast_releases_ignored"], 1)
        self.assertEqual(summary["decode_storage_only_read_tasks"], len(reads))
        self.assertTrue(all(row["task_type"] != "STORAGE_TO_D"
                            for row in summary["conservative_examples"]))

    def test_generated_decode_first_reads_bypass_batch_compute_gate(self):
        dag = read_csv(self.outputs["traffic-off"] / "task_dag.csv")
        by_id = {row["task_id"]: row for row in dag}
        reads = [row for row in dag if row["task_type"] == "STORAGE_TO_D"]
        self.assertTrue(reads)
        for row in reads:
            deps = row["depends_on"].split(";")
            self.assertEqual(len(deps), 1)
            parent = by_id[deps[0]]
            self.assertEqual(parent["task_type"], "P_DONE")
            self.assertEqual(parent["request_id"], row["request_id"])
        for row in dag:
            if row["task_type"] == "DECODE_STEP_COMPUTE":
                self.assertNotEqual(row["d_node"], "")

    def test_large_decode_batch_keeps_read_dependencies_short(self):
        rows = []
        count = 241

        def add(batch, request="", **values):
            row = dict(task_id=len(rows), request_id=request, task_class="TIMER", task_type="P_BATCH_BEGIN",
                       depends_on="", release_time_us="", duration_us=0, src_node="", dst_node="", bytes=0,
                       execution_schema="v14-domain-serial-batch-tp-logical", network_wave="",
                       storage_read_latency_us=150, tp_members="", tp_degree=2, batch_id=batch,
                       batch_type="PREFILL" if batch <= count else "DECODE",
                       p_node=0 if batch <= count else "", d_node=0, layer_index=0, block_index=0)
            row.update(values)
            rows.append(row)
            return row["task_id"]

        done = []
        for request in range(count + 1):
            begin = add(request, release_time_us=1000 + request)
            compute = add(request, task_class="COMPUTE", task_type="PREFILL_LAYER", depends_on=str(begin), duration_us=1)
            write = add(request, request, task_class="NETWORK", task_type="PREFIX_STORE_WRITE", depends_on=str(compute),
                        bytes=2, network_wave=f"P:{request}", tp_members="[[0,0,0,1,0,1],[1,2,0,1,0,1]]")
            done.append(add(request, request, task_type="P_DONE", depends_on=str(write)))
        old = add(count + 1, task_class="COMPUTE", task_type="DECODE_STEP_COMPUTE", layer_index=-1,
                  depends_on=str(done[0]), duration_us=1000)
        begin = add(count + 2, task_type="D_BATCH_BEGIN", depends_on=" ".join(map(str, [old] + done[1:])))
        reads = []
        for layer in range(2):
            for request in range(1, count + 1):
                reads.append(add(count + 2, request, task_class="NETWORK", task_type="STORAGE_TO_D",
                                 layer_index=layer, depends_on=str(done[request]), bytes=2,
                                 network_wave=f"D:all:{layer}", tp_members="[[0,1,0,0,0,1],[1,1,0,2,0,1]]"))
        add(count + 2, task_class="COMPUTE", task_type="DECODE_STEP_COMPUTE", layer_index=-1,
            depends_on=" ".join(map(str, [begin] + reads)), duration_us=10)
        source = self.root / "large-read-all.csv"
        write_csv(source, rows)
        output = self.root / "large-read-all"
        self.convert(source, output)
        mapping = read_csv(output / "mapping.csv")
        read_rows = [row for row in mapping if row["task_type"] == "STORAGE_TO_D"]
        self.assertEqual(len(read_rows), count * 2 * 2)
        self.assertEqual(len({row["phaseId"] for row in read_rows}), 1)
        self.assertTrue(all(len(phases(row)) == 1 for row in read_rows))
        pref_writes = {row["request_id"]: row["phaseId"] for row in mapping if row["task_type"] == "PREFIX_STORE_WRITE"}
        self.assertTrue(all(phases(row) == {pref_writes[row["request_id"]]} for row in read_rows))
        computes = [row for row in mapping if row["task_type"] == "DECODE_STEP_COMPUTE"]
        self.assertEqual(phases(computes[-1]), {computes[0]["phaseId"], read_rows[0]["phaseId"]})

    def test_old_decode_batch_read_gate_is_rejected(self):
        rows = read_csv(self.outputs["traffic-off"] / "task_dag.csv")
        begins = {row["batch_id"]: row["task_id"] for row in rows if row["task_type"] == "D_BATCH_BEGIN"}
        for row in rows:
            if row["task_type"] == "STORAGE_TO_D":
                row["depends_on"] = begins[row["batch_id"]]
        source = self.root / "old-decode-read-gate.csv"
        write_csv(source, rows)
        output = self.root / "old-decode-read-gate"
        result = self.convert(source, output, expected=1)
        self.assertIn("regenerate task_dag.csv with the v8 generator", result.stderr)
        self.assertFalse((output / "traffic.csv").exists())

    def test_generated_decode_compute_waits_all_layers_once(self):
        output = self.outputs["traffic-off"]
        mapping = read_csv(output / "mapping.csv")
        dag = {row["task_id"]: row for row in read_csv(output / "task_dag.csv")}
        reads = [row for row in mapping if row["task_type"] == "STORAGE_TO_D"]
        computes = [row for row in mapping if row["task_type"] == "DECODE_STEP_COMPUTE"]
        self.assertEqual(len(computes), 1)
        self.assertFalse(any(row["task_type"] == "DECODE_LAYER" for row in mapping))
        compute = computes[0]
        read_phases = {row["phaseId"] for row in reads}
        self.assertEqual(len(read_phases), 1)
        self.assertEqual(phases(compute), read_phases)
        self.assertEqual({int(row["layer_index"]) for row in reads}, set(range(78)))
        self.assertTrue(all(len(phases(row)) == 1 for row in reads))
        self.assertEqual(compute["request_id"], "")
        self.assertEqual(compute["delay"], f"{dag[compute['dag_task_id']]['duration_us']}us")
        cfg = json.loads(CONFIG.read_text())
        trace = {str(row["request_id"]): row for row in map(json.loads, self.trace.read_text().splitlines())}
        dag_reads = [row for row in dag.values() if row["task_type"] == "STORAGE_TO_D"]
        requests = {row["request_id"] for row in dag_reads}
        self.assertEqual(sum(int(row["bytes"]) for row in dag_reads),
                         sum(trace[request]["input_length"] for request in requests) * cfg["model"]["kv_bytes_per_token"])
        self.assertEqual(len(reads), sum(len(json.loads(row["tp_members"])) for row in dag_reads))
        for row in mapping:
            if row["task_type"] == "DECODE_WRITE":
                self.assertEqual(row["delay"], "0us")
                self.assertEqual(phases(row), {compute["phaseId"]})

    def test_resident_request_next_token_waits_for_compute_not_write(self):
        requests = [json.loads(line) for line in self.trace.read_text().splitlines()]
        requests[1]["output_length"] = 2
        source = self.root / "two-token-trace.jsonl"
        source.write_text("".join(json.dumps(row) + "\n" for row in requests))
        output = self.root / "two-token"
        self.run_command([GENERATOR, source, "--config", CONFIG, "--warmup-requests", "1",
                          "--dag-detail", "traffic", "--out-dir", output, "--no-progress"])
        self.convert(output / "task_dag.csv", output)
        mapping = read_csv(output / "mapping.csv")
        reads = [row for row in mapping if row["task_type"] == "STORAGE_TO_D"]
        self.assertTrue(reads)
        self.assertEqual({row["decode_token_index"] for row in reads}, {"0"})
        computes = [row for row in mapping if row["task_type"] in {"DECODE_LAYER", "DECODE_STEP_COMPUTE"}]
        batches = sorted({row["batch_id"] for row in computes}, key=int)
        self.assertEqual(len(batches), 2)
        first_last = next(row for row in computes if row["batch_id"] == batches[0])
        second_first = next(row for row in computes if row["batch_id"] == batches[1] and row["task_type"] == "DECODE_STEP_COMPUTE")
        self.assertEqual(len(computes), 2)
        self.assertEqual(second_first["layer_index"], "-1")
        self.assertEqual(second_first["request_id"], "")
        self.assertEqual(phases(second_first), {first_last["phaseId"]})
        write_phases = {row["phaseId"] for row in mapping if row["task_type"] == "DECODE_WRITE"}
        self.assertTrue(phases(second_first).isdisjoint(write_phases))
        detail = read_csv(output / "decode_compute.csv")
        self.assertEqual([row["compute_granularity"] for row in detail], ["step", "step"])
        self.assertEqual(second_first["delay"], detail[-1]["compute_us"] + "us")

    def test_decode_forecasts_do_not_change_traffic_but_prefill_arrivals_do(self):
        source = self.outputs["traffic-off"] / "task_dag.csv"
        rows = read_csv(source)
        changed = self.root / "changed-forecast.csv"
        for row in rows:
            if row["task_type"] == "D_BATCH_BEGIN":
                row["release_time_us"] = "1000000000000"
        with changed.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        output = self.root / "changed-forecast"
        self.convert(changed, output)
        self.assertEqual((self.outputs["traffic-off"] / "traffic.csv").read_bytes(),
                         (output / "traffic.csv").read_bytes())
        # External release timing still belongs to Prefill's batch-start gate.
        old_timer = next(row for row in read_csv(output / "traffic.csv") if row["opType"] == "TIMER")
        rows[0]["release_time_us"] = str(int(rows[0]["release_time_us"]) + 1000)
        with changed.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        self.convert(changed, output)
        new_timer = next(row for row in read_csv(output / "traffic.csv") if row["opType"] == "TIMER")
        self.assertNotEqual(old_timer["delay"], new_timer["delay"])

    def test_decode_storage_latency_override_zero_fractional_and_dag_default(self):
        rows = read_csv(self.outputs["traffic-off"] / "task_dag.csv")
        for row in rows:
            if row["task_type"] == "STORAGE_TO_D":
                row["storage_read_latency_us"] = "17.25"
        source = self.root / "decode-storage-latency.csv"
        with source.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        for latency, expected in (("0", "0us"), ("37.5", "37500ns"), (None, "17250ns")):
            with self.subTest(latency=latency):
                output = self.root / f"latency-{latency}"
                self.convert(source, output, storage_latency=latency)
                reads = [row for row in read_csv(output / "mapping.csv")
                         if row["task_type"] == "STORAGE_TO_D"]
                self.assertTrue(reads)
                self.assertEqual({row["delay"] for row in reads}, {expected})

    def test_decode_reads_overlap_previous_compute_but_whole_step_waits_all(self):
        rows = []

        def add(batch, **values):
            row = dict(task_id=len(rows), request_id="", task_class="TIMER",
                       task_type="P_BATCH_BEGIN", depends_on="", release_time_us="",
                       duration_us=0, src_node="", src_port="", dst_node="", dst_port="", bytes=0,
                       execution_schema="v14-domain-serial-batch-tp-logical",
                       network_wave="", storage_read_latency_us=150,
                       tp_members="", tp_degree=2, batch_id=batch,
                       batch_type="PREFILL" if batch < 3 else "DECODE",
                       p_node=0 if batch < 3 else "", d_node=0,
                       layer_index=0, block_index="")
            row.update(values)
            rows.append(row)
            return row["task_id"]

        def network(batch, request, stage, deps, layer, block=0):
            members = ([[0, 1, 0, 0, 0, 32], [1, 1, 0, 2, 0, 32]]
                       if stage == "STORAGE_TO_D" else
                       [[0, 0, 0, 1, 0, 32], [1, 2, 0, 1, 0, 32]])
            return add(batch, request_id=request, task_class="NETWORK", task_type=stage,
                       depends_on=" ".join(map(str, deps)), layer_index=layer, block_index=block,
                       bytes=64, network_wave=f"{batch}:{stage}:{layer}", tp_members=json.dumps(members))

        done = {}
        for request in range(3):
            begin = add(request, release_time_us=1000 + request * 10)
            compute = add(request, task_class="COMPUTE", task_type="PREFILL_LAYER",
                          depends_on=str(begin), duration_us=10)
            write = network(request, request, "PREFIX_STORE_WRITE", [compute], 0)
            done[request] = add(request, request_id=request, task_type="P_DONE", depends_on=str(write))
        begin = add(3, task_type="D_BATCH_BEGIN", depends_on=str(done[0]), release_time_us=900000)
        old_reads = [network(3, 0, "STORAGE_TO_D", [done[0]], layer) for layer in range(2)]
        old_compute = add(3, task_class="COMPUTE", task_type="DECODE_STEP_COMPUTE", layer_index=-1,
                          depends_on=" ".join(map(str, [begin] + old_reads)), duration_us=1000)
        network(3, 0, "DECODE_WRITE", [old_compute], -1)
        begin = add(4, task_type="D_BATCH_BEGIN", depends_on=f"{old_compute} {done[1]} {done[2]}",
                    release_time_us=999999999)
        new_reads = [network(4, request, "STORAGE_TO_D", [done[request]], layer, block)
                     for layer in range(2) for request in (1, 2) for block in (0, 1)]
        new_compute = add(4, task_class="COMPUTE", task_type="DECODE_STEP_COMPUTE", layer_index=-1,
                          depends_on=" ".join(map(str, [begin] + new_reads)), duration_us=300)
        for request in (0, 1, 2):
            network(4, request, "DECODE_WRITE", [new_compute], -1)

        def physical_rows():
            expanded, aliases = [], {}
            for row in rows:
                deps = [physical for token in row["depends_on"].split() for physical in aliases[int(token)]]
                aliases[row["task_id"]] = []
                members = json.loads(row["tp_members"]) if row["tp_members"] else [None]
                for member in members:
                    item = dict(row, task_id=len(expanded), depends_on=" ".join(map(str, deps)),
                                execution_schema="v14-domain-serial-batch-tp-sharded", tp_members="")
                    if member is not None:
                        _, src, sp, dst, dp, size = member
                        item.update(src_node=src, src_port=sp, dst_node=dst, dst_port=dp, bytes=size)
                    aliases[row["task_id"]].append(item["task_id"])
                    expanded.append(item)
            return expanded

        for schema, input_rows, options in (
                ("logical", rows, ()), ("physical", physical_rows(), ()),
                ("per-rank", rows, ("--phase-grouping", "per-rank", "--timing-policy", "error"))):
            with self.subTest(schema=schema):
                source = self.root / f"decode-read-all-{schema}.csv"
                write_csv(source, input_rows)
                output = self.root / f"decode-read-all-{schema}"
                self.convert(source, output, extra=options)
                mapping = read_csv(output / "mapping.csv")
                native = read_csv(output / "traffic.csv")
                computes = [row for row in mapping if row["task_type"] == "DECODE_STEP_COMPUTE"]
                self.assertEqual(len(computes), 2)
                old, new = computes
                reads = [row for row in mapping if row["task_type"] == "STORAGE_TO_D" and row["batch_id"] == "4"]
                read_phases = {row["phaseId"] for row in reads}
                self.assertEqual(len(read_phases), 1)
                writes = {row["request_id"]: row["phaseId"] for row in mapping if row["task_type"] == "PREFIX_STORE_WRITE"}
                self.assertNotEqual(writes["1"], writes["2"])
                for row in reads:
                    self.assertEqual(phases(row), {writes[row["request_id"]]})
                    self.assertEqual(row["delay"], "150us")
                self.assertEqual(phases(new), {old["phaseId"]} | read_phases)
                for row in mapping:
                    if row["task_type"] == "DECODE_WRITE" and row["batch_id"] == "4":
                        self.assertEqual(phases(row), {new["phaseId"]})
                        self.assertEqual(row["delay"], "0us")

                # Controlled network durations test the phase barrier itself:
                # a slow last-layer Read must hold Compute despite faster peers.
                for slow_read in (False, True):
                    phase_done, task_start = {}, {}
                    groups = defaultdict(list)
                    origins = {row["taskId"]: row for row in mapping}
                    for row in native:
                        groups[int(row["phaseId"])].append(row)
                    for phase in sorted(groups):
                        finishes = []
                        for row in groups[phase]:
                            ready = max((phase_done[int(pid)] for pid in phases(row)), default=0)
                            delay = row["delay"]
                            delay_us = (float(delay[:-2]) if delay.endswith("us") else float(delay[:-2]) / 1000)
                            origin = origins[row["taskId"]]
                            if row["opType"] == "TIMER":
                                task_start[row["taskId"]] = ready
                                finishes.append(ready + delay_us)
                            else:
                                task_start[row["taskId"]] = ready + delay_us
                                duration = 10
                                if origin["task_type"] == "DECODE_WRITE" and origin["batch_id"] == "3":
                                    duration = 100000
                                if slow_read and origin in reads and origin["request_id"] == "1" and origin["layer_index"] == "1":
                                    duration = 2000
                                finishes.append(ready + delay_us + duration)
                        phase_done[phase] = max(finishes)
                    old_done = phase_done[int(old["phaseId"])]
                    self.assertTrue(all(task_start[row["taskId"]] < old_done for row in reads))
                    expected = max(old_done, *(phase_done[int(pid)] for pid in read_phases))
                    self.assertEqual(task_start[new["taskId"]], expected)
                    self.assertEqual(expected > old_done, slow_read)
                debug = read_csv(output / "traffic_debug.csv")
                self.assertEqual(len(native), len(debug))
                self.assertTrue(all(row["request_id"] == origin["request_id"] for row, origin in zip(debug, mapping)))

    def test_write_barriers_separate_batches_and_preserve_chunk_request_ids(self):
        # Two batches, each with two requests and two blocks. One TP member
        # exceeds uint32 and must split into two native traffic records.
        rows = []
        for batch in range(2):
            def add(**values):
                row = dict(task_id=len(rows), request_id="", task_class="TIMER",
                           task_type="P_BATCH_BEGIN", depends_on="", release_time_us=0,
                           duration_us=0, src_node="", dst_node="", bytes=0,
                           execution_schema="v14-domain-serial-batch-tp-logical",
                           network_wave="", storage_read_latency_us=0,
                           tp_members="", tp_degree=2, batch_id=batch,
                           batch_type="PREFILL", p_node=0, layer_index=0, block_index="")
                row.update(values)
                rows.append(row)
                return row["task_id"]

            begin = add(depends_on=str(len(rows) - 1) if rows else "")
            compute = add(task_class="COMPUTE", task_type="PREFILL_LAYER",
                          depends_on=str(begin), duration_us=7)
            writes = []
            for request in (0, 1):
                for block in (0, 1):
                    size = (1 << 32) + 17 if request == block == 0 else 32
                    members = [[0, 0, 0, 1, 0, size], [1, 2, 0, 1, 0, 32]]
                    writes.append(add(request_id=request, task_class="NETWORK",
                                      task_type="PREFIX_STORE_WRITE", depends_on=str(compute),
                                      bytes=size + 32, network_wave=f"P:{batch}:1",
                                      tp_members=json.dumps(members), block_index=block))
            add(request_id=0, task_type="P_DONE", depends_on=" ".join(map(str, writes)))
        source = self.root / "two-batches.csv"
        with source.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        output = self.root / "two-batches"
        custom_debug = output / "debug-custom.csv"
        self.convert(source, output, extra=("--debug-output", custom_debug))
        mapping = read_csv(output / "mapping.csv")
        debug = read_csv(custom_debug)
        self.assertFalse((output / "traffic_debug.csv").exists())
        self.assertEqual(len(mapping), len(debug))
        write_phases = defaultdict(set)
        chunks = defaultdict(list)
        for origin, detail in zip(mapping, debug):
            self.assertEqual(origin["taskId"], detail["taskId"])
            self.assertEqual(origin["request_id"], detail["request_id"])
            if origin["task_type"] == "PREFIX_STORE_WRITE":
                write_phases[origin["batch_id"]].add(origin["phaseId"])
                chunks[origin["dag_task_id"]].append(origin)
        self.assertEqual(set(write_phases), {"0", "1"})
        self.assertTrue(all(len(value) == 1 for value in write_phases.values()))
        self.assertTrue(write_phases["0"].isdisjoint(write_phases["1"]))
        split_chunks = [row for rows in chunks.values() for row in rows if row["chunk_count"] == "2"]
        self.assertEqual(len(split_chunks), 4)
        self.assertTrue(all(row["request_id"] == "0" for row in split_chunks))

    def test_debug_path_cannot_overwrite_input_or_native_output(self):
        source = self.outputs["traffic-off"] / "task_dag.csv"
        output = self.root / "path-collision"
        output.mkdir()
        original = b"previous validated traffic\n"
        (output / "traffic.csv").write_bytes(original)
        for path in (source, output / "traffic.csv", output / "mapping.csv", output / "summary.json"):
            result = self.convert(source, output, expected=1, extra=("--debug-output", path))
            self.assertIn("paths must be distinct", result.stderr)
        self.assertEqual((output / "traffic.csv").read_bytes(), original)

    def test_prefill_ignores_decode_policy_options(self):
        source = self.outputs["traffic-off"] / "task_dag.csv"
        output = self.root / "per-rank"
        self.convert(source, output, extra=("--phase-grouping", "per-rank",
                                           "--timing-policy", "conservative"))
        expected = [row for row in read_csv(self.outputs["traffic-off"] / "mapping.csv")
                    if row["batch_type"] == "PREFILL"]
        actual = [row for row in read_csv(output / "mapping.csv")
                  if row["batch_type"] == "PREFILL"]
        self.assertEqual(expected, actual)

    def test_bad_metadata_cannot_silently_publish_legacy_traffic(self):
        good = read_csv(self.outputs["traffic-off"] / "task_dag.csv")
        for mutation in ("missing-column", "empty-value", "wrong-value", "missing-node", "decode-type", "decode-node"):
            with self.subTest(mutation=mutation):
                rows = [dict(row) for row in good]
                fields = list(rows[0])
                if mutation == "missing-column":
                    fields.remove("batch_type")
                    for row in rows:
                        del row["batch_type"]
                elif mutation == "empty-value":
                    next(row for row in rows if row["task_type"] == "PREFIX_READ")["batch_type"] = ""
                elif mutation == "wrong-value":
                    next(row for row in rows if row["task_type"] == "PREFILL_LAYER")["batch_type"] = "DECODE"
                elif mutation == "missing-node":
                    rows[0]["p_node"] = ""
                elif mutation == "decode-type":
                    next(row for row in rows if row["task_type"] == "DECODE_STEP_COMPUTE")["batch_type"] = "PREFILL"
                else:
                    next(row for row in rows if row["task_type"] == "DECODE_STEP_COMPUTE")["d_node"] = ""
                source = self.root / f"{mutation}.csv"
                with source.open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fields)
                    writer.writeheader()
                    writer.writerows(rows)
                output = self.root / mutation
                output.mkdir()
                original = b"previous validated traffic\n"
                (output / "traffic.csv").write_bytes(original)
                (output / "traffic_debug.csv").write_bytes(original)
                result = self.convert(source, output, expected=1)
                self.assertIn("regenerate task_dag.csv", result.stderr)
                self.assertEqual((output / "traffic.csv").read_bytes(), original)
                self.assertEqual((output / "traffic_debug.csv").read_bytes(), original)
                self.assertFalse((output / "summary.json").exists())


if __name__ == "__main__":
    unittest.main()
