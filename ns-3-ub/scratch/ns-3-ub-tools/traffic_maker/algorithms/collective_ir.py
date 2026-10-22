from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class TransferOperation(str, Enum):
    COPY = "copy"
    REDUCE = "reduce"


@dataclass(frozen=True)
class Chunk:
    chunk_id: str
    byte_length: int


@dataclass(frozen=True)
class Transfer:
    sender: int
    receiver: int
    chunk_ids: tuple[str, ...]
    byte_length: int
    operation: TransferOperation = TransferOperation.COPY
    source_retains: bool = True


@dataclass(frozen=True)
class AtomicPhase:
    phase: int
    transfers: tuple[Transfer, ...]
    label: str = ""


@dataclass(frozen=True)
class CollectiveContract:
    collective: str
    rank_size: int
    input_bytes_per_rank: int
    initial_holdings: tuple[frozenset[str], ...]
    final_holdings: tuple[frozenset[str], ...]
    required_contributors: tuple[tuple[str, frozenset[int]], ...] = ()
    input_bytes_by_rank: tuple[int, ...] | None = None
    expected_chunk_bytes: tuple[tuple[str, int], ...] = ()
    chunk_offsets: tuple[tuple[str, int, int], ...] = ()


def split_evenly(total_bytes: int, part_count: int) -> tuple[int, ...]:
    """Split bytes deterministically, assigning remainder bytes to low indexes."""
    base, remainder = divmod(total_bytes, part_count)
    return tuple(base + (index < remainder) for index in range(part_count))


@dataclass(frozen=True)
class CollectiveSchedule:
    contract: CollectiveContract
    algorithm: str
    chunks: tuple[Chunk, ...]
    phases: tuple[AtomicPhase, ...]


def build_allgather_contract(
    rank_size: int, input_bytes_per_rank: int
) -> CollectiveContract:
    rank_size = int(rank_size)
    input_bytes_per_rank = int(input_bytes_per_rank)
    if rank_size <= 0:
        raise ValueError("rank_size must be > 0")
    if input_bytes_per_rank < 0:
        raise ValueError("input_bytes_per_rank must be >= 0")
    chunk_ids = (
        tuple(f"chunk-{rank}" for rank in range(rank_size))
        if input_bytes_per_rank > 0
        else ()
    )
    return CollectiveContract(
        collective="allgather",
        rank_size=rank_size,
        input_bytes_per_rank=input_bytes_per_rank,
        initial_holdings=tuple(
            frozenset((f"chunk-{rank}",)) if chunk_ids else frozenset()
            for rank in range(rank_size)
        ),
        final_holdings=tuple(frozenset(chunk_ids) for _ in range(rank_size)),
        expected_chunk_bytes=tuple(
            (chunk_id, input_bytes_per_rank) for chunk_id in chunk_ids
        ),
    )
