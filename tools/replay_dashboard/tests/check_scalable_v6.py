"""Standalone v6 checks. Run manually; not executed while preparing this delivery."""
import csv
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from task_store import create_tables,import_workload,import_tasks,completion_buckets,common_quantiles
from topology import csv_rows


class Reporter:
    def rows(self,path):return csv_rows(path)
    def lines(self,path):
        with open(path) as f:yield from f
    def stage(self,*args,**kwargs):pass
    def update(self,*args,**kwargs):pass


class DiskTaskChecks(unittest.TestCase):
    def test_cohort_quantiles_and_completion_boundaries(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);db=sqlite3.connect(root/'tasks.sqlite');create_tables(db)
            with (root/'traffic.csv').open('w',newline='') as f:
                w=csv.writer(f);w.writerow(['taskId','sourceNode','destNode','dataSize(Byte)','opType'])
                w.writerows([(1,0,16,100,'URMA_WRITE'),(2,1,17,200,'URMA_READ'),(3,2,18,100,'URMA_WRITE'),(4,3,19,100,'URMA_WRITE')])
            reporter=Reporter();import_workload(db,root/'traffic.csv',reporter)
            cases=[]
            for cid,ends in [('ecmp',[500000000,1100000000,None,None]),('te',[250000000,900000000,950000000,None]),('toe',[300000000,1000000000,None,None])]:
                (root/cid).mkdir()
                with (root/cid/'replay_task_events.csv').open('w',newline='') as f:
                    w=csv.writer(f);w.writerow(['case_id','time_ns','event','task_kind','node_id','task_id'])
                    for tid in range(1,4):w.writerow([cid,0,'start','wqe',tid-1,tid])
                    for tid,end in enumerate(ends,1):
                        if end is not None:w.writerow([cid,end,'complete','wqe',tid-1,tid])
                info=import_tasks(db,root,dict(id=cid,path=cid),reporter)
                cases.append(dict(id=cid,mode='ecmp' if cid=='ecmp' else cid,**info))
            n,_=common_quantiles(db,cases,{},reporter)
            self.assertEqual(n,2)
            self.assertAlmostEqual(cases[0]['quantiles']['p50']['seconds'],.8)
            self.assertAlmostEqual(cases[1]['quantiles']['p99']['seconds'],.8935)
            bins=completion_buckets(db,'ecmp',500000000000,1.25,reporter)
            self.assertEqual([b['time'] for b in bins],[0,.5,1,1.25])
            self.assertEqual([b['completed'] for b in bins],[0,1,1,2])
            self.assertEqual(cases[0]['pendingTasks'],1)
            # A summary contains no per-task time arrays.
            self.assertNotIn('completedTimes',cases[0])
            db.close()

    def test_native_task_trace(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);(root/'replay').mkdir();(root/'runlog').mkdir()
            (root/'runlog'/'TaskTrace_node_0.tr').write_text('[0.000000us] WQE Starts, jettyNum: 20 taskId: 1\n[500000.000000us] WQE Completes, jettyNum: 20 taskId: 1\n')
            db=sqlite3.connect(root/'tasks.sqlite');create_tables(db)
            db.execute("INSERT INTO workload VALUES(1,0,16,100,'URMA_WRITE')")
            info=import_tasks(db,root,dict(id='ecmp',path='replay'),Reporter())
            self.assertEqual(info['taskSource'],'native')
            self.assertEqual(db.execute('SELECT end_ps-start_ps FROM tasks').fetchone()[0],500000000000)
            db.close()


if __name__=='__main__':unittest.main()
