from __future__ import annotations

from typing import IO

from ..collective_ir import (
    AtomicPhase,
    Chunk,
    CollectiveContract,
    CollectiveSchedule,
    Transfer,
    split_evenly,
)


def _chunk_id(source: int, destination: int) -> str:
    return f"src-{source}-dst-{destination}"


def _destination_block_sizes(total_bytes: int, rank_size: int) -> tuple[int, ...]:
    return split_evenly(total_bytes, rank_size)


def _expected_holdings(
    rank_size: int, destination_block_sizes: tuple[int, ...]
) -> tuple[tuple[frozenset[str], ...], tuple[frozenset[str], ...]]:
    initial_holdings = tuple(
        frozenset(
            _chunk_id(source, destination)
            for destination, byte_length in enumerate(destination_block_sizes)
            if byte_length > 0
        )
        for source in range(rank_size)
    )
    final_holdings = tuple(
        frozenset(
            _chunk_id(source, destination)
            for source in range(rank_size)
            if destination_block_sizes[destination] > 0
        )
        for destination in range(rank_size)
    )
    return initial_holdings, final_holdings


def build_a2a_pairwise_contract(
    rank_size: int, input_bytes_per_rank: int
) -> CollectiveContract:
    """Build the canonical uniform AllToAll contract for pairwise scheduling."""
    rank_size = int(rank_size)
    input_bytes_per_rank = int(input_bytes_per_rank)
    if rank_size <= 0:
        raise ValueError("rank_size must be > 0")
    if input_bytes_per_rank < 0:
        raise ValueError("input_bytes_per_rank must be >= 0")

    initial_holdings, final_holdings = _expected_holdings(
        rank_size, _destination_block_sizes(input_bytes_per_rank, rank_size)
    )
    return CollectiveContract(
        collective="alltoall",
        rank_size=rank_size,
        input_bytes_per_rank=input_bytes_per_rank,
        initial_holdings=initial_holdings,
        final_holdings=final_holdings,
        expected_chunk_bytes=tuple(
            (_chunk_id(source, destination), byte_length)
            for source in range(rank_size)
            for destination, byte_length in enumerate(
                _destination_block_sizes(input_bytes_per_rank, rank_size)
            )
            if byte_length > 0
        ),
    )


def _validate_contract(contract: CollectiveContract) -> tuple[int, tuple[int, ...]]:
    if contract.collective != "alltoall":
        raise ValueError("pairwise AllToAll requires an alltoall contract")
    if contract.rank_size <= 0:
        raise ValueError("contract rank_size must be > 0")
    if contract.input_bytes_per_rank < 0:
        raise ValueError("contract input_bytes_per_rank must be >= 0")

    rank_size = contract.rank_size
    destination_block_sizes = _destination_block_sizes(
        contract.input_bytes_per_rank, rank_size
    )
    initial_holdings, final_holdings = _expected_holdings(
        rank_size, destination_block_sizes
    )
    if contract.initial_holdings != initial_holdings:
        raise ValueError("pairwise AllToAll contract initial holdings do not match its uniform layout")
    if contract.final_holdings != final_holdings:
        raise ValueError("pairwise AllToAll contract final holdings do not match its uniform layout")
    return rank_size, destination_block_sizes


def _group_phases(
    atomic_phases: tuple[AtomicPhase, ...], peers_per_phase: int, label: str
) -> tuple[AtomicPhase, ...]:
    return tuple(
        AtomicPhase(
            phase=phase_id,
            transfers=tuple(
                transfer
                for atomic_phase in atomic_phases[start : start + peers_per_phase]
                for transfer in atomic_phase.transfers
            ),
            label=label,
        )
        for phase_id, start in enumerate(range(0, len(atomic_phases), peers_per_phase))
    )


def build_a2a_pairwise_schedule(
    contract: CollectiveContract, *, peers_per_phase: int = 1
) -> CollectiveSchedule:
    """Build uniform AllToAll as pairwise exchanges from an independent contract."""
    rank_size, destination_block_sizes = _validate_contract(contract)
    if peers_per_phase < 1 or (rank_size > 1 and peers_per_phase >= rank_size):
        raise ValueError("peers_per_phase must satisfy 1 <= K < rank_size")

    chunks = tuple(
        Chunk(_chunk_id(source, destination), byte_length)
        for source in range(rank_size)
        for destination, byte_length in enumerate(destination_block_sizes)
        if byte_length > 0
    )
    atomic_phases = []
    for distance in range(1, rank_size):
        transfers = []
        for source in range(rank_size):
            destination = (source + distance) % rank_size
            byte_length = destination_block_sizes[destination]
            if byte_length > 0:
                transfers.append(
                    Transfer(
                        sender=source,
                        receiver=destination,
                        chunk_ids=(_chunk_id(source, destination),),
                        byte_length=byte_length,
                        source_retains=False,
                    )
                )
        atomic_phases.append(
            AtomicPhase(distance - 1, tuple(transfers), "AllToAllPairwise")
        )

    return CollectiveSchedule(
        contract=contract,
        algorithm="pairwise",
        chunks=chunks,
        phases=_group_phases(tuple(atomic_phases), peers_per_phase, "AllToAllPairwise"),
    )


def pretty_print_a2a_pairwise_schedule(
    schedule: CollectiveSchedule, *, file: IO[str] | None = None
) -> None:
    output = file
    print(
        f"[{schedule.algorithm}] rank_size={schedule.contract.rank_size} "
        f"input_bytes_per_rank={schedule.contract.input_bytes_per_rank} "
        f"phases={len(schedule.phases)}",
        file=output,
    )
    for phase in schedule.phases:
        print(f"[phase {phase.phase}] {phase.label}", file=output)
        for transfer in phase.transfers:
            print(
                f"  r{transfer.sender}->r{transfer.receiver} "
                f"bytes={transfer.byte_length} chunks={list(transfer.chunk_ids)}",
                file=output,
            )
