from __future__ import annotations

from typing import IO

from ..collective_ir import (
    AtomicPhase,
    CollectiveContract,
    CollectiveSchedule,
    TransferOperation,
)
from .common import contract_chunks, transfer


def _largest_power_of_two(value: int) -> int:
    if value <= 0:
        raise ValueError("rank_size must be positive")
    return 1 << (value.bit_length() - 1)


def build_ar_rhd_schedule(contract: CollectiveContract) -> CollectiveSchedule:
    """Build complete FoldIn, halving, doubling, and FoldOut AllReduce."""
    chunks, chunk_sizes = contract_chunks(contract)
    rank_size = contract.rank_size
    active_size = _largest_power_of_two(rank_size)
    remainder = rank_size - active_size
    phases = []

    if remainder:
        transfers = []
        all_results = list(range(rank_size))
        for even_rank in range(0, 2 * remainder, 2):
            odd_rank = even_rank + 1
            item = transfer(
                even_rank,
                odd_rank,
                all_results,
                chunk_sizes,
                operation=TransferOperation.REDUCE,
                source_retains=False,
            )
            if item is not None:
                transfers.append(item)
        phases.append(AtomicPhase(len(phases), tuple(transfers), "FoldIn"))

    active_to_real = [2 * rank + 1 for rank in range(remainder)]
    active_to_real.extend(
        rank + remainder for rank in range(remainder, active_size)
    )
    target_groups = [
        [2 * rank, 2 * rank + 1] if rank < remainder else [rank + remainder]
        for rank in range(active_size)
    ]

    distance = active_size >> 1
    while distance:
        transfers = []
        for active_rank, rank in enumerate(active_to_real):
            peer_active = active_rank ^ distance
            peer = active_to_real[peer_active]
            start = peer_active & ~(distance - 1)
            result_ranks = [
                result_rank
                for group in target_groups[start : start + distance]
                for result_rank in group
            ]
            item = transfer(
                rank,
                peer,
                result_ranks,
                chunk_sizes,
                operation=TransferOperation.REDUCE,
                source_retains=False,
            )
            if item is not None:
                transfers.append(item)
        phases.append(AtomicPhase(len(phases), tuple(transfers), "ReduceScatter"))
        distance >>= 1

    distance = 1
    while distance < active_size:
        transfers = []
        for active_rank, rank in enumerate(active_to_real):
            peer = active_to_real[active_rank ^ distance]
            start = active_rank & ~(distance - 1)
            result_ranks = [
                result_rank
                for group in target_groups[start : start + distance]
                for result_rank in group
            ]
            item = transfer(
                rank,
                peer,
                result_ranks,
                chunk_sizes,
                operation=TransferOperation.COPY,
                source_retains=True,
            )
            if item is not None:
                transfers.append(item)
        phases.append(AtomicPhase(len(phases), tuple(transfers), "AllGather"))
        distance <<= 1

    if remainder:
        transfers = []
        all_results = list(range(rank_size))
        for even_rank in range(0, 2 * remainder, 2):
            odd_rank = even_rank + 1
            item = transfer(
                odd_rank,
                even_rank,
                all_results,
                chunk_sizes,
                operation=TransferOperation.COPY,
                source_retains=True,
            )
            if item is not None:
                transfers.append(item)
        phases.append(AtomicPhase(len(phases), tuple(transfers), "FoldOut"))

    return CollectiveSchedule(
        contract,
        "recursive-halving-doubling",
        chunks,
        tuple(phases),
    )


def pretty_print_ar_rhd_schedule(
    schedule: CollectiveSchedule, *, file: IO[str] | None = None
) -> None:
    print(schedule, file=file)
