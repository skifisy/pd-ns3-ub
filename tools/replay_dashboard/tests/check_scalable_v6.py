"""Standalone task-import regression checks; run manually."""
import csv
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from task_store import create_tables, import_workload, import_tasks, completion_buckets, common_quantiles
from topology import csv_rows


class Reporter:
    def rows(self, path):
        return csv_rows(path)
    def lines(self, path):
        with open(path) as stream:
            yield from stream
    def stage(self, *args, **kwargs):
        pass
    def update(self, *args, **kwargs):
        pass


TASK_STATS_HEADER = [
    'taskId', 'sourceNode', 'destNode', 'dataSize(Byte)', 'opType',
    'taskStartTime(us)', 'taskCompletesTime(us)',
]


class DiskTaskChecks(unittest.TestCase):
    def test_cohort_quantiles_and_completion_boundaries(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            db = sqlite3.connect(root / 'tasks.sqlite')
            create_tables(db)
            with (root / 'traffic.csv').open('w', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerow(['taskId', 'sourceNode', 'destNode', 'dataSize(Byte)', 'opType'])
                writer.writerows([
                    (1, 0, 16, 100, 'URMA_WRITE'),
                    (2, 1, 17, 200, 'URMA_READ'),
                    (3, 2, 18, 100, 'URMA_WRITE'),
                    (4, 3, 19, 100, 'URMA_WRITE'),
                ])
            reporter = Reporter()
            import_workload(db, root / 'traffic.csv', reporter)
            cases = []
            for cid, ends in [
                ('ecmp', [500000000, 1100000000, None, None]),
                ('te', [250000000, 900000000, 950000000, None]),
                ('toe', [300000000, 1000000000, None, None]),
            ]:
                output = root / cid / 'output'
                output.mkdir(parents=True)
                with (output / 'task_statistics.csv').open('w', newline='') as stream:
                    writer = csv.writer(stream)
                    writer.writerow(TASK_STATS_HEADER)
                    for task_id, end_ns in enumerate(ends, 1):
                        start_us = 0 if task_id <= 3 else ''
                        end_us = '' if end_ns is None else int(end_ns / 1000)
                        size = 200 if task_id == 2 else 100
                        op = 'URMA_READ' if task_id == 2 else 'URMA_WRITE'
                        writer.writerow([task_id, task_id - 1, task_id + 15, size, op, start_us, end_us])
                info = import_tasks(db, root, dict(id=cid, path=cid), reporter)
                cases.append(dict(id=cid, mode='ecmp' if cid == 'ecmp' else cid, **info))
            n, _ = common_quantiles(db, cases, {}, reporter)
            self.assertEqual(n, 2)
            self.assertAlmostEqual(cases[0]['quantiles']['p50']['seconds'], .8)
            self.assertAlmostEqual(cases[1]['quantiles']['p99']['seconds'], .8935)
            bins = completion_buckets(db, 'ecmp', 500000000000, 1.25, reporter)
            self.assertEqual([b['time'] for b in bins], [0, .5, 1, 1.25])
            self.assertEqual([b['completed'] for b in bins], [0, 1, 1, 2])
            self.assertEqual(cases[0]['pendingTasks'], 1)
            self.assertEqual(cases[0]['taskSource'], 'statistics')
            self.assertNotIn('completedTimes', cases[0])
            db.close()

    def test_missing_statistics_never_falls_back_to_events_or_trace(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            (root / 'replay').mkdir()
            (root / 'runlog').mkdir()
            (root / 'replay' / 'replay_task_events.csv').write_text(
                'case_id,time_ns,event,task_kind,node_id,task_id\n'
                'ecmp,0,start,WQE,0,1\n'
                'ecmp,500000000,complete,WQE,0,1\n'
            )
            (root / 'runlog' / 'TaskTrace_node_0.tr').write_text(
                '[0.000000us] WQE Starts, jettyNum: 20 taskId: 1\n'
                '[500000.000000us] WQE Completes, jettyNum: 20 taskId: 1\n'
            )
            db = sqlite3.connect(root / 'tasks.sqlite')
            create_tables(db)
            db.execute("INSERT INTO workload VALUES(1,0,16,100,'URMA_WRITE')")
            with self.assertRaisesRegex(FileNotFoundError, 'output/task_statistics.csv'):
                import_tasks(db, root, dict(id='ecmp', path='replay'), Reporter())
            db.close()

    def test_csv_without_a_valid_completed_task_is_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            output = root / 'ecmp' / 'output'
            output.mkdir(parents=True)
            with (output / 'task_statistics.csv').open('w', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerow(TASK_STATS_HEADER)
                writer.writerow([1, 0, 16, 100, 'URMA_WRITE', 0, 'NaN'])
            db = sqlite3.connect(root / 'tasks.sqlite')
            create_tables(db)
            db.execute("INSERT INTO workload VALUES(1,0,16,100,'URMA_WRITE')")
            with self.assertRaisesRegex(ValueError, 'no usable completed tasks'):
                import_tasks(db, root, dict(id='ecmp', path='ecmp'), Reporter())
            db.close()


if __name__ == '__main__':
    unittest.main()
