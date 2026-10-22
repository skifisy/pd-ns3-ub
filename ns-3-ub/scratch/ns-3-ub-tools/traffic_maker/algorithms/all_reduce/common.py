from __future__ import annotations

from ..collective_ir import (
    Chunk,
    CollectiveContract,
    Transfer,
    TransferOperation,
    split_evenly,
)


def chunk_id(result_rank: int) -> str:
    return f"chunk-{result_rank}"


def build_allreduce_contract(
    rank_size: int, input_bytes_per_rank: int
) -> CollectiveContract:
    rank_size = int(rank_size)
    input_bytes_per_rank = int(input_bytes_per_rank)
    if rank_size <= 0:
        raise ValueError("rank_size must be > 0")
    if input_bytes_per_rank < 0:
        raise ValueError("input_bytes_per_rank must be >= 0")
    chunk_sizes = split_evenly(input_bytes_per_rank, rank_size)
    chunk_ids = tuple(
        chunk_id(rank) for rank, size in enumerate(chunk_sizes) if size > 0
    )
    contributors = frozenset(range(rank_size))
    holdings = tuple(frozenset(chunk_ids) for _ in range(rank_size))
    return CollectiveContract(
        collective="allreduce",
        rank_size=rank_size,
        input_bytes_per_rank=input_bytes_per_rank,
        initial_holdings=holdings,
        final_holdings=holdings,
        required_contributors=tuple(
            (result_chunk, contributors) for result_chunk in chunk_ids
        ),
        expected_chunk_bytes=tuple(
            (chunk_id(rank), size)
            for rank, size in enumerate(chunk_sizes)
            if size > 0
        ),
    )


def contract_chunks(
    contract: CollectiveContract,
) -> tuple[tuple[Chunk, ...], dict[str, int]]:
    if contract.collective != "allreduce":
        raise ValueError("AllReduce requires an allreduce contract")
    if contract.rank_size <= 0:
        raise ValueError("rank_size must be > 0")
    if contract.input_bytes_per_rank < 0:
        raise ValueError("input_bytes_per_rank must be >= 0")
    sizes = split_evenly(contract.input_bytes_per_rank, contract.rank_size)
    chunks = tuple(
        Chunk(chunk_id(rank), size)
        for rank, size in enumerate(sizes)
        if size > 0
    )
    return chunks, {chunk.chunk_id: chunk.byte_length for chunk in chunks}


def transfer(
    sender: int,
    receiver: int,
    result_ranks: list[int] | tuple[int, ...],
    chunk_sizes: dict[str, int],
    *,
    operation: TransferOperation,
    source_retains: bool,
) -> Transfer | None:
    chunk_ids = tuple(
        result_chunk
        for result_rank in result_ranks
        if (result_chunk := chunk_id(result_rank)) in chunk_sizes
    )
    if not chunk_ids:
        return None
    return Transfer(
        sender=sender,
        receiver=receiver,
        chunk_ids=chunk_ids,
        byte_length=sum(chunk_sizes[result_chunk] for result_chunk in chunk_ids),
        operation=operation,
        source_retains=source_retains,
    )
