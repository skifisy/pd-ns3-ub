from __future__ import annotations

from typing import IO, List

from ..collective_ir import (
    AtomicPhase,
    CollectiveContract,
    CollectiveSchedule,
    TransferOperation,
)
from .common import contract_chunks, transfer


HCCL_MIN_SLICE_ALIGN = 128


def round_up_with_divisor(value: int, divisor: int) -> int:
    if divisor <= 0:
        raise ValueError("divisor must be > 0")
    return ((value + divisor - 1) // divisor) * divisor


def hccl_calc_slices(
    total_bytes: int, rank_size: int, *, align: int = HCCL_MIN_SLICE_ALIGN
) -> List[int]:
    """Return the legacy HCCL-aligned slice sizes for source comparisons."""
    if rank_size <= 0:
        raise ValueError("rank_size must be > 0")
    if total_bytes < 0:
        raise ValueError("total_bytes must be >= 0")
    calculated = (total_bytes + rank_size - 1) // rank_size
    aligned = round_up_with_divisor(calculated, align) if total_bytes > 0 else 0
    residue = total_bytes
    sizes = []
    for _ in range(rank_size):
        size = min(aligned, residue)
        sizes.append(size)
        residue -= size
    return sizes


def nhr_get_step_num(rank_size: int) -> int:
    if rank_size <= 1:
        return 0
    return (rank_size - 1).bit_length()


def nhr_get_rank_mapping(
    rank_size: int, *, keep_order: bool = False, return_tree: bool = False
):
    """Preserve HCCL's legacy rank reorder helper for source fixtures."""
    tree = list(range(rank_size))
    if keep_order:
        return (tree, tree) if return_tree else tree
    temporary = [0] * rank_size
    steps = nhr_get_step_num(rank_size)
    length = rank_size

    for step in range(steps):
        slices = (rank_size - 1 + (1 << step)) // (1 << (step + 1))
        if slices <= 1:
            break
        odd_part = False
        part = 0
        while part * length < rank_size:
            start = part * length
            end = min(start + length, rank_size)
            for index in range(start, end):
                offset = index - start
                destination = (
                    start + offset // 2
                    if offset % 2 == 0
                    else start + (offset + length) // 2
                )
                temporary[destination] = tree[index]
            odd_part |= (end - start) % 2 == 1
            part += 1
        tree = temporary.copy()
        if odd_part:
            break
        length >>= 1
    mapping = [0] * rank_size
    for mapped, rank in enumerate(tree):
        mapping[rank] = mapped
    return (tree, mapping) if return_tree else mapping


def build_nhr_schedule(contract: CollectiveContract) -> CollectiveSchedule:
    """Build HCCL NHR ReduceScatter followed by NHR AllGather."""
    chunks, chunk_sizes = contract_chunks(contract)
    rank_size = contract.rank_size
    steps = nhr_get_step_num(rank_size)
    phases = []

    for step in range(steps):
        delta_rank = 1 << step
        count = (rank_size - 1 + delta_rank) // (1 << (step + 1))
        stride = 1 << (step + 1)
        transfers = []
        for rank in range(rank_size):
            peer = (rank - delta_rank) % rank_size
            block = peer
            result_ranks = []
            for _ in range(count):
                result_ranks.append(block)
                block = (block - stride) % rank_size
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

    for step in range(steps):
        exponent = steps - 1 - step
        delta_rank = 1 << exponent
        count = (rank_size - 1 + delta_rank) // (1 << (exponent + 1))
        stride = 1 << (exponent + 1)
        transfers = []
        for rank in range(rank_size):
            peer = (rank + delta_rank) % rank_size
            block = rank
            result_ranks = []
            for _ in range(count):
                result_ranks.append(block)
                block = (block - stride) % rank_size
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

    return CollectiveSchedule(contract, "nhr", chunks, tuple(phases))


def pretty_print_nhr_schedule(
    schedule: CollectiveSchedule,
    *,
    file: IO[str] | None = None,
    show_slicemap: bool = True,
    show_bytes: bool = True,
) -> None:
    del show_slicemap, show_bytes
    print(schedule, file=file)


def iter_nhr_step_details(contract: CollectiveContract):
    schedule = build_nhr_schedule(contract)
    for phase in schedule.phases:
        for item in phase.transfers:
            yield phase.phase, phase.label, item


def dump_nhr_debug(
    contract: CollectiveContract,
    *,
    print_slicemap: bool = False,
    print_steps: bool = False,
    file: IO[str] | None = None,
) -> None:
    if print_slicemap or print_steps:
        pretty_print_nhr_schedule(build_nhr_schedule(contract), file=file)
