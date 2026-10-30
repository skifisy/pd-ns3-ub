"""Manual v8 regression checks. Not executed while producing this delivery."""
import csv
import json
import os
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(HERE))
import prepare as preparation
from prepare import prepare, decode_frame, fingerprint, validate_cache, PROCESSING_FILES
from queries import global_trend, link_trend, progress_rows, policy_windows


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

    def test_bidirectional_tx_wire_bytes_drive_storage_throughput(self):
        # Fixture logs include WRITE payload directed from a Leaf to Storage.
        # No READ_RESPONSE-specific column is necessary.
        with sqlite3.connect(self.cache) as db:
            info=json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            te=next(c for c in info['cases'] if c['id']=='te')
            self.assertTrue(te['storageSampleComplete'])
            self.assertIsNotNone(te['storageUtilization'])
            self.assertIsNotNone(te['storageThroughputGbps'])
            expected_full_run=db.execute(
                "SELECT SUM(storage_bits)/SUM(storage_duration)/1e9 FROM global_samples WHERE case_id='te'"
            ).fetchone()[0]
            self.assertAlmostEqual(te['storageThroughputGbps'],expected_full_run)
            frame=decode_frame(db.execute("SELECT data FROM frames WHERE case_id='te' AND t0=.11").fetchone()[0])
            self.assertTrue(frame['global']['storageSampleComplete'])
            with (HERE/'smoke10ms/te/replay_link_samples.csv').open(newline='') as stream:
                selected=[row for row in csv.DictReader(stream)
                          if (row['src_node'],row['dst_node']) in {('1','11'),('11','1')}
                          and row['start_ns']=='110000000']
            sent=sum(int(row['tx_wire_bytes']) for row in selected if row['src_node']=='1')
            received=sum(int(row['tx_wire_bytes']) for row in selected if row['src_node']=='11')
            expected=(sent+received)*8/.01/1e9
            self.assertEqual((sent,received),(250,6000))
            self.assertAlmostEqual(frame['global']['storageThroughputGbps'],expected)
            self.assertAlmostEqual(frame['global']['storageTxGbps'],sent*8/.01/1e9)
            self.assertAlmostEqual(frame['global']['storageRxGbps'],received*8/.01/1e9)
            wire,capacity,tx_bits,rx_bits=db.execute(
                "SELECT storage_bits,storage_capacity,storage_tx_bits,storage_rx_bits "
                "FROM global_samples WHERE case_id='te' AND t0=.11"
            ).fetchone()
            self.assertEqual((wire,tx_bits,rx_bits),(50000,2000,48000))
            self.assertAlmostEqual(capacity,160000)
            self.assertAlmostEqual(wire/capacity,0.3125)
            self.assertAlmostEqual(frame['global']['fabricUtilization'],.185)
            points=global_trend(db,'te',.11,.12,2,100000)
            self.assertAlmostEqual(points[0]['storageThroughputGbps'],expected*100000)
            self.assertAlmostEqual(points[0]['storageTxGbps']+points[0]['storageRxGbps'],expected*100000)

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

    def test_code_content_changes_invalidate_cache_but_timestamps_do_not(self):
        with tempfile.TemporaryDirectory() as name:
            code = Path(name)
            for source in PROCESSING_FILES:
                shutil.copyfile(HERE/source, code/source)
            with patch.object(preparation, '__file__', str(code/'prepare.py')):
                manifest = HERE/'smoke10ms/experiment.json'
                before = fingerprint(manifest)
                source = code/'recording.py'
                stat = source.stat()
                os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns+10**9))
                self.assertEqual(fingerprint(manifest), before)
                content = source.read_bytes()
                source.write_bytes(b'#'+content[1:])
                os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns))
                self.assertNotEqual(fingerprint(manifest), before)

    def test_use_cache_never_checks_sources_or_rewrites_database(self):
        before = self.cache.stat().st_mtime_ns
        with patch.object(preparation, 'fingerprint', side_effect=AssertionError('read source')):
            self.assertEqual(prepare(self.root/'absent.json',self.cache,use_cache=True),self.cache)
        self.assertEqual(self.cache.stat().st_mtime_ns, before)
        missing = self.root/'absent.sqlite'
        with self.assertRaises(FileNotFoundError):
            prepare(self.root/'absent.json',missing,use_cache=True)
        self.assertFalse(missing.exists())
        with self.assertRaisesRegex(ValueError, 'mutually exclusive'):
            prepare(None,self.cache,force=True,use_cache=True)

    def test_use_cache_rejects_incompatible_database_without_rebuilding(self):
        copied = self.root/'incompatible.sqlite'
        shutil.copyfile(self.cache,copied)
        with sqlite3.connect(copied) as db:
            db.execute('DROP TABLE node_paths')
        before = copied.read_bytes()
        with self.assertRaisesRegex(ValueError,'Incompatible SQLite cache'):
            prepare(None,copied,use_cache=True)
        self.assertEqual(copied.read_bytes(),before)

    def test_legacy_cache_can_be_served_without_policy_details(self):
        copied = self.root/'legacy.sqlite'
        shutil.copyfile(self.cache,copied)
        with sqlite3.connect(copied) as db:
            db.execute("DELETE FROM metadata WHERE key='schema'")
            info = json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            for case in info['cases']:
                case['capabilities'].pop('wcmpPolicy')
            db.execute("UPDATE metadata SET value=? WHERE key='bootstrap'",(json.dumps(info),))
            db.execute('DROP TABLE policy_epochs')
            db.execute('DROP TABLE policy_weights')
        self.assertEqual(validate_cache(copied),copied)

    def test_wcmp_policy_survives_idle_windows_and_resets_on_publication(self):
        copied = self.root/'policy.sqlite'
        shutil.copyfile(self.cache,copied)
        with sqlite3.connect(copied) as db:
            db.execute("DELETE FROM node_paths WHERE case_id='te' AND t0>=.5")
            policy = policy_windows(db,'te',10,11,.5,.6)
            self.assertEqual(policy,[dict(epoch=1,time=.1035,paths=[
                dict(transit=-1,weight=.5),dict(transit=12,weight=.5)])])
            self.assertEqual(policy_windows(db,'te',10,11,0,.1035),[])
            self.assertEqual(policy_windows(db,'te',10,11,.1035,.11),policy)
            # A new global publication with no OD weights must not reuse epoch 1.
            db.execute("INSERT INTO policy_epochs VALUES('te',2,.6)")
            rows = policy_windows(db,'te',10,11,.59,.61)
            self.assertEqual([p['epoch'] for p in rows],[1,2])
            self.assertEqual(policy_windows(db,'te',10,11,.6,.7),[
                dict(epoch=2,time=.6,paths=[])])
            self.assertEqual(policy_windows(db,'te',11,10,.5,.6)[0]['paths'],[])

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
            self.assertIsNone(row['storageThroughputGbps'])
            info=json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            te=next(c for c in info['cases'] if c['id']=='te')
            self.assertIsNone(te['storageUtilization'])
            self.assertIsNone(te['storageThroughputGbps'])
            self.assertIsNotNone(row['fabricUtilization'])


    def test_missing_leaf_to_storage_sample_is_unknown(self):
        copied=self.root/'missing_rx'
        shutil.copytree(HERE/'smoke10ms',copied)
        path=copied/'te/replay_link_samples.csv'
        with path.open(newline='') as stream:
            reader=csv.DictReader(stream);fields=reader.fieldnames
            rows=[r for r in reader if not (r['src_node']=='11' and r['dst_node']=='1')]
        with path.open('w',newline='') as stream:
            writer=csv.DictWriter(stream,fieldnames=fields);writer.writeheader();writer.writerows(rows)
        cache=self.root/'missing_rx.sqlite';prepare(copied/'experiment.json',cache)
        with sqlite3.connect(cache) as db:
            info=json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            te=next(c for c in info['cases'] if c['id']=='te')
            self.assertFalse(te['storageSampleComplete'])
            self.assertIsNone(te['storageUtilization'])
            self.assertIsNone(te['storageThroughputGbps'])


if __name__=='__main__':
    unittest.main()
