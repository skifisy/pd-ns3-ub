#!/usr/bin/env bash
set -Eeuo pipefail

# ===== Paths =====
TRACE="/mnt/liujiaxin/scratch/mooncake_agent_v14_pipeline_tp_logical_dspread_10min/trace_10min.jsonl"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="${SCRIPT_DIR}/mooncake_pd_config_v15_glm51.json"
DAG_SCRIPT="${SCRIPT_DIR}/mooncake_trace_to_dag_v15.py"
TRAFFIC_SCRIPT="${SCRIPT_DIR}/mooncake_dag_to_traffic_v15.py"

OUT_DIR="/mnt/yuhaoze/pd-ns3-ub/ns-3-ub/scratch/mooncake_agent_v18_pipeline"
WORK_DIR="${OUT_DIR}/traffic_work"

# ===== Experiment parameters: retained from your original commands =====
WARMUP_REQUESTS=2400
PREFILL_MAX_BATCH=16
DECODE_MAX_BATCH=64
DAG_DETAIL="traffic"

TIMING_POLICY="max-phase"
PHASE_GROUPING="request-wave"
PRIORITY=7
STORAGE_READ_LATENCY_US=150
DATA_SIZE_DIVISOR=100000
TRAFFIC_NAME="traffic.csv"
TRAFFIC_SUMMARY="${OUT_DIR}/traffic_summary_v17.json"

fail() {
  echo "[ERROR] $*" >&2
  exit 1
}

command -v python3 >/dev/null 2>&1 || fail "python3 not found"
[[ -f "$TRACE" ]] || fail "Trace not found: $TRACE"
[[ -f "$CONFIG" ]] || fail "Latest GLM-5.1 config not found: $CONFIG"
[[ -f "$DAG_SCRIPT" ]] || fail "DAG script not found: $DAG_SCRIPT"
[[ -f "$TRAFFIC_SCRIPT" ]] || fail "Traffic script not found: $TRAFFIC_SCRIPT"

mkdir -p "$OUT_DIR" "$WORK_DIR"

# Validate Prefill FLOPs and fixed Decode timing independently.
python3 - "$CONFIG" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    cfg = json.load(f)

cm = cfg.get("compute_model", {})
if cm.get("mode") != "glm51_flops":
    raise SystemExit(
        "ERROR: compute_model.mode is not 'glm51_flops' in the selected config"
    )

print("[INFO] Using config:", sys.argv[1])
print("[INFO] Prefill compute model:", cm.get("mode"))
if cm.get("decode_mode", "legacy") != "legacy":
    raise SystemExit("ERROR: this experiment requires compute_model.decode_mode=legacy")
print("[INFO] Decode timing (us):", cfg["timing"]["first_decode_step_us"],
      cfg["timing"]["decode_us_per_token"], "(folded into network delays; no Decode TIMERs)")
print("[INFO] NPU:", cm.get("npu_model"), cm.get("npu_precision"))
print("[INFO] Peak ops/s:", cm.get("npu_peak_ops_per_sec"))
print("[INFO] Efficiency:", cm.get("efficiency"))
print("[INFO] Indexer Key Cache resident:", cm.get("indexer_key_cache_resident"))
PY

echo
echo "========== Step 1/2: Trace -> DAG =========="
python3 -u "$DAG_SCRIPT" \
  "$TRACE" \
  --config "$CONFIG" \
  --warmup-requests "$WARMUP_REQUESTS" \
  --prefill-max-batch "$PREFILL_MAX_BATCH" \
  --decode-max-batch "$DECODE_MAX_BATCH" \
  --out-dir "$OUT_DIR" \
  --dag-detail "$DAG_DETAIL"

echo
echo "========== Step 2/2: DAG -> Traffic =========="
python3 -u "$TRAFFIC_SCRIPT" \
  "${OUT_DIR}/task_dag.csv" \
  --out-dir "$OUT_DIR" \
  --work-dir "$WORK_DIR" \
  --timing-policy "$TIMING_POLICY" \
  --phase-grouping "$PHASE_GROUPING" \
  --priority "$PRIORITY" \
  --storage-read-latency-us "$STORAGE_READ_LATENCY_US" \
  --data-size-divisor "$DATA_SIZE_DIVISOR" \
  --traffic-name "$TRAFFIC_NAME" \
  --summary-output "$TRAFFIC_SUMMARY"

echo
echo "========== Finished =========="
echo "Config:          ${CONFIG}"
echo "DAG:             ${OUT_DIR}/task_dag.csv"
echo "Prefill detail:  ${OUT_DIR}/prefill_layer_compute.csv"
echo "Traffic:         ${OUT_DIR}/${TRAFFIC_NAME}"
echo "Traffic summary: ${TRAFFIC_SUMMARY}"
