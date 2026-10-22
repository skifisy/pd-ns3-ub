# SimAI Flow Export to ns-3-ub Workflow

This workflow converts SimAI MockNCCL flow models into an ns-3-ub `traffic.csv`.

The conversion point is the SimAI MockNCCL `FlowModels` DAG, not the SimAI workload text file and not an ns-3 backend trace. SimAI first expands the workload into per-flow transfers, then `simai_flow_to_traffic.py` maps those flows to ns-3-ub tasks.

```text
SimAI workload/config
  -> MockNCCL FlowModels
  -> AS_FLOW_EXPORT JSONL
  -> simai_flow_to_traffic.py
  -> ns-3-ub traffic.csv
  -> ub-quick-example case run
```

## Files

- `traffic_maker/simai_flow_to_traffic.py`: converter from SimAI JSONL to ns-3-ub `traffic.csv`.
- `traffic_maker/SIMAI_FLOW_WORKFLOW.md`: this workflow.
- `trace_analysis/parse_trace.py`: post-processing entry used by ns-3-ub cases.

## Prerequisites

1. SimAI must be built with `AS_FLOW_EXPORT` support in `MockNcclGroup.cc`.
2. The ns-3-ub case must already contain the non-traffic files:
   - `node.csv`
   - `topology.csv`
   - `routing_table.csv`
   - `transport_channel.csv`
   - `network_attribute.txt`
3. If trace post-processing is enabled, `network_attribute.txt` must set `UB_PYTHON_SCRIPT_PATH` to the real `parse_trace.py` path.

For example, when running from the ns-3-ub repository root:

```text
global UB_PYTHON_SCRIPT_PATH "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"
```

If the case is run outside the repository root, use an absolute path instead.

## Step 1: Export SimAI FlowModels

Run SimAI with `AS_FLOW_EXPORT` set to a JSONL output path:

```bash
cd <SimAI repo>

AS_FLOW_EXPORT=/tmp/simai_flows.jsonl \
AS_SEND_LAT=3 \
AS_NVLS_ENABLE=0 \
./bin/SimAI_simulator \
  -t 1 \
  -w ./example/microAllReduce.txt \
  -n ./Spectrum-X_8g_8gps_400Gbps_H100 \
  -c runs/macos/SimAI-macos.conf
```

`AS_NVLS_ENABLE=0` is recommended for the direct host-to-host conversion path. NVLS flows may use NVSwitch pseudo endpoints, which cannot be represented directly as ns-3-ub host-to-host tasks.

Each JSONL record has schema `simai-flow-v1` and includes fields such as:

- `collective_id`
- `group_type`
- `op`
- `state`
- `layer_num`
- `channel_id`
- `flow_id`
- `src`
- `dst`
- `flow_size`
- `chunk_id`
- `chunk_count`
- `conn_type`
- `parent_flow_id`
- `child_flow_id`

## Step 2: Convert JSONL to traffic.csv

Run the converter from the ns-3-ub-tools directory:

```bash
cd <ns-3-ub repo>/scratch/ns-3-ub-tools

python3 traffic_maker/simai_flow_to_traffic.py \
  /tmp/simai_flows.jsonl \
  -o /tmp/simai-ub-case/traffic.csv \
  --host-count 8
```

The converter writes the standard ns-3-ub header:

```csv
taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases
```

Default mapping:

- `src` -> `sourceNode`
- `dst` -> `destNode`
- `flow_size` -> `dataSize(Byte)`
- `opType` -> `URMA_WRITE`
- `priority` -> `7`
- `delay` -> `0ns`
- `parent_flow_id` DAG -> `phaseId` and `dependOnPhases`

## Rank Mapping

By default, SimAI rank IDs are used directly as ns-3-ub host node IDs. If the SimAI rank order differs from the ns-3-ub host IDs, provide a rank map:

```csv
simai_rank,ub_node
0,4
1,5
2,6
3,7
4,0
5,1
6,2
7,3
```

Then run:

```bash
python3 traffic_maker/simai_flow_to_traffic.py \
  /tmp/simai_flows.jsonl \
  -o /tmp/simai-ub-case/traffic.csv \
  --rank-map /tmp/rank_map.csv \
  --host-count 8
```

Always pass `--host-count` when possible. It catches endpoints outside the ns-3-ub host ID range early, especially accidental NVLS/NVSwitch pseudo endpoints.

## Phase Mapping

SimAI flows are a DAG. ns-3-ub dependencies are phase-based.

Default mode:

```bash
--phase-mode level
```

This groups all flows at the same DAG level within a collective into one ns-3-ub phase. It keeps the output compact and preserves bulk ordering.

Precise mode:

```bash
--phase-mode flow
```

This gives every SimAI flow its own phase. It preserves dependencies more exactly, but produces many phases.

Successive collectives are serialized by default. To allow collectives to overlap:

```bash
--allow-overlap-collectives
```

## Step 3: Prepare an ns-3-ub Case

Place the converted `traffic.csv` next to the other case files:

```text
<case_dir>/
  node.csv
  topology.csv
  routing_table.csv
  transport_channel.csv
  traffic.csv
  network_attribute.txt
```

Make sure:

- all `sourceNode` and `destNode` values are host node IDs in `node.csv`;
- `transport_channel.csv` has the required host pairs and priorities;
- `network_attribute.txt` sets `UB_PYTHON_SCRIPT_PATH` if post-processing is enabled.

## Step 4: Run ns-3-ub

From the ns-3-ub repository root:

```bash
./ns3 run 'scratch/ub-quick-example --case-path=/tmp/simai-ub-case'
```

Or use an already built binary:

```bash
./build/scratch/ns3.44-ub-quick-example-debug --case-path=/tmp/simai-ub-case
```

Expected run signs:

- the log prints `[traffic] Load traffic.csv`;
- the log prints `Scheduled local tasks: <N>`;
- the run ends with `[run] Simulation finished`;
- post-processing writes `output/task_statistics.csv` and `output/throughput.csv` when traces are enabled.

## Verified Example

A verified 8-rank SimAI AllReduce export produced:

- `1792` JSONL flow records;
- `1792` ns-3-ub tasks;
- `28` ns-3-ub phases in `--phase-mode level`;
- `1174405120` total task bytes;
- `146800640` bytes from each source rank.

For the example workload with two AllReduce collectives of `16777216` and `67108864` bytes, the per-rank send volume matches:

```text
(16777216 + 67108864) * 2 * (8 - 1) / 8 = 146800640
```

## Limitations

- The converter maps each SimAI flow to one ns-3-ub task.
- The default operation type is `URMA_WRITE`; use `--op-type` only if the ns-3-ub case supports the chosen operation.
- NVLS/NVSwitch pseudo endpoints need an explicit host-level projection before they can be represented in `traffic.csv`.
- ns-3-ub `dependOnPhases` is phase-level. Use `--phase-mode flow` when per-flow dependency fidelity is more important than compact output.
