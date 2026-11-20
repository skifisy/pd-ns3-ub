import gzip
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from prepare import parser, prepare
from queries import series
from fixtures import make_case, write_csv


class DashboardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.case = self.root / "case"
        self.windows = make_case(self.case)
        self.args = parser().parse_args(["--case",str(self.case),"--out-dir",str(self.root/"out"),
                                         "--throughput-scale","100000"])

    def tearDown(self):
        self.temp.cleanup()

    def build(self):
        cache = prepare(self.args)
        self.db = sqlite3.connect(cache)
        self.addCleanup(self.db.close)
        self.info = json.loads(self.db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
        self.direction = self.info["pairs"][0]["directions"][0]
        return cache

    def test_roles_capacity_idle_quantiles_and_peak(self):
        self.build()
        self.assertEqual(self.info["roles"], {"P":[37,38],"D":[49],"S":[53,54]})
        self.assertEqual(len(self.info["pairs"]), 6)
        summary = self.info["summaries"][str(self.direction)]
        self.assertAlmostEqual(summary["payload"]["peak_bps"], 2400000)
        self.assertAlmostEqual(summary["wire"]["peak_bps"], 4000000)
        self.assertEqual(summary["wire"]["p99_bps"], 0)  # idle windows count
        self.assertAlmostEqual(summary["wire"]["hot90_seconds"], .001)
        self.assertAlmostEqual(summary["payload"]["mean_bps"], 2400 / self.info["end"])
        coarse = series(self.db,self.info,0,self.info["end"],16,"wire")
        bins = coarse["directions"][str(self.direction)]["bins"]
        self.assertLess(len(bins),128)
        self.assertEqual(max(r[3] for r in bins),400)  # scale applies to peak, too
        self.assertLess(max(r[2] for r in bins),400)
        capacity = coarse["directions"][str(self.direction)]["capacity"]
        self.assertEqual(capacity[0][1],200)  # two 1 Mbps links, one direction only
        self.assertEqual(capacity[1],[.64,100])

    def test_zoom_conserves_bytes_and_boundary_peak(self):
        self.build()
        for a,b in [(0,self.info["end"]),(.65,.651),(.65025,.65075),(.001,.650),(.651,.8)]:
            result = series(self.db,self.info,a,b,16,"wire",["37-53"])
            bins = result["directions"][str(self.direction)]["bins"]
            self.assertAlmostEqual(bins[0][0],a)
            self.assertAlmostEqual(bins[-1][1],b)
            bits = sum((r[1]-r[0])*r[2]*1e9/100000 for r in bins)
            expected = max(0,min(b,.651)-max(a,.650))*4000000
            self.assertAlmostEqual(bits,expected,places=6)
            self.assertEqual(max(r[3] for r in bins),400 if expected else 0)
        # The direction without a direct link is retained with zero capacity and load.
        empty = series(self.db,self.info,0,self.info["end"],16,"wire",["38-54"])
        for value in empty["directions"].values():
            self.assertTrue(all(r[2]==r[4]==0 for r in value["bins"]))

    def test_physical_ports_equal_pair_aggregation(self):
        self.build()
        first = series(self.db,self.info,0,self.info["end"],16,"wire")
        make_case(self.case,physical=True)
        self.args.force = True
        other = prepare(self.args)
        with sqlite3.connect(other) as db:
            self.assertEqual(first,series(db,self.info,0,self.info["end"],16,"wire"))

    def test_gzip_and_cache_reuse(self):
        for path in self.case.glob("replay_*.csv"):
            with gzip.open(str(path)+".gz","wb") as stream:
                stream.write(path.read_bytes())
            path.unlink()
        cache = self.build()
        before = cache.stat().st_mtime_ns
        self.assertEqual(prepare(self.args),cache)
        self.assertEqual(cache.stat().st_mtime_ns,before)

    def test_missing_payload_is_not_zero_demand(self):
        (self.case/"replay_node_path_samples.csv").unlink()
        self.build()
        self.assertEqual(self.info["metrics"],["wire"])
        self.assertTrue(self.info["warnings"])
        with self.assertRaisesRegex(ValueError,"unavailable"):
            series(self.db,self.info,0,.1,16,"payload")

    def test_partial_payload_capture_is_unavailable(self):
        path=self.case/"replay_run.csv"
        path.write_text(path.read_text().replace("detail_windows,all","detail_windows,0s:1s"))
        self.build()
        self.assertEqual(self.info["metrics"],["wire"])

    def test_invalid_inputs_leave_existing_cache_untouched(self):
        cache = self.build()
        before = cache.read_bytes()
        self.args.force = True
        path=self.case/"replay_link_samples.csv"
        lines=path.read_text().splitlines()
        # Drop the entire first sample, leaving the metadata start inconsistent.
        path.write_text("\n".join([lines[0]]+[line for line in lines[1:] if not line.startswith("0,")])+"\n")
        with self.assertRaisesRegex(ValueError,"start_ns"):
            prepare(self.args)
        self.assertEqual(cache.read_bytes(),before)

    def test_malformed_capacity_and_missing_sample_rejected(self):
        path=self.case/"replay_link_samples.csv"
        text=path.read_text()
        path.write_text(text.replace(",20000.0,",",19000.0,",1))
        with self.assertRaisesRegex(ValueError,"capacity_bits"):
            prepare(self.args)
        make_case(self.case)
        lines=path.read_text().splitlines()
        path.write_text("\n".join([lines[0]]+[line for line in lines[1:] if not line.startswith("20,")])+"\n")
        with self.assertRaisesRegex(ValueError,"contiguous"):
            prepare(self.args)

    def test_explicit_roles_without_node_csv(self):
        (self.case/"node.csv").unlink()
        (self.case/"topology.csv").unlink()
        self.args.prefill_leaves="37-38"
        self.args.decode_leaves="49"
        self.args.storage_leaves="53,54"
        self.build()
        self.assertEqual(len(self.info["pairs"]),6)

    def test_api_validation_and_local_assets(self):
        from fastapi.testclient import TestClient
        from server import create_app
        cache=self.build()
        with TestClient(create_app(cache)) as client:
            self.assertEqual(client.get("/").status_code,200)
            self.assertEqual(client.get("/vendor/echarts.min.js").status_code,200)
            self.assertEqual(client.get("/api/bootstrap").json()["case_id"],"ecmp")
            for params in [dict(start=0,end=0),dict(start="nan",end=1),dict(start=-1,end=1),
                           dict(start=0,end=1,pairs="unknown"),dict(start=0,end=1,points=3000)]:
                self.assertIn(client.get("/api/series",params=params).status_code,(400,422))
            response=client.get("/api/series",params=dict(start=.65,end=.651,metric="wire"))
            self.assertEqual(response.status_code,200)
            self.assertEqual(response.json()["level"],0)


if __name__ == "__main__":
    unittest.main()
