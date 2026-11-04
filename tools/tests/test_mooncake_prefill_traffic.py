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
        self.assertEqual(summary["timer_rows"], 79)
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
                    if native["opType"] == "TIMER" and origin["task_type"] != "DECODE_READ_COMPUTE_WAIT":
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

    def test_decode_batch_keeps_prefill_write_and_prior_decode_compute(self):
        rows = []

        def add(batch, **values):
            row = dict(task_id=len(rows), request_id="", task_class="TIMER",
                       task_type="P_BATCH_BEGIN", depends_on="", release_time_us="",
                       duration_us=0, src_node="", src_port="", dst_node="", dst_port="", bytes=0,
                       execution_schema="v14-domain-serial-batch-tp-logical",
                       network_wave="", storage_read_latency_us=150,
                       tp_members="", tp_degree=2, batch_id=batch,
                       batch_type="PREFILL" if batch < 2 else "DECODE",
                       p_node=0, layer_index=0, block_index="")
            row.update(values)
            rows.append(row)
            return row["task_id"]

        done = []
        for batch in range(2):
            begin = add(batch, release_time_us=1000)
            compute = add(batch, task_class="COMPUTE", task_type="PREFILL_LAYER",
                          depends_on=str(begin), duration_us=10)
            first_write = add(batch, request_id=batch, task_class="NETWORK",
                              task_type="PREFIX_STORE_WRITE", depends_on=str(compute),
                              block_index=0, bytes=64, network_wave=f"P:{batch}:1",
                              tp_members=json.dumps([[0, 0, 0, 1, 0, 32], [1, 2, 0, 1, 0, 32]]))
            compute = add(batch, task_class="COMPUTE", task_type="PREFILL_LAYER",
                          depends_on=str(compute), duration_us=11, layer_index=1)
            last_write = add(batch, request_id=batch, task_class="NETWORK",
                             task_type="PREFIX_STORE_WRITE", depends_on=str(compute),
                             layer_index=1, block_index=0, bytes=64, network_wave=f"P:{batch}:2",
                             tp_members=json.dumps([[0, 0, 0, 1, 0, 32], [1, 2, 0, 1, 0, 32]]))
            done.append(add(batch, request_id=batch, task_type="P_DONE",
                            depends_on=f"{first_write} {last_write} {compute}"))
        begin = add(2, task_type="D_BATCH_BEGIN", depends_on=str(done[0]), release_time_us=900000)
        read = add(2, request_id=0, task_class="NETWORK", task_type="STORAGE_TO_D",
                   depends_on=str(begin), block_index=0, bytes=64, network_wave="D:2:0",
                   tp_members=json.dumps([[0, 1, 0, 0, 0, 32], [1, 1, 0, 2, 0, 32]]))
        compute = add(2, task_class="COMPUTE", task_type="DECODE_LAYER",
                      depends_on=str(read), duration_us=20)
        free = add(2, task_type="D_COMPUTE_DONE", depends_on=str(compute))
        begin = add(3, task_type="D_BATCH_BEGIN", depends_on=f"{free} {done[1]}",
                    release_time_us=999999999)
        for block in (0, 1):
            add(3, request_id=1, task_class="NETWORK", task_type="STORAGE_TO_D",
                depends_on=str(begin), block_index=block, bytes=64, network_wave="D:3:0",
                tp_members=json.dumps([[0, 1, 0, 0, 0, 32], [1, 1, 0, 2, 0, 32]]))

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
                        rank, src, sp, dst, dp, size = member
                        item.update(src_node=src, src_port=sp, dst_node=dst, dst_port=dp, bytes=size)
                    aliases[row["task_id"]].append(item["task_id"])
                    expanded.append(item)
            return expanded

        for schema, input_rows in (("logical", rows), ("physical", physical_rows())):
            with self.subTest(schema=schema):
                source = self.root / f"decode-compute-wait-{schema}.csv"
                write_csv(source, input_rows)
                output = self.root / f"decode-compute-wait-{schema}"
                self.convert(source, output)
                mapping = read_csv(output / "mapping.csv")
                read_rows = [row for row in mapping if row["task_type"] == "STORAGE_TO_D"]
                self.assertEqual({row["delay"] for row in read_rows}, {"150us"})
                final_write = {row["batch_id"]: row["phaseId"] for row in mapping
                               if row["task_type"] == "PREFIX_STORE_WRITE" and row["layer_index"] == "1"}
                old_reads = [row for row in read_rows if row["batch_id"] == "2"]
                new_reads = [row for row in read_rows if row["batch_id"] == "3"]
                self.assertTrue(all(phases(row) == {final_write["0"]} for row in old_reads))
                waits = [row for row in mapping if row["task_type"] == "DECODE_READ_COMPUTE_WAIT"]
                self.assertEqual(len(waits), 1)
                self.assertEqual(waits[0]["delay"], "20us")
                self.assertEqual(phases(waits[0]), {row["phaseId"] for row in old_reads})
                expected = {final_write["1"], waits[0]["phaseId"]}
                self.assertTrue(all(phases(row) == expected for row in new_reads))
                self.assertEqual(len({row["phaseId"] for row in new_reads}), 1)
                native = read_csv(output / "traffic.csv")
                debug = read_csv(output / "traffic_debug.csv")
                self.assertEqual(len(native), len(debug))
                for detail, origin in zip(debug, mapping):
                    self.assertEqual(detail["request_id"], origin["request_id"])
                timer = next(row for row in native if row["phaseId"] == waits[0]["phaseId"])
                self.assertEqual(timer["opType"], "TIMER")

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
        for mutation in ("missing-column", "empty-value", "wrong-value", "missing-node"):
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
                else:
                    rows[0]["p_node"] = ""
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
