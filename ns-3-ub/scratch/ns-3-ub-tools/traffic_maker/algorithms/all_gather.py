from __future__ import annotations

from .all_reduce.nhr import nhr_get_step_num
from .collective_ir import (
    AtomicPhase,
    Chunk,
    CollectiveContract,
    CollectiveSchedule,
    Transfer,
)


def _validate_contract(contract: CollectiveContract) -> None:
    if contract.collective != "allgather":
        raise ValueError("AllGather requires an allgather contract")
    if contract.rank_size <= 0:
        raise ValueError("AllGather requires rank_size > 0")
    if contract.input_bytes_per_rank < 0:
        raise ValueError("AllGather requires input_bytes_per_rank >= 0")


def _allgather_chunks(contract: CollectiveContract) -> tuple[Chunk, ...]:
    if contract.input_bytes_per_rank == 0:
        return ()
    return tuple(
        Chunk(f"chunk-{rank}", contract.input_bytes_per_rank)
        for rank in range(contract.rank_size)
    )


def _copy_transfer(
    contract: CollectiveContract,
    sender: int,
    receiver: int,
    origin_ranks: tuple[int, ...],
    *,
    source_retains: bool = True,
) -> Transfer | None:
    if contract.input_bytes_per_rank == 0:
        return None
    return Transfer(
        sender=sender,
        receiver=receiver,
        chunk_ids=tuple(f"chunk-{rank}" for rank in origin_ranks),
        byte_length=len(origin_ranks) * contract.input_bytes_per_rank,
        source_retains=source_retains,
    )


def build_ag_ring_schedule(contract: CollectiveContract) -> CollectiveSchedule:
    """Build a unidirectional ring AllGather from an independent contract."""
    _validate_contract(contract)
    chunks = _allgather_chunks(contract)
    phases = []
    for step in range(contract.rank_size - 1):
        transfers = []
        for rank in range(contract.rank_size):
            chunk_id = f"chunk-{(rank - step) % contract.rank_size}"
            if contract.input_bytes_per_rank > 0:
                transfers.append(
                    Transfer(
                        sender=rank,
                        receiver=(rank + 1) % contract.rank_size,
                        chunk_ids=(chunk_id,),
                        byte_length=contract.input_bytes_per_rank,
                    )
                )
        phases.append(AtomicPhase(step, tuple(transfers), "AllGather"))
    return CollectiveSchedule(contract, "ring", chunks, tuple(phases))


def build_ag_nhr_schedule(contract: CollectiveContract) -> CollectiveSchedule:
    """Build HCCL's standalone NHR AllGather schedule."""
    _validate_contract(contract)
    rank_size = contract.rank_size
    n_steps = nhr_get_step_num(rank_size)
    phases = []

    for step in range(n_steps):
        exponent = n_steps - 1 - step
        delta_rank = 1 << exponent
        n_blocks = (rank_size - 1 + delta_rank) // (1 << (exponent + 1))
        delta_block_index = 1 << (exponent + 1)
        transfers = []

        for rank in range(rank_size):
            send_to = (rank + delta_rank) % rank_size
            tx_index = rank
            origin_ranks = []
            for _ in range(n_blocks):
                origin_ranks.append(tx_index)
                tx_index = (tx_index - delta_block_index + rank_size) % rank_size
            transfer = _copy_transfer(
                contract, rank, send_to, tuple(origin_ranks)
            )
            if transfer is not None:
                transfers.append(transfer)

        phases.append(AtomicPhase(step, tuple(transfers), "AllGather"))

    return CollectiveSchedule(
        contract, "nhr", _allgather_chunks(contract), tuple(phases)
    )


def build_ag_recursive_doubling_schedule(
    contract: CollectiveContract,
) -> CollectiveSchedule:
    """Build binary Recursive Exchange with FoldIn/FoldOut for arbitrary ranks."""
    _validate_contract(contract)
    rank_size = contract.rank_size
    active_size = 1 << (rank_size.bit_length() - 1)
    remainder = rank_size - active_size
    phases = []
    holdings = [{rank} for rank in range(rank_size)]

    if remainder:
        transfers = []
        for rank in range(0, 2 * remainder, 2):
            transfer = _copy_transfer(
                contract, rank, rank + 1, (rank,), source_retains=False
            )
            if transfer is not None:
                transfers.append(transfer)
            holdings[rank + 1].add(rank)
            holdings[rank].clear()
        phases.append(AtomicPhase(len(phases), tuple(transfers), "FoldIn"))

    active_ranks = tuple(
        2 * active_rank + 1 if active_rank < remainder else active_rank + remainder
        for active_rank in range(active_size)
    )
    distance = 1
    while distance < active_size:
        phase_start = [set(rank_holdings) for rank_holdings in holdings]
        transfers = []
        for active_rank, rank in enumerate(active_ranks):
            peer = active_ranks[active_rank ^ distance]
            transfer = _copy_transfer(
                contract, rank, peer, tuple(sorted(phase_start[rank]))
            )
            if transfer is not None:
                transfers.append(transfer)
        for transfer in transfers:
            holdings[transfer.receiver].update(phase_start[transfer.sender])
        phases.append(AtomicPhase(len(phases), tuple(transfers), "AllGather"))
        distance <<= 1

    if remainder:
        transfers = []
        for rank in range(0, 2 * remainder, 2):
            origin_ranks = tuple(sorted(holdings[rank + 1]))
            transfer = _copy_transfer(contract, rank + 1, rank, origin_ranks)
            if transfer is not None:
                transfers.append(transfer)
        phases.append(AtomicPhase(len(phases), tuple(transfers), "FoldOut"))

    return CollectiveSchedule(
        contract,
        "recursive-doubling",
        _allgather_chunks(contract),
        tuple(phases),
    )
