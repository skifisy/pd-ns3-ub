from __future__ import annotations

import random
from typing import IO, Sequence

from ..collective_ir import AtomicPhase, Chunk, CollectiveContract, CollectiveSchedule, Transfer


def _validate_matrix(name: str, matrix: Sequence[Sequence[int]], rank_size: int) -> None:
    if len(matrix) != rank_size:
        raise ValueError(f"{name} must have {rank_size} rows, got {len(matrix)}")
    for row_id, row in enumerate(matrix):
        if len(row) != rank_size:
            raise ValueError(f"{name}[{row_id}] must have {rank_size} cols, got {len(row)}")


def _normalize_byte_matrix(
    matrix: Sequence[Sequence[int]],
) -> tuple[tuple[int, ...], ...]:
    rank_size = len(matrix)
    if rank_size <= 0:
        raise ValueError("traffic_matrix must have at least one rank")
    _validate_matrix("traffic_matrix", matrix, rank_size)

    normalized = []
    for source, row in enumerate(matrix):
        normalized_row = []
        for destination, byte_length in enumerate(row):
            if (
                isinstance(byte_length, bool)
                or not isinstance(byte_length, int)
                or byte_length < 0
            ):
                raise ValueError(
                    f"traffic_matrix[{source}][{destination}] must be a non-negative integer"
                )
            normalized_row.append(byte_length)
        normalized.append(tuple(normalized_row))
    return tuple(normalized)


def _chunk_id(source: int, destination: int) -> str:
    return f"src-{source}-dst-{destination}"


def _expected_holdings(
    matrix: tuple[tuple[int, ...], ...]
) -> tuple[tuple[frozenset[str], ...], tuple[frozenset[str], ...]]:
    rank_size = len(matrix)
    initial_holdings = tuple(
        frozenset(
            _chunk_id(source, destination)
            for destination, byte_length in enumerate(matrix[source])
            if byte_length > 0
        )
        for source in range(rank_size)
    )
    final_holdings = tuple(
        frozenset(
            _chunk_id(source, destination)
            for source in range(rank_size)
            if matrix[source][destination] > 0
        )
        for destination in range(rank_size)
    )
    return initial_holdings, final_holdings


def _max_rank_volume(matrix: tuple[tuple[int, ...], ...]) -> int:
    rank_size = len(matrix)
    max_send = max(sum(row) for row in matrix)
    max_receive = max(
        sum(matrix[source][destination] for source in range(rank_size))
        for destination in range(rank_size)
    )
    return max(max_send, max_receive)


def build_a2a_pairwisev_contract(
    matrix: Sequence[Sequence[int]],
) -> CollectiveContract:
    """Build the ownership contract for a matrix-defined AllToAllV operation."""
    normalized = _normalize_byte_matrix(matrix)
    initial_holdings, final_holdings = _expected_holdings(normalized)
    return CollectiveContract(
        collective="alltoallv",
        rank_size=len(normalized),
        # The shared contract has one scalar only; retain the legacy schedule's max volume.
        input_bytes_per_rank=_max_rank_volume(normalized),
        initial_holdings=initial_holdings,
        final_holdings=final_holdings,
        input_bytes_by_rank=tuple(sum(row) for row in normalized),
        expected_chunk_bytes=tuple(
            (_chunk_id(source, destination), byte_length)
            for source, row in enumerate(normalized)
            for destination, byte_length in enumerate(row)
            if byte_length > 0
        ),
        chunk_offsets=_chunk_offsets(normalized),
    )


def _chunk_offsets(
    matrix: tuple[tuple[int, ...], ...]
) -> tuple[tuple[str, int, int], ...]:
    rank_size = len(matrix)
    send_offsets = []
    for row in matrix:
        offset = 0
        send_offsets.append([])
        for byte_length in row:
            send_offsets[-1].append(offset)
            offset += byte_length
    receive_offsets = [[0] * rank_size for _ in range(rank_size)]
    for destination in range(rank_size):
        offset = 0
        for source in range(rank_size):
            receive_offsets[destination][source] = offset
            offset += matrix[source][destination]
    return tuple(
        (
            _chunk_id(source, destination),
            send_offsets[source][destination],
            receive_offsets[destination][source],
        )
        for source in range(rank_size)
        for destination in range(rank_size)
        if matrix[source][destination] > 0
    )


def _validate_contract(
    contract: CollectiveContract, matrix: tuple[tuple[int, ...], ...]
) -> None:
    if contract.collective != "alltoallv":
        raise ValueError("pairwise AllToAllV requires an alltoallv contract")
    if contract.rank_size != len(matrix):
        raise ValueError("AllToAllV contract rank_size does not match traffic_matrix")

    initial_holdings, final_holdings = _expected_holdings(matrix)
    if contract.initial_holdings != initial_holdings:
        raise ValueError("AllToAllV contract initial holdings do not match traffic_matrix")
    if contract.final_holdings != final_holdings:
        raise ValueError("AllToAllV contract final holdings do not match traffic_matrix")
    if contract.input_bytes_per_rank != _max_rank_volume(matrix):
        raise ValueError("AllToAllV contract input_bytes_per_rank does not match traffic_matrix")
    if contract.input_bytes_by_rank != tuple(sum(row) for row in matrix):
        raise ValueError("AllToAllV contract input_bytes_by_rank does not match traffic_matrix")


def build_a2a_pairwisev_schedule(
    contract: CollectiveContract,
    matrix: Sequence[Sequence[int]],
) -> CollectiveSchedule:
    """Build matrix-defined AllToAllV pairwise phases from an ownership contract."""
    normalized = _normalize_byte_matrix(matrix)
    _validate_contract(contract, normalized)
    rank_size = contract.rank_size

    chunks = tuple(
        Chunk(_chunk_id(source, destination), byte_length)
        for source, row in enumerate(normalized)
        for destination, byte_length in enumerate(row)
        if byte_length > 0
    )
    phases = []
    for distance in range(1, rank_size):
        transfers = []
        for source in range(rank_size):
            destination = (source + distance) % rank_size
            byte_length = normalized[source][destination]
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
        phases.append(
            AtomicPhase(distance - 1, tuple(transfers), "AllToAllVPairwise")
        )

    return CollectiveSchedule(
        contract=contract,
        algorithm="pairwise",
        chunks=chunks,
        phases=tuple(phases),
    )


def build_a2a_pairwisev_from_byte_matrix(
    matrix: Sequence[Sequence[int]], **_lowering_options: object
) -> CollectiveSchedule:
    """Create a matrix contract and its pairwise AllToAllV schedule."""
    contract = build_a2a_pairwisev_contract(matrix)
    return build_a2a_pairwisev_schedule(contract, matrix)


def build_uniform_byte_matrix(
    rank_size: int, data_size: int, **_lowering_options: object
) -> list[list[int]]:
    if rank_size <= 0:
        raise ValueError("rank_size must be positive")
    if data_size < 0:
        raise ValueError("data_size must be non-negative")
    matrix = [[0] * rank_size for _ in range(rank_size)]
    for source in range(rank_size):
        peers = list(range(rank_size))
        base, remainder = divmod(data_size, len(peers))
        for index, destination in enumerate(peers):
            matrix[source][destination] = base + (1 if index < remainder else 0)
    return matrix


def build_random_byte_matrix(
    rank_size: int,
    data_size: int,
    *,
    seed: int,
    **_lowering_options: object,
) -> list[list[int]]:
    if rank_size <= 0:
        raise ValueError("rank_size must be positive")
    if data_size < 0:
        raise ValueError("data_size must be non-negative")

    rng = random.Random(seed)
    matrix = [[0] * rank_size for _ in range(rank_size)]
    for source in range(rank_size):
        peers = list(range(rank_size))
        if not peers:
            continue
        cuts = (
            sorted(rng.sample(range(data_size + len(peers) - 1), len(peers) - 1))
            if len(peers) > 1
            else []
        )
        bounds = [-1, *cuts, data_size + len(peers) - 1]
        parts = [
            bounds[index + 1] - bounds[index] - 1
            for index in range(len(peers))
        ]
        rng.shuffle(peers)
        for destination, byte_length in zip(peers, parts):
            matrix[source][destination] = byte_length
    return matrix


def pretty_print_a2a_pairwisev_schedule(
    schedule: CollectiveSchedule, *, file: IO[str] | None = None
) -> None:
    print(
        f"[{schedule.algorithm}] rank_size={schedule.contract.rank_size} "
        f"max_rank_volume={schedule.contract.input_bytes_per_rank} "
        f"phases={len(schedule.phases)}",
        file=file,
    )
    for phase in schedule.phases:
        print(f"[phase {phase.phase}] {phase.label}", file=file)
        for transfer in phase.transfers:
            print(
                f"  r{transfer.sender}->r{transfer.receiver} "
                f"bytes={transfer.byte_length} chunks={list(transfer.chunk_ids)}",
                file=file,
            )


build_a2av_pairwise_schedule = build_a2a_pairwisev_schedule
pretty_print_a2av_pairwise_schedule = pretty_print_a2a_pairwisev_schedule
