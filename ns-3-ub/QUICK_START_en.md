# Quick Start

**Language**: [English](QUICK_START_en.md) | [中文](QUICK_START.md)

> This project is built on ns-3.44 and verified on Linux and Windows WSL. For detailed platform support, installation steps, system requirements, and build options, see [ns-3.44 Documentation](https://www.nsnam.org/releases/ns-3-44/documentation/), [Installation Guide](https://www.nsnam.org/docs/release/3.44/installation/singlehtml/), and [ns-3.44 Source](https://gitlab.com/nsnam/ns-3-dev/-/tree/ns-3.44?ref_type=tags).

## Prerequisites

The core build depends on the following tools. Download can be via either Git or source archive (via a web browser, wget, or curl).

| Purpose       | Tool                         | Minimum version        |
| ------------- | ---------------------------- | ---------------------- |
| Download      | git (for Git download) <br/>or: tar and bunzip2 (Web)      | No minimum version     |
| Compiler      | g++<br/>or: clang++          | >= 10<br/>>= 11        |
| Configuration | python3                      | >= 3.8                 |
| Build system  | cmake<br/>plus one of make / ninja / Xcode | cmake >= 3.13<br/>No minimum version |

If using Conda/virtualenv, please ensure that the `python3` used later matches the interpreter used to install dependencies.

### Check versions quickly

From the command line, you can check the versions as follows:

| Tool    | Version check command |
| ------- | --------------------- |
| g++     | `g++ --version`       |
| clang++ | `clang++ --version`   |
| python3 | `python3 -V`          |
| cmake   | `cmake --version`     |

## Get the Code

```bash
# Clone the project
git clone https://gitcode.com/open-usim/ns-3-ub.git
cd ns-3-ub

# Initialize and update submodules (includes Python analysis tools)
git submodule update --init --recursive

# If the above command fails, you can clone manually:
# git clone https://gitcode.com/open-usim/ns-3-ub-tools.git scratch/ns-3-ub-tools

# Verify submodule status
git submodule status
```

To automatically trigger trace analysis after simulation completion, please configure the tool path in the corresponding use case's `network_attribute.txt`, for example:

```
global UB_PYTHON_SCRIPT_PATH "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"
```

## Python Tools & Dependencies

The project's Python toolset is located in `scratch/ns-3-ub-tools/` ([open-usim/ns-3-ub-tools](https://gitcode.com/open-usim/ns-3-ub-tools)):

- Topology/Visualization: `net_sim_builder.py`, `topo_plot.py`, `user_topo_*.py`
- Traffic Generation: `traffic_maker/*`
- Trace Analysis: `trace_analysis/parse_trace.py`

It is recommended to install dependencies using the `requirements.txt` in the project:

```bash
python3 -m pip install --user -r scratch/ns-3-ub-tools/requirements.txt
# You may encounter `externally-managed-environment` restrictions, in which case please try using a virtual environment
# Use domestic mirror sources to accelerate downloads
pip install -i https://pypi.tuna.tsinghua.edu.cn/simple -r scratch/ns-3-ub-tools/requirements.txt
# Or use conda:
conda install pandas matplotlib seaborn
# Or manually install the above dependencies
```

Note: Please install the required third-party packages via `requirements.txt` before running `trace_analysis/parse_trace.py`.

Tip: If you use an agent that supports repo-local skills, this repository includes OpenUSim skills under ` .codex/skills/ `. They check startup facts from this page and `README_en.md`, then help plan experiments, generate cases, run simulations, and analyze results through `openusim-welcome`, `openusim-plan-experiment`, `openusim-run-experiment`, and `openusim-analyze-results`. They cover smoke runs, old-case reproduction, one-off debugging, A/B comparisons, parameter sweeps, and controlled-variable studies. For comparisons, the plan records the baseline, compared configurations, fixed controls, prediction, and evidence source before cases are generated. After analysis confirms a root cause or a reusable judgment, `openusim-capture-insights` can write it as a knowledge card with user approval. These skills depend on the current `ns-3-ub` working tree and `scratch/ns-3-ub-tools/`, and are maintained with this repository.

## Build

```bash
# Known issue: the ns-3 launcher (./ns3) is incompatible with Python 3.14 (argparse); use python3.12 to run it.
# One build may use -j for speed; do not start multiple build/test tasks at once.
BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}

# Configure only modules required by UB simulations
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-werror -d release -G Ninja

# Build the UB simulation entry and its dependencies
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

If you need to develop or validate other ns-3 modules, use a full configuration without `--enable-modules`.

### (Optional) Enable Unison multi-threaded simulation

To enable Unison for ns-3 multi-threaded parallel simulation (MTP), add `--enable-mtp` during configuration (you may also enable examples):

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --enable-mtp --disable-werror -d release -G Ninja
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example

# Use --mtp-threads to enable multi-threading at runtime (must be >= 2)
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp --mtp-threads=8'
```

See [UNISON_README.md](UNISON_README.md) for additional MTP usage, behavioral boundaries, and diagnostic options.

## Run a Minimal Example

```bash
# If using Conda, ensure its bin is in PATH first (or activate environment first)
# export PATH=<your-conda-path>/bin:$PATH

# Install dependencies
python3 -m pip install --user -r scratch/ns-3-ub-tools/requirements.txt

# Run small example and trigger trace analysis
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'

# Verify output
ls scratch/2nodes_single-tp/output/
# Expected to contain: task_statistics.csv  throughput.csv
```

If you encounter `ModuleNotFoundError: No module named 'pandas'`, it means the runtime `python3` is inconsistent with the interpreter used to install dependencies; please check PATH, or use `python3 -m pip install --user ...` to install dependencies in the current interpreter.

## Examples under scratch

The following are the available use case directories and corresponding run commands currently provided in the repository:

- 2 nodes (single TP):
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'
  ```

- 2 nodes (multiple TP):
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_multiple-tp'
  ```

- 2 nodes (packet spray):
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_packet-spray'
  ```

- 2D FullMesh 4x4 (multipath All-to-All):
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2dfm4x4-multipath_a2a'
  # Enable multi-threading acceleration (requires --enable-mtp compilation)
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2dfm4x4-multipath_a2a --mtp-threads=8'
  ```

- 2D FullMesh 4x4 (hierarchical broadcast):
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2dfm4x4-hierarchical_broadcast'
  ```

- Clos (32 hosts / 4 leafs / 8 spines, pod2pod):
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/clos_32hosts-4leafs-8spines_pod2pod'
  # Multi-threading recommended for large topologies
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/clos_32hosts-4leafs-8spines_pod2pod --mtp-threads=16'
  ```

Note: Some large-scale use cases take a long time to run. Please use `--mtp-threads=8` to enable multi-threading (requires `--enable-mtp` compilation).

## Full Workflow Example

```bash
# Run complete example, including Python post-processing
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2dfm4x4-multipath_a2a'

# Expected output:
[01:23:37]:Run case: scratch/2dfm4x4-multipath_a2a
[01:23:37]:Set component attributes
[01:23:37]:Create node.
[01:23:37]:Start Client.
[01:23:37]:Simulator finished!
[01:23:37]:Start Parse Trace File.
All dependencies are satisfied, starting script execution...
Processing complete, results saved to scratch/2dfm4_4-multipath_a2a/output/task_statistics.csv
Processing complete, results saved to scratch/2dfm4_4-multipath_a2a/output/throughput.csv
[01:23:37]:Program finished.

# View generated result files
ls scratch/2dfm4x4-multipath_a2a/output/
# task_statistics.csv  throughput.csv
```

## Configuration Files

Each use case directory typically contains the following files (format can refer to existing examples):

- `network_attribute.txt` - Network global parameters (can configure `UB_PYTHON_SCRIPT_PATH` for automatic post-processing)
- `node.csv` - Node definitions
- `topology.csv` - Topology connections
- `routing_table.csv` - Routing table
- `transport_channel.csv` - Optional explicit RTP channels; omitted channels are reserved while traffic loads
- `traffic.csv` - Traffic definitions

For the `ub-quick-example` entry, configuration semantics, and commands, see [scratch/README.md](scratch/README.md).
---

## Related Documentation

| Document | Description |
|----------|-------------|
| [README_en.md](README_en.md) | Project overview: UB components, repo layout, and key concepts |
| [scratch/README.md](scratch/README.md) | Config-driven entry: configuration semantics, commands, case execution, and parameter reference |
| [open-usim/ns-3-ub-tools](https://gitcode.com/open-usim/ns-3-ub-tools) | Toolchain (submodule): case generation and trace post-processing/analysis |
