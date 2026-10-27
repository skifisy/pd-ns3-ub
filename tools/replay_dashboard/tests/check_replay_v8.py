"""Manual v8 regression checks. Not executed while producing this delivery."""
import csv
import json
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(HERE))
from prepare import prepare, decode_frame
from queries import global_trend, link_trend, progress_rows


class ReplayV8Checks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory()
        cls.root = Path(cls.scratch.name)
        cls.cache = cls.root/'fixture.sqlite'
        prepare(HERE/'smoke10ms/experiment.json',cls.cache)

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def test_window_event_boundaries_and_bounded_bootstrap(self):
        with sqlite3.connect(self.cache) as db:
            info=json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            self.assertEqual(info['windowSeconds'],.01)
            self.assertEqual(info['commonTasks'],5)
            self.assertTrue(all(c['taskSource']=='statistics' for c in info['cases']))
            self.assertTrue(info['synthetic'])
            self.assertTrue(all('completionBins' not in c for c in info['cases']))
            windows=list(db.execute("SELECT t0,t1 FROM frames WHERE case_id='te' AND t0>=.1 AND t0<.11"))
            self.assertEqual(windows,[(.1,.1035),(.1035,.11)])

    def test_old_log_storage_utilization_and_missing_decode_column(self):
        with sqlite3.connect(self.cache) as db:
            info=json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            te=next(c for c in info['cases'] if c['id']=='te')
            self.assertTrue(te['storageSampleComplete'])
            self.assertIsNotNone(te['storageUtilization'])
            self.assertGreaterEqual(te['storageUtilization'],0)
            frame=decode_frame(db.execute("SELECT data FROM frames WHERE case_id='te' AND t0=.11").fetchone()[0])
            self.assertIsNone(frame['global']['storageDecodeGbps'])
            self.assertAlmostEqual(frame['global']['fabricUtilization'],.185)
            points=global_trend(db,'te',.11,.12,2,100000)
            self.assertIsNone(points[0]['storageDecodeGbps'])
            self.assertAlmostEqual(points[0]['fabricUtilization'],.185)

    def test_new_decode_counters_use_only_storage_direction(self):
        copied=self.root/'decode'
        shutil.copytree(HERE/'smoke10ms',copied)
        for cid in ('ecmp','te','toe'):
            path=copied/cid/'replay_link_samples.csv'
            with path.open(newline='') as stream:
                reader=csv.DictReader(stream)
                fields=[*reader.fieldnames,'read_response_tx_wire_bytes']
                rows=list(reader)
            for row in rows:
                # Compute hosts and fabric may also have traffic but must not
                # contribute to the storage-side READ_RESPONSE metric.
                is_storage=row['src_node']=='1' and row['dst_node']=='11'
                row['read_response_tx_wire_bytes'] = (
                    str(int(row['tx_wire_bytes'])//2) if is_storage
                    else str(int(row['tx_wire_bytes']))
                )
            with path.open('w',newline='') as stream:
                writer=csv.DictWriter(stream,fieldnames=fields)
                writer.writeheader();writer.writerows(rows)
        cache=self.root/'decode.sqlite'
        prepare(copied/'experiment.json',cache)
        with sqlite3.connect(cache) as db:
            info=json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            case=next(c for c in info['cases'] if c['id']=='te')
            self.assertIsNotNone(case['storageUtilization'])
            frame=decode_frame(db.execute("SELECT data FROM frames WHERE case_id='te' AND t0=.11").fetchone()[0])
            self.assertTrue(frame['global']['decodeSampleComplete'])
            with (copied/'te/replay_link_samples.csv').open(newline='') as stream:
                selected=[row for row in csv.DictReader(stream)
                          if row['src_node']=='1' and row['dst_node']=='11'
                          and row['start_ns']=='110000000']
            expected=sum(int(row['read_response_tx_wire_bytes']) for row in selected)*8/.01/1e9
            self.assertAlmostEqual(frame['global']['storageDecodeGbps'],expected)
            points=global_trend(db,'te',.11,.12,2,100000)
            self.assertAlmostEqual(points[0]['storageDecodeGbps'],expected*100000)

    def test_reconfiguration_trend_uses_duration_and_capacity_integrals(self):
        with sqlite3.connect(self.cache) as db:
            # The 10 ms interval has 3.5 ms at 4 Mbps + 6.5 ms at 8 Mbps.
            row=link_trend(db,'toe',10,11,.25,.26,1,100000)[0]
            self.assertAlmostEqual(row['capacity'],660)
            self.assertAlmostEqual(row['gbps'],542)
            self.assertAlmostEqual(row['utilization'],542/660)

    def test_progress_boundaries_and_seek_seed(self):
        with sqlite3.connect(self.cache) as db:
            rows=progress_rows(db,'ecmp',.125,.135)
            self.assertEqual(rows[0]['completed'],0)
            self.assertEqual(rows[1]['time'],.13)
            self.assertEqual(rows[1]['completed'],1)
            self.assertEqual(progress_rows(db,'toe',.29,.30)[0]['completed'],2)

    def test_old_sampling_interval_cannot_be_relabelled_as_ten_ms(self):
        copied=self.root/'mismatch'
        shutil.copytree(HERE/'smoke10ms',copied)
        path=copied/'ecmp/replay_run.csv'
        path.write_text(path.read_text().replace('sample_interval_ns,10000000','sample_interval_ns,500000000'))
        with self.assertRaisesRegex(ValueError,'recollect'):
            prepare(copied/'experiment.json',self.root/'mismatch.sqlite')

    def test_missing_access_sample_is_unknown(self):
        copied=self.root/'missing'
        shutil.copytree(HERE/'smoke10ms',copied)
        path=copied/'te/replay_link_samples.csv'
        with path.open(newline='') as stream:
            reader=csv.DictReader(stream);fields=reader.fieldnames
            rows=[r for r in reader if not (r['src_node']=='1' and r['dst_node']=='11')]
        with path.open('w',newline='') as stream:
            writer=csv.DictWriter(stream,fieldnames=fields);writer.writeheader();writer.writerows(rows)
        cache=self.root/'missing.sqlite';prepare(copied/'experiment.json',cache)
        with sqlite3.connect(cache) as db:
            row=global_trend(db,'te',0,1,10,100000)[0]
            self.assertIsNone(row['storageDecodeGbps'])
            info=json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            te=next(c for c in info['cases'] if c['id']=='te')
            self.assertIsNone(te['storageUtilization'])
            self.assertIsNotNone(row['fabricUtilization'])


if __name__=='__main__':
    unittest.main()
