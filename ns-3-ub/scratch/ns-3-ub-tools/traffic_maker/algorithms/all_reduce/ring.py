from __future__ import annotations

from typing import IO

from ..collective_ir import (
    AtomicPhase,
    CollectiveContract,
    CollectiveSchedule,
    TransferOperation,
)
from .common import contract_chunks, transfer


def build_ar_ring_schedule(contract: CollectiveContract) -> CollectiveSchedule:
    """Build HCCL-style Ring ReduceScatter followed by Ring AllGather."""
    chunks, chunk_sizes = contract_chunks(contract)
    rank_size = contract.rank_size
    phases = []

    for step in range(rank_size - 1):
        transfers = []
        for rank in range(rank_size):
            result_rank = (rank - step - 1) % rank_size
            item = transfer(
                rank,
                (rank + 1) % rank_size,
                [result_rank],
                chunk_sizes,
                operation=TransferOperation.REDUCE,
                source_retains=False,
            )
            if item is not None:
                transfers.append(item)
        phases.append(AtomicPhase(len(phases), tuple(transfers), "ReduceScatter"))

    for step in range(rank_size - 1):
        transfers = []
        for rank in range(rank_size):
            result_rank = (rank - step) % rank_size
            item = transfer(
                rank,
                (rank + 1) % rank_size,
                [result_rank],
                chunk_sizes,
                operation=TransferOperation.COPY,
                source_retains=True,
            )
            if item is not None:
                transfers.append(item)
        phases.append(AtomicPhase(len(phases), tuple(transfers), "AllGather"))

    return CollectiveSchedule(contract, "ring", chunks, tuple(phases))


def pretty_print_ar_ring_schedule(
    schedule: CollectiveSchedule, *, file: IO[str] | None = None
) -> None:
    print(schedule, file=file)
