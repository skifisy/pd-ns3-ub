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
from metrics import load_metrics, attach_metrics, COUNTS, VALUES


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
            self.assertNotIn('commonTasks',info)
            self.assertTrue(all('quantiles' not in c for c in info['cases']))
            tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            self.assertNotIn('cohort',tables)
            self.assertNotIn('fct_values',tables)
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
            self.assertNotIn('storageUtilization',te)
            self.assertNotIn('storageThroughputGbps',te)
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
            self.assertNotIn('storageThroughputGbps',points[0])
            self.assertAlmostEqual(points[0]['fabricUtilization'],.185)

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

    def test_summary_changes_do_not_invalidate_replay_cache(self):
        copied = self.root/'independent_summary'
        shutil.copytree(HERE/'smoke10ms',copied)
        manifest = copied/'experiment.json'
        before = fingerprint(manifest)
        config = json.loads(manifest.read_text())
        config['metrics_csv'] = 'te/summary.tsv'
        config['metrics_tps_unit'] = 'token/s'
        config['cases'][0]['metrics_scheme'] = 'ECMP'
        manifest.write_text(json.dumps(config))
        summary = copied/'te/summary.tsv'
        summary.write_text('summary is loaded by the server, not the replay importer')
        self.assertEqual(fingerprint(manifest),before)
        summary.write_text('updated externally computed metrics')
        self.assertEqual(fingerprint(manifest),before)

    def test_new_summary_overlays_an_existing_cache_without_reimport(self):
        config = json.loads((HERE/'smoke10ms/experiment.json').read_text())
        metrics = load_metrics(HERE/'smoke10ms/experiment.json',config)
        with sqlite3.connect(self.cache) as db:
            cached = json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
        before = self.cache.stat().st_mtime_ns
        with patch.object(preparation,'fingerprint',side_effect=AssertionError('read trace')):
            prepare(None,self.cache,use_cache=True)
            info = attach_metrics(cached,metrics)
        self.assertEqual(info['cases'][0]['metrics']['fct_p999_ms'],450)
        self.assertEqual(self.cache.stat().st_mtime_ns,before)

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
            self.assertNotIn('storageThroughputGbps',row)
            info=json.loads(db.execute("SELECT value FROM metadata WHERE key='bootstrap'").fetchone()[0])
            te=next(c for c in info['cases'] if c['id']=='te')
            self.assertNotIn('storageUtilization',te)
            self.assertNotIn('storageThroughputGbps',te)
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
            self.assertNotIn('storageUtilization',te)
            self.assertNotIn('storageThroughputGbps',te)
            frame=decode_frame(db.execute("SELECT data FROM frames WHERE case_id='te' LIMIT 1").fetchone()[0])
            self.assertFalse(frame['global']['storageSampleComplete'])


class SummaryMetricsChecks(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.manifest = self.root/'experiment.json'
        self.config = dict(metrics_csv='summary.csv',throughputScale=100000,
                           cases=[dict(id='ecmp'),dict(id='te'),dict(id='toe')])
        self.rows = []
        for scheme in ('ECMP','TE','TOE'):
            self.rows.append(dict(scheme=scheme, **dict.fromkeys(COUNTS,1092),
                **dict(zip(VALUES,('12263.93644','20277.81115','25343.88748','25343.88748',
                                   '7.526006','38.022006','90.786006','139.028128',
                                   '1.929938722','6.22583117')))))
            self.rows[-1].update(valid_fct_flows=16517940,common_valid_fct_flows=16517940)

    def write_summary(self,delimiter=',',fields=None):
        with (self.root/'summary.csv').open('w',encoding='utf-8-sig',newline='') as stream:
            writer = csv.DictWriter(stream,fieldnames=fields or ['scheme',*COUNTS,*VALUES],
                                    delimiter=delimiter,extrasaction='ignore')
            writer.writeheader()
            writer.writerows(self.rows)

    def test_reads_all_quantiles_and_averages_without_rescaling(self):
        for delimiter in (',','\t'):
            with self.subTest(delimiter=delimiter):
                self.write_summary(delimiter)
                metrics = load_metrics(self.manifest,self.config)
                self.assertEqual(metrics['cases']['ecmp']['fct_p999_ms'],139.028128)
                self.assertEqual(metrics['cases']['ecmp']['ttft_p999_ms'],25343.88748)
                self.assertEqual(metrics['cases']['ecmp']['avg_tps'],1.929938722)
                self.assertEqual(metrics['cases']['ecmp']['avg_throughput_gbps'],6.22583117)

    def test_legacy_values_are_ignored_without_mutating_cached_bootstrap(self):
        self.write_summary()
        cached = dict(commonTasks=42,statistics={'op_types':['WRITE']},cases=[
            dict(id=case['id'],quantiles={'p99':999},storageThroughputGbps=123,
                 storageUtilization=.9) for case in self.config['cases']])
        info = attach_metrics(cached,load_metrics(self.manifest,self.config))
        self.assertNotIn('commonTasks',info)
        for case in info['cases']:
            self.assertNotIn('quantiles',case)
            self.assertNotIn('storageUtilization',case)
            self.assertNotIn('storageThroughputGbps',case)
            self.assertEqual(case['metrics']['fct_p999_ms'],139.028128)
        self.assertIn('quantiles',cached['cases'][0])

    def test_explicit_mapping_and_override(self):
        self.write_summary()
        self.config['metrics_csv'] = 'absent.csv'
        self.config['cases'] = [dict(id='custom-case',metrics_scheme='te')]
        metrics = load_metrics(self.manifest,self.config,self.root/'summary.csv')
        self.assertEqual(metrics['cases']['custom-case']['scheme'],'TE')

    def test_missing_or_duplicate_schemes_and_missing_p999_are_errors(self):
        self.write_summary(fields=[key for key in ['scheme',*COUNTS,*VALUES] if key!='fct_p999_ms'])
        with self.assertRaisesRegex(ValueError,'fct_p999_ms'):
            load_metrics(self.manifest,self.config)
        self.rows.append(dict(self.rows[0]))
        self.write_summary()
        with self.assertRaisesRegex(ValueError,'duplicate scheme'):
            load_metrics(self.manifest,self.config)
        self.rows = self.rows[:1]
        self.write_summary()
        with self.assertRaisesRegex(ValueError,'no metrics row'):
            load_metrics(self.manifest,self.config)

    def test_missing_values_and_zero_sample_counts_are_not_displayed_as_zero(self):
        self.rows[0].update(fct_p999_ms='',avg_tps='NaN',common_valid_throughput_requests=0)
        self.write_summary()
        values = load_metrics(self.manifest,self.config)['cases']['ecmp']
        self.assertIsNone(values['fct_p999_ms'])
        self.assertIsNone(values['avg_tps'])
        self.assertIsNone(values['avg_throughput_gbps'])

    def test_invalid_values_and_obsolete_statistics_filters_are_rejected(self):
        for invalid in ('-1','inf','not-a-number'):
            self.rows[0]['fct_p999_ms'] = invalid
            self.write_summary()
            with self.assertRaisesRegex(ValueError,'fct_p999_ms'):
                load_metrics(self.manifest,self.config)
        self.config['statistics'] = {'task_ids':[1]}
        with self.assertRaisesRegex(ValueError,'filters no longer apply'):
            load_metrics(self.manifest,self.config)


if __name__=='__main__':
    unittest.main()
