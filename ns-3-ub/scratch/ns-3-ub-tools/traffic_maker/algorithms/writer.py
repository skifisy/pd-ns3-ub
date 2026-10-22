from __future__ import annotations

import csv
from pathlib import Path
from typing import Sequence

from .collective_ir import CollectiveSchedule
from .semantic_validator import validate_semantic_schedule


TRAFFIC_HEADER = [
    "taskId",
    "sourceNodeId",
    "destNodeId",
    "dataSize(Byte)",
    "opType",
    "priority",
    "delay",
    "phaseId",
    "dependOnPhases",
]


def write_schedule_csv(
    output_path: Path,
    rank_groups: Sequence[Sequence[int]],
    schedule: CollectiveSchedule,
    *,
    phase_delay: str = "0ns",
    include_inactive: bool = False,
    include_local: bool = False,
    delay_first_phase: bool = False,
) -> Path:
    validate_semantic_schedule(schedule)
    phases = schedule.phases
    chunk_bytes = {chunk.chunk_id: chunk.byte_length for chunk in schedule.chunks}
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.writer(output_file)
        writer.writerow(TRAFFIC_HEADER)
        task_id = 0
        phase_id = 0
        for node_ids_by_rank in rank_groups:
            local_tasks = []
            if include_local:
                for rank in range(schedule.contract.rank_size):
                    local_chunks = (
                        schedule.contract.initial_holdings[rank]
                        & schedule.contract.final_holdings[rank]
                    )
                    local_bytes = sum(chunk_bytes[chunk_id] for chunk_id in local_chunks)
                    if local_bytes > 0:
                        local_tasks.append((rank, local_bytes))
            if local_tasks:
                for rank, local_bytes in local_tasks:
                    writer.writerow(
                        [
                            task_id,
                            node_ids_by_rank[rank],
                            node_ids_by_rank[rank],
                            local_bytes,
                            "URMA_WRITE",
                            7,
                            phase_delay if delay_first_phase else "0ns",
                            phase_id,
                            "",
                        ]
                    )
                    task_id += 1
                phase_id += 1
            for group_phase_id, phase in enumerate(phases):
                dependency = (
                    ""
                    if group_phase_id == 0 and not local_tasks
                    else str(phase_id - 1)
                )
                delay = phase_delay if dependency or delay_first_phase else "0ns"
                for transfer in phase.transfers:
                    writer.writerow(
                        [
                            task_id,
                            node_ids_by_rank[transfer.sender],
                            node_ids_by_rank[transfer.receiver],
                            transfer.byte_length,
                            "URMA_WRITE",
                            7,
                            delay,
                            phase_id,
                            dependency,
                        ]
                    )
                    task_id += 1
                phase_id += 1
    return output_path
