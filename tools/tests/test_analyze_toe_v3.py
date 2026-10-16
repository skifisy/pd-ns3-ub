"""Regression tests for the actual single-file ToE analysis entry point."""
import csv
import contextlib
import importlib.util
import io
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("analyze_toe_test_target", Path(__file__).resolve().parents[1] / "analyze_toe_v3.py")
MOD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MOD
SPEC.loader.exec_module(MOD)

def write_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f); w.writerow(header); w.writerows(rows)


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


INITIAL = {(37, 53): 4, (37, 54): 4, (38, 53): 4, (38, 54): 4}
TARGET = {(37, 53): 6, (37, 54): 2, (38, 53): 2, (38, 54): 6}
TRAFFIC_HEADER = ["taskId", "sourceNode", "destNode", "dataSize(Byte)", "opType", "priority", "delay", "phaseId", "dependOnPhases"]
TRAFFIC = [[i, 0, 1, 1000 if i in (0, 1, 6, 7) else 100, "URMA_READ", 7, "0us", i, ""] for i in range(8)]
STARTS = [900000, 1200000, 0, 50, 2500000, 2700000, 1950000, 3200000]
BASE_FINISH = [1100000, 1300000, 0, 52, "NaN", 2700010, 2150000, 3500000]
TOE_FINISH = [1120000, 1280000, 0, "52.0000000002", "NaN", 2700010, 2100000, 3490000]


def circuits(snapshot):
    ports = {37: 1, 38: 1, 53: 1, 54: 1}
    rows = []
    for (a, b), count in sorted(snapshot.items()):
        for _ in range(count):
            rows.append((a, ports[a], b, ports[b]))
            ports[a] += 1; ports[b] += 1
    return rows


def make_case(path, toe=False, ideal=False, traces=True, physical=True):
    path.mkdir()
    write_csv(path / "traffic.csv", TRAFFIC_HEADER, TRAFFIC)
    write_csv(path / "node.csv", ["nodeId", "nodeType", "portNum", "forwardDelay"],
              [["0..1", "DEVICE", 2, "1ns"], ["37..38", "SWITCH", 20, "1ns"], ["53..54", "SWITCH", 20, "1ns"]])
    links = [[0, 0, 37, 0, "400Gbps", "1us"], [0, 1, 38, 0, "400Gbps", "1us"],
             [1, 0, 53, 0, "400Gbps", "1us"], [1, 1, 54, 0, "400Gbps", "1us"]]
    links += [[a, ap, b, bp, "400Gbps", "1us"] for a, ap, b, bp in circuits(INITIAL)]
    write_csv(path / "topology.csv", ["nodeId1", "portId1", "nodeId2", "portId2", "bandwidth", "delay"], links)
    finishes = ([1090000, 1270000, 0, 52, "NaN", 2700010, 2080000, 3450000] if ideal else TOE_FINISH if toe else BASE_FINISH)
    rows = [t + [STARTS[i], finishes[i]] for i, t in enumerate(TRAFFIC)]
    write_csv(path / "output/task_statistics.csv", TRAFFIC_HEADER + ["taskStartTime(us)", "taskCompletesTime(us)"], rows)
    if toe or ideal:
        summary = []
        for ep, solved, applied, pause in [(1, 1, 1.05, 0 if ideal else .05), (2, 2, 2.001, 0 if ideal else .001)]:
            summary += [[ep, solved, "solved", 1e-7, 1.01e-7, 4, 1, 0, 0]]
            if not ideal:
                summary += [[ep, solved, "draining", 1e-7, 1.01e-7, 4, 1, 0, 0]]
            summary += [[ep, applied, "applied", 1e-7, 1.01e-7, 4, 1, pause, pause]]
        summary += [[3, 3, "solved", 1e-7, 1.01e-7, 0, 1, 0, 0]]
        write_csv(path / "output/toe_summary.csv", ["epoch", "time_seconds", "status", "min_mlu", "applied_mlu_bound", "changed_circuits", "solver_optimal", "drain_seconds", "pause_seconds"], summary)
        topology = []
        for ep, t, status, snap in [(0, 0, "initial", INITIAL), (1, 1, "target", TARGET),
                                     (1, 1.05, "applied", TARGET), (2, 2.001, "applied", INITIAL), (3, 3, "target", INITIAL)]:
            topology += [[ep, t, status, a, b, n, n * 400] for (a, b), n in snap.items()]
        write_csv(path / "output/toe_topology.csv", ["epoch", "time_seconds", "status", "leaf_a", "leaf_b", "circuits", "capacity_gbps"], topology)
        if physical:
            physical_rows = [[ep, t, "applied", a, ap, b, bp] for ep, t, snap in [(1, 1.05, TARGET), (2, 2.001, INITIAL)]
                             for a, ap, b, bp in circuits(snap)]
            write_csv(path / "output/toe_circuits.csv", ["epoch", "time_seconds", "status", "leaf_a", "port_a", "leaf_b", "port_b"], physical_rows)
        if toe:
            write_csv(path / "output/task_toe_exposure.csv", ["task_id", "epoch", "pause_start_us", "pause_end_us"],
                      [[0, 1, 1000000, 1040000], [0, 1, 1020000, 1050000]])
            write_csv(path / "output/pair_metrics.csv", ["time_us", "src_leaf", "dst_leaf", "queue_bytes", "backlog_bytes"],
                      [[900000, 37, 53, 2000, 10000], [920000, 37, 53, 2500, 12000],
                       [1200000, 37, 53, 500, 1000], [1250000, 37, 53, 600, 1200]])
    if traces:
        (path / "runlog").mkdir()
        (path / "runlog/L1PairTrace.tr").write_text(
            "[900000us] L1 Pair Tx, SrcL1: 37 DstL1: 53 PacketUid: 1 PacketSize: 1000 TaskId: 0\n"
            "[1060000us] L1 Pair Tx, SrcL1: 53 DstL1: 37 PacketUid: 2 PacketSize: 1000 TaskId: 0\n"
            "[1210000us] L1 Pair Tx, SrcL1: 37 DstL1: 53 PacketUid: 3 PacketSize: 1000 TaskId: 1\n"
            "[1960000us] L1 Pair Tx, SrcL1: 37 DstL1: 53 PacketUid: 4 PacketSize: 1000 TaskId: 6\n"
            "[2010000us] L1 Pair Tx, SrcL1: 53 DstL1: 38 PacketUid: 5 PacketSize: 1000 TaskId: 6\n", encoding="utf-8")
        (path / "runlog/PortTrace_node_0_port_0.tr").write_text(
            "[1210000us] Port Tx, port ID: 0 PacketSize: 1000 TaskId: 1\n", encoding="utf-8")
        (path / "runlog/PortTrace_node_37_port_5.tr").write_text(
            "[1210000us] Port Tx, port ID: 5 PacketSize: 1000 TaskId: 1\n", encoding="utf-8")
        (path / "runlog/PortTrace_node_53_port_0.tr").write_text(
            "[1210001us] Port Tx, port ID: 0 PacketSize: 1000 TaskId: 1\n", encoding="utf-8")



def real_case(path, toe=False):
    make_case(path, toe=toe)
    if not toe:
        return
    directory = path / "toe"
    directory.mkdir()
    for file in (path / "output").glob("toe_*.csv"):
        file.rename(directory / file.name)
    file = directory / "toe_topology.csv"
    file.write_text(file.read_text().replace("leaf_a,leaf_b,circuits,capacity_gbps", "src_leaf,dst_leaf,circuit_count,capacity_gbps_per_direction"))
    file = directory / "toe_circuits.csv"
    file.write_text(file.read_text().replace("leaf_a,port_a,leaf_b,port_b", "src_leaf,src_port,dst_leaf,dst_port"))
    # Partial planning records must never be merged into full applied snapshots.
    with file.open("a") as f:
        f.write("1,1.0,retained,37,999,53,999\n")
        f.write("1,1.0,remove,37,998,54,998\n")
        f.write("1,1.0,add,37,997,53,997\n")
    header = ["epoch","time_seconds","status","scope","affected_leaf_pairs","paused_tp_channels","total_tp_channels",
              "retained_circuits","rewired_circuits","unpaused_new_payload_bytes","pause_seconds"]
    write_csv(directory / "toe_reconfiguration.csv", header,
              [[ep,t,status,"selective",4,8,16,12,4,2048,pause]
               for ep,t,pause in ((1,1.05,.05),(2,2.001,.001)) for status in ("applied",)])


class UnifiedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        real_case(self.root / "ecmp")
        real_case(self.root / "toe", toe=True)

    def tearDown(self):
        self.temp.cleanup()

    def run_combined(self, *extra, output="result"):
        stream = io.StringIO()
        argv = ["--baseline", str(self.root/"ecmp"), "--toe", str(self.root/"toe"), "--output", str(self.root/output), *map(str,extra)]
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            status = MOD.main(argv)
        return status, self.root/output, stream.getvalue()

    def test_real_repository_logs_and_zero_task_csv_rereads(self):
        real_rows = MOD.rows
        opens = []
        real_open = Path.open
        def guarded_rows(source, name):
            self.assertNotIn(name, ("task_compare.csv", "task_link_usage.csv"))
            return real_rows(source, name)
        def tracked_open(path, *args, **kwargs):
            mode = args[0] if args else kwargs.get("mode", "r")
            if path.name == "toe_topology.csv" and "r" in mode:
                opens.append(path)
            return real_open(path, *args, **kwargs)
        with patch.object(MOD,"rows",side_effect=guarded_rows), patch.object(MOD,"scan_tasks",side_effect=AssertionError("second scan")), patch.object(MOD,"summary_main",side_effect=AssertionError("second analysis")), patch.object(Path,"open",tracked_open):
            status,out,log = self.run_combined("--include-link-samples")
        self.assertEqual(status,0,log)
        self.assertEqual(len(opens),1)
        report = json.loads((out/"analysis_summary.json").read_text())
        self.assertEqual(report["pipeline"]["task_compare_csv_read_passes"],0)
        self.assertEqual(report["pipeline"]["task_rows_consumed"],8)
        events = read_csv(out/"reconfiguration_events.csv")
        self.assertEqual(len(events),3)
        self.assertEqual(events[0]["pause_scope"],"selective")
        self.assertEqual(events[0]["paused_tp_count"],"8")
        self.assertEqual(events[0]["unpaused_tx_bytes"],"2048")
        run = read_csv(out/"run_comparison_summary.csv")[0]
        self.assertEqual(run["decision_count"],"3")
        self.assertEqual(run["actual_reconfiguration_count"],"2")
        self.assertAlmostEqual(float(run["pause_timeline_union_seconds"]),.051)
        groups = read_csv(out/"topology_group_changes_v3.csv")
        ps = [r for r in groups if r["leaf_group"]=="P↔S"]
        self.assertEqual([(r["circuits_before"],r["circuits_after"]) for r in ps],[("16","16"),("16","16")])
        self.assertEqual(len(read_csv(out/"topology_changes.csv")),8)
        focus = next(r for r in read_csv(out/"task_focus_summary.csv") if r["dimension"] == "all")
        self.assertEqual(sum(int(focus[k + "_tasks"]) for k in ("tail_enter", "tail_exit", "tail_stay", "normal_stay")),7)
        self.assertEqual(int(focus["pause_known_tasks"]),7)
        self.assertLessEqual(len((out/"toe_ai_summary_v3.txt").read_text().splitlines()),1500)
        self.assertIn("summary_render",report["stage_seconds"])
        self.assertTrue(report["cases"][1]["capabilities"]["dynamic_physical_snapshots"])

    def test_streamed_focus_equals_explicit_summary_only(self):
        status,out,log = self.run_combined()
        self.assertEqual(status,0,log)
        expected = read_csv(out/"task_focus_summary.csv")
        directory = self.root/"reformatted"
        with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(MOD.main(["--summary-only",str(out),"--output",str(directory/"report.txt")]),0)
        actual = read_csv(directory/"task_focus_summary.csv")
        self.assertEqual(len(actual),len(expected))
        for left,right in zip(expected,actual):
            for key in left:
                if key=="threshold_source":continue
                self.assertEqual(left[key],right[key],key)

    def test_missing_logs_fail_before_tasks_or_traces(self):
        for file in (self.root/"toe/toe").glob("*.csv"):file.unlink()
        with patch.object(MOD,"load_tasks",side_effect=AssertionError("expensive input read")):
            status,out,log = self.run_combined()
        self.assertEqual(status,2)
        self.assertIn("--toe-log-dir",log)
        self.assertIn(str(self.root/"toe/toe/toe_summary.csv"),log)
        self.assertFalse((out/"task_compare.csv").exists())

    def test_explicit_fct_only_retains_unknown_timeline(self):
        for file in (self.root/"toe/toe").glob("*.csv"):file.unlink()
        status,out,log = self.run_combined("--allow-missing-toe-logs","--trace-mode","off")
        self.assertEqual(status,0,log)
        self.assertEqual(read_csv(out/"reconfiguration_events.csv"),[])
        task = read_csv(out/"task_compare.csv")[0]
        self.assertEqual(task["pause_overlap_us"],"")
        self.assertEqual(task["topology_epoch_start_toe"],"")

    def test_custom_toe_log_directory_and_stale_duplicate(self):
        external = self.root/"custom"
        (self.root/"toe/toe").rename(external)
        (self.root/"toe/toe_summary.csv").write_text("stale,file\n1,2\n")
        status,out,log = self.run_combined("--toe-log-dir","ToE",external)
        self.assertEqual(status,0,log)
        files = json.loads((out/"analysis_summary.json").read_text())["cases"][1]["files"]
        self.assertEqual(Path(files["toe_summary"]),external/"toe_summary.csv")

    def test_ambiguity_is_reported_before_scanning(self):
        (self.root/"toe/toe_summary.csv").write_text("stale,file\n1,2\n")
        with patch.object(MOD,"load_tasks",side_effect=AssertionError("late validation")):
            status,out,log = self.run_combined()
        self.assertEqual(status,2)
        self.assertIn("ambiguous",log)

    def test_focus_limit_does_not_truncate_legacy_groups(self):
        status,out,log = self.run_combined("--focus-max-groups","1")
        self.assertEqual(status,0,log)
        groups = {r["group"]:r for r in read_csv(out/"fct_group_summary.csv")}
        self.assertEqual(groups["all"]["paired_tasks"],"7")
        self.assertEqual(int(groups["small"]["paired_tasks"])+int(groups["large"]["paired_tasks"]),7)

    def test_bad_summary_option_fails_before_loading_tasks(self):
        with patch.object(MOD,"load_tasks",side_effect=AssertionError("late argument check")):
            status,out,log = self.run_combined("--max-lines","10")
        self.assertEqual(status,2)
        self.assertIn("80..1500",log)

    def test_numeric_rank_query_matches_linear_percentiles(self):
        import sqlite3
        con=sqlite3.connect(":memory:")
        con.execute("CREATE TABLE numbers(v INTEGER)")
        for values in ([],[1],[9,1],[10,3,3,100,5,2,7]):
            con.execute("DELETE FROM numbers")
            con.executemany("INSERT INTO numbers VALUES(?)",[(v,) for v in values])
            actual=MOD.quantiles(con,"SELECT v FROM numbers ORDER BY v",(),len(values))
            for name,q in (("p50",.5),("p95",.95),("p99",.99),("max",1)):
                expected=MOD.percentile(sorted(values),q)
                if expected is None:self.assertIsNone(actual[name])
                else:self.assertAlmostEqual(actual[name],expected)
        con.close()



def make_demand(path, complete=True, zero=False):
    path.parent.mkdir(parents=True,exist_ok=True)
    rows=[]
    for ep,stamp,gbps in ((1,1.0,10),(2,2.0,5),(3,3.0,20)):
        for a in range(37,57):
            for b in range(37,57):
                value=0 if zero or (a,b)!=(53,37) else gbps
                rows.append([ep,stamp,a,b,value*1e9,value,value*1e9*30/8,0,0,30,20,ep,int(complete),"solver_input"])
    write_csv(path,["epoch","decision_seconds","src_leaf","dst_leaf","demand_bps","demand_gbps","peak_window_bytes","latest_window_bytes","latest_gbps","sample_window_seconds","history_windows","completed_window","matrix_complete","matrix_kind"],rows)


class DemandTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        real_case(self.root/"ecmp");real_case(self.root/"toe",toe=True)
        self.raw=self.root/"toe/toe/toe_demand.csv";make_demand(self.raw)

    def tearDown(self):self.temp.cleanup()

    def combined(self,*extra,output="result"):
        stream=io.StringIO()
        with contextlib.redirect_stdout(stream),contextlib.redirect_stderr(stream):
            code=MOD.main(["--baseline",str(self.root/"ecmp"),"--toe",str(self.root/"toe"),"--output",str(self.root/output),*map(str,extra)])
        return code,self.root/output,stream.getvalue()

    def test_default_changes_and_real_direction(self):
        code,out,log=self.combined()
        self.assertEqual(code,0,log)
        rows=read_csv(out/"demand_epoch_summary.csv")
        self.assertEqual([r["epoch"] for r in rows],["1","2","3"])
        self.assertTrue(rows[0]["demand_svg"])
        self.assertFalse(rows[2]["demand_svg"])
        self.assertEqual(rows[0]["decision_seconds"],"1.0")
        self.assertEqual(rows[0]["applied_seconds"],"1.05")
        self.assertEqual(rows[1]["delta_reference_epoch"],"1")
        self.assertEqual(len(read_csv(out/"demand_matrices.csv")),1200)
        self.assertEqual(len(list((out/"demand_heatmaps").glob("*.svg"))),5)
        svg=(out/rows[1]["demand_delta_svg"]).read_text()
        self.assertIn("S53 → P37: -5 Gbps",svg)
        self.assertIn("P37 → S53: 0 Gbps",svg)
        matrix=MOD.read_demand_file(self.raw,"ToE")[1]
        groups=MOD.role_demand(matrix["cells"])
        self.assertEqual(groups["S","P"],10)
        self.assertEqual(groups["P","S"],0)

    def test_all_decisions_also_renders_no_change(self):
        code,out,log=self.combined("--demand-heatmaps","all")
        self.assertEqual(code,0,log)
        rows=read_csv(out/"demand_epoch_summary.csv")
        self.assertTrue(rows[2]["demand_svg"])
        self.assertEqual(rows[2]["event_status"],"no_change")
        self.assertEqual(rows[2]["delta_reference_epoch"],"2")
        self.assertEqual(len(list((out/"demand_heatmaps").glob("*.svg"))),8)

    def test_explicit_matrix_file_is_read_once_in_combined_mode(self):
        original=MOD.read_demand_file
        with patch.object(MOD,"read_demand_file",wraps=original) as reader:
            code,out,log=self.combined("--demand-file","ToE",self.raw)
        self.assertEqual(code,0,log)
        self.assertEqual(reader.call_count,1)

    def test_missing_solver_inputs_are_not_zero_matrices(self):
        self.raw.unlink()
        code,out,log=self.combined()
        self.assertEqual(code,0,log)
        rows=read_csv(out/"demand_epoch_summary.csv")
        self.assertTrue(all(r["coverage"]=="missing_solver_demand_log" for r in rows))
        self.assertEqual(read_csv(out/"demand_matrices.csv"),[])
        self.assertIn("Missing solver-demand log",(out/"demand_heatmaps.html").read_text())
        self.assertEqual(list((out/"demand_heatmaps").glob("*.svg")),[])

    def test_incomplete_declared_full_matrix_is_rejected(self):
        text=self.raw.read_text().splitlines()
        self.raw.write_text("\n".join(text[:-1])+"\n")
        with self.assertRaisesRegex(ValueError,"400 cells"):
            MOD.read_demand_file(self.raw,"ToE")

    def test_partial_input_preserves_unknown_cells(self):
        rows=read_csv(self.raw)
        for r in rows:r["matrix_complete"]="0"
        rows=[r for r in rows if not(r["epoch"]=="1" and r["src_leaf"]=="37" and r["dst_leaf"]=="53")]
        with self.raw.open("w",newline="") as f:
            w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
        code,out,log=self.combined()
        self.assertEqual(code,0,log)
        summary=read_csv(out/"demand_epoch_summary.csv")[0]
        self.assertEqual(summary["coverage"],"partial_missing_cells")
        svg=(out/summary["demand_svg"]).read_text()
        self.assertIn("P37 → S53: NA",svg)

    def test_bps_gbps_mismatch_and_negative_values_are_rejected(self):
        for replacement in ("1,999,", "-1,-1e-9,"):
            make_demand(self.raw)
            value=self.raw.read_text().replace("10000000000.0,10,",replacement,1)
            self.raw.write_text(value)
            with self.assertRaises(ValueError):MOD.read_demand_file(self.raw,"ToE")

    def test_demand_only_never_reads_tasks_or_traces(self):
        code,out,log=self.combined()
        self.assertEqual(code,0,log)
        with patch.object(MOD,"scan_tasks",side_effect=AssertionError("task scan")),patch.object(MOD,"load_tasks",side_effect=AssertionError("raw tasks")),patch.object(MOD,"summary_main",side_effect=AssertionError("FCT summary")):
            stream=io.StringIO()
            with contextlib.redirect_stdout(stream),contextlib.redirect_stderr(stream):
                code=MOD.main(["--demand-only",str(out),"--output",str(self.root/"redraw"),"--demand-heatmaps","all"])
        self.assertEqual(code,0,stream.getvalue())
        self.assertEqual(len(read_csv(self.root/"redraw/demand_matrices.csv")),1200)
        self.assertFalse((self.root/"redraw/task_compare.csv").exists())

    def test_solver_timestamp_mismatch_is_rejected(self):
        code,out,log=self.combined()
        self.assertEqual(code,0,log)
        self.raw.write_text(self.raw.read_text().replace("1,1.0,","1,99.0,"))
        stream=io.StringIO()
        with contextlib.redirect_stdout(stream),contextlib.redirect_stderr(stream):
            code=MOD.main(["--demand-only",str(out),"--demand-file","ToE",str(self.raw),"--output",str(self.root/"bad")])
        self.assertEqual(code,2)
        self.assertIn("differs from solved time",stream.getvalue())

    def test_svg_is_valid_xml_and_titles_are_escaped(self):
        import xml.etree.ElementTree as ET
        svg=MOD.heatmap_svg({("A","A"):0},["A"],"<unsafe>&title","<script>",0)
        ET.fromstring(svg)
        self.assertIn("&lt;unsafe&gt;&amp;title",svg)
        self.assertNotIn("<script>",svg)

    def test_zero_complete_matrix_is_known_zero(self):
        make_demand(self.raw,zero=True)
        code,out,log=self.combined()
        self.assertEqual(code,0,log)
        row=read_csv(out/"demand_epoch_summary.csv")[0]
        self.assertEqual(row["coverage"],"complete")
        self.assertEqual(float(row["total_known_demand_gbps"]),0)
        self.assertEqual(row["known_cells"],"400")


if __name__=="__main__":
    unittest.main()
