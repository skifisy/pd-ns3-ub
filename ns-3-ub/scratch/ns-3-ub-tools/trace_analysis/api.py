"""Callable trace-analysis entry point shared by v2 and tests."""

from __future__ import annotations

from pathlib import Path

from trace_analysis.cal_throughput import process_files as write_throughput
from trace_analysis.task_statistics import process_files_to_csv


def parse_trace_case(
    case_path: str | Path,
    output_dir: str | Path,
    *,
    overwrite: bool = False,
) -> dict[str, str]:
    case = Path(case_path).expanduser().resolve()
    output = Path(output_dir).expanduser().resolve()
    runlog = case / "runlog"
    traffic = case / "traffic.csv"
    if not case.is_dir():
        raise ValueError(f"trace case directory does not exist: {case}")
    if not runlog.is_dir():
        raise ValueError(f"trace input directory does not exist: {runlog}")
    if not traffic.is_file():
        raise ValueError(f"trace input traffic.csv does not exist: {traffic}")
    output.mkdir(parents=True, exist_ok=True)
    task_output = output / "task_statistics.csv"
    throughput_output = output / "throughput.csv"
    existing = [path.name for path in (task_output, throughput_output) if path.exists()]
    if existing and not overwrite:
        raise ValueError(
            f"analysis output already exists: {', '.join(existing)}; use --overwrite to replace"
        )
    process_files_to_csv(str(runlog), str(traffic), str(task_output))
    write_throughput(str(runlog), str(throughput_output))
    return {
        "casePath": str(case),
        "runlogPath": str(runlog),
        "outputDir": str(output),
        "taskStatistics": str(task_output),
        "throughput": str(throughput_output),
    }
