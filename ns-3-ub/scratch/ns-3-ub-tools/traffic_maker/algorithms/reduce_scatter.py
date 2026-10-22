from __future__ import annotations

import math

from .all_reduce.nhr import nhr_get_step_num
from .collective_ir import (
    AtomicPhase,
    Chunk,
    CollectiveContract,
    CollectiveSchedule,
    Transfer,
    TransferOperation,
    split_evenly,
)


def _validate_inputs(rank_size: int, input_bytes_per_rank: int) -> tuple[int, int]:
    rank_size = int(rank_size)
    input_bytes_per_rank = int(input_bytes_per_rank)
    if rank_size <= 0:
        raise ValueError("rank_size must be > 0")
    if input_bytes_per_rank < 0:
        raise ValueError("input_bytes_per_rank must be >= 0")
    return rank_size, input_bytes_per_rank


def _chunk_id(rank: int) -> str:
    return f"chunk-{rank}"


def build_reduce_scatter_contract(
    rank_size: int, input_bytes_per_rank: int
) -> CollectiveContract:
    """Build the algorithm-independent ReduceScatter correctness contract."""
    rank_size, input_bytes_per_rank = _validate_inputs(
        rank_size, input_bytes_per_rank
    )
    chunk_sizes = split_evenly(input_bytes_per_rank, rank_size)
    chunk_ids = tuple(
        _chunk_id(rank) for rank, size in enumerate(chunk_sizes) if size > 0
    )
    contributors = frozenset(range(rank_size))
    return CollectiveContract(
        collective="reduce-scatter",
        rank_size=rank_size,
        input_bytes_per_rank=input_bytes_per_rank,
        initial_holdings=tuple(frozenset(chunk_ids) for _ in range(rank_size)),
        final_holdings=tuple(
            frozenset((_chunk_id(rank),)) if chunk_sizes[rank] > 0 else frozenset()
            for rank in range(rank_size)
        ),
        required_contributors=tuple(
            (chunk_id, contributors) for chunk_id in chunk_ids
        ),
        expected_chunk_bytes=tuple(
            (_chunk_id(rank), size)
            for rank, size in enumerate(chunk_sizes)
            if size > 0
        ),
    )


def _contract_chunks(
    contract: CollectiveContract,
) -> tuple[tuple[Chunk, ...], dict[str, int]]:
    if contract.collective != "reduce-scatter":
        raise ValueError("ReduceScatter requires a reduce-scatter contract")
    rank_size, input_bytes_per_rank = _validate_inputs(
        contract.rank_size, contract.input_bytes_per_rank
    )
    chunk_sizes = split_evenly(input_bytes_per_rank, rank_size)
    chunks = tuple(
        Chunk(_chunk_id(rank), size)
        for rank, size in enumerate(chunk_sizes)
        if size > 0
    )
    return chunks, {chunk.chunk_id: chunk.byte_length for chunk in chunks}


def _transfer(
    sender: int,
    receiver: int,
    block_ranks: list[int],
    chunk_sizes: dict[str, int],
    *,
    operation: TransferOperation = TransferOperation.REDUCE,
) -> Transfer | None:
    chunk_ids = tuple(
        chunk_id
        for block in block_ranks
        if (chunk_id := _chunk_id(block)) in chunk_sizes
    )
    if not chunk_ids:
        return None
    return Transfer(
        sender=sender,
        receiver=receiver,
        chunk_ids=chunk_ids,
        byte_length=sum(chunk_sizes[chunk_id] for chunk_id in chunk_ids),
        operation=operation,
        source_retains=False,
    )


def build_rs_ring_schedule(contract: CollectiveContract) -> CollectiveSchedule:
    """Build a standalone unidirectional ring ReduceScatter schedule."""
    chunks, chunk_sizes = _contract_chunks(contract)
    phases = []
    for step in range(contract.rank_size - 1):
        transfers = []
        for rank in range(contract.rank_size):
            block = (rank - step - 1) % contract.rank_size
            transfer = _transfer(
                rank,
                (rank + 1) % contract.rank_size,
                [block],
                chunk_sizes,
            )
            if transfer is not None:
                transfers.append(transfer)
        phases.append(AtomicPhase(step, tuple(transfers), "ReduceScatter"))
    return CollectiveSchedule(contract, "ring", chunks, tuple(phases))


def build_rs_nhr_schedule(contract: CollectiveContract) -> CollectiveSchedule:
    """Build HCCL's standalone NHR ReduceScatter network schedule."""
    chunks, chunk_sizes = _contract_chunks(contract)
    n_steps = nhr_get_step_num(contract.rank_size)
    phases = []
    for step in range(n_steps):
        delta_rank = 1 << step
        n_blocks = (contract.rank_size - 1 + delta_rank) // (1 << (step + 1))
        delta_block_index = 1 << (step + 1)
        transfers = []
        for rank in range(contract.rank_size):
            send_to = (rank - delta_rank) % contract.rank_size
            block = send_to
            blocks = []
            for _ in range(n_blocks):
                blocks.append(block)
                block = (block - delta_block_index) % contract.rank_size
            transfer = _transfer(rank, send_to, blocks, chunk_sizes)
            if transfer is not None:
                transfers.append(transfer)
        phases.append(AtomicPhase(step, tuple(transfers), "ReduceScatter"))
    return CollectiveSchedule(contract, "nhr", chunks, tuple(phases))


def _largest_power_of_two(value: int) -> int:
    return 1 << int(math.floor(math.log2(value)))


def build_rs_recursive_halving_schedule(
    contract: CollectiveContract,
) -> CollectiveSchedule:
    """Build complete FoldIn/halving/FoldOut ReduceScatter for any rank count."""
    chunks, chunk_sizes = _contract_chunks(contract)
    rank_size = contract.rank_size
    power_of_two = _largest_power_of_two(rank_size)
    remainder = rank_size - power_of_two
    n_steps = int(math.log2(power_of_two))
    phases = []
    phase_id = 0

    if remainder:
        transfers = []
        all_blocks = list(range(rank_size))
        for rank in range(0, 2 * remainder, 2):
            transfer = _transfer(rank, rank + 1, all_blocks, chunk_sizes)
            if transfer is not None:
                transfers.append(transfer)
        phases.append(AtomicPhase(phase_id, tuple(transfers), "FoldIn"))
        phase_id += 1

    active_to_real = [2 * rank + 1 for rank in range(remainder)]
    active_to_real.extend(
        rank + remainder for rank in range(remainder, power_of_two)
    )
    target_groups = [
        [2 * rank, 2 * rank + 1] if rank < remainder else [rank + remainder]
        for rank in range(power_of_two)
    ]

    for step in range(n_steps):
        distance = 1 << (n_steps - step - 1)
        transfers = []
        for active_rank, rank in enumerate(active_to_real):
            peer_active = active_rank ^ distance
            peer = active_to_real[peer_active]
            tx_start = peer_active & ~(distance - 1)
            blocks = [
                block
                for group in target_groups[tx_start : tx_start + distance]
                for block in group
            ]
            transfer = _transfer(rank, peer, blocks, chunk_sizes)
            if transfer is not None:
                transfers.append(transfer)
        phases.append(AtomicPhase(phase_id, tuple(transfers), "ReduceScatter"))
        phase_id += 1

    if remainder:
        transfers = []
        for odd_rank in range(1, 2 * remainder, 2):
            even_rank = odd_rank - 1
            transfer = _transfer(
                odd_rank,
                even_rank,
                [even_rank],
                chunk_sizes,
                operation=TransferOperation.COPY,
            )
            if transfer is not None:
                transfers.append(transfer)
        phases.append(AtomicPhase(phase_id, tuple(transfers), "FoldOut"))

    return CollectiveSchedule(
        contract,
        "recursive-halving",
        chunks,
        tuple(phases),
    )
