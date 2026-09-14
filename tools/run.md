生成task_dag的命令：
python3 /home/liujiaxin/pd-ns3-ub/tools/mooncake_trace_to_pd_store_dag_v6_layer_pipeline.py \
conversation_trace.jsonl \
--config /home/liujiaxin/pd-ns3-ub/tools/mooncake_pd_store_config_v6_layer_pipeline.json \
--warmup-requests 1200 \
--out-dir /mnt/liujiaxin/scratch/mooncake_pd_example_read_fix

截断dag的命令：
python3 - <<'PY'                                      
import pandas as pd
src = "task_dag.csv"
dst = "task_dag_4.csv"
df = pd.read_csv(src, dtype=str)
reqs = sorted(
    df["request_id"].astype(int).unique()
)[:4]  # 4对应最终1w+条traffic
df = df[
    df["request_id"].astype(int).isin(reqs)
]
df.to_csv(dst, index=False)
print("Selected requests:", len(reqs))
print("Request range:", reqs[0], "~", reqs[-1])
print("DAG rows:", len(df))
PY

dag转化为traffic的命令：
python3 /home/liujiaxin/pd-ns3-ub/tools/task_dag_to_ub_traffic_v6_layer_pipeline_with_mapping.py task_dag_4.csv --output traffic.csv --mapping-output mapping-4.csv

最后在/home/liujiaxin/pd-ns3-ub/ns-3-ub执行：
./ns3 run 'scratch/ub-quick-example /mnt/liujiaxin/scratch/mooncake_pd_example_read_fix --dependency-visibility-delay=1ns --mtp-threads=8'
