#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from typing import Any

from .collective_ir import CollectiveContract, CollectiveSchedule, TransferOperation
from .registry import get_algorithm


class SemanticValidationError(RuntimeError):
    pass


def _compact_result(schedule: CollectiveSchedule) -> dict[str, Any]:
    transfers = [transfer for phase in schedule.phases for transfer in phase.transfers]
    return {
        "ok": True,
        "collective": schedule.contract.collective,
        "algorithm": schedule.algorithm,
        "rank_size": schedule.contract.rank_size,
        "phase_count": len(schedule.phases),
        "transfer_count": len(transfers),
        "wire_bytes": sum(transfer.byte_length for transfer in transfers),
    }


def _render_holdings(
    holdings: list[dict[str, frozenset[int]]], *, reduction: bool
) -> dict[str, Any]:
    return {
        str(rank): (
            {
                chunk_id: sorted(contributors)
                for chunk_id, contributors in sorted(rank_chunks.items())
            }
            if reduction
            else sorted(rank_chunks)
        )
        for rank, rank_chunks in enumerate(holdings)
    }


def _validate_contract_shape(
    contract: CollectiveContract,
    known_chunks: set[str],
    required: dict[str, frozenset[int]],
) -> None:
    collective = contract.collective
    supported = {"allgather", "reduce-scatter", "allreduce", "alltoall", "alltoallv"}
    if collective not in supported:
        raise SemanticValidationError(f"unsupported collective {collective!r}")
    initial_owners = {
        chunk_id: sum(chunk_id in holdings for holdings in contract.initial_holdings)
        for chunk_id in known_chunks
    }
    final_owners = {
        chunk_id: sum(chunk_id in holdings for holdings in contract.final_holdings)
        for chunk_id in known_chunks
    }
    rank_domain = frozenset(range(contract.rank_size))
    if collective == "allgather":
        valid = (
            not required
            and all(count == 1 for count in initial_owners.values())
            and all(count == contract.rank_size for count in final_owners.values())
        )
    elif collective == "reduce-scatter":
        valid = (
            set(required) == known_chunks
            and all(value == rank_domain for value in required.values())
            and all(count == contract.rank_size for count in initial_owners.values())
            and all(count == 1 for count in final_owners.values())
        )
    elif collective == "allreduce":
        valid = (
            set(required) == known_chunks
            and all(value == rank_domain for value in required.values())
            and all(count == contract.rank_size for count in initial_owners.values())
            and all(count == contract.rank_size for count in final_owners.values())
        )
    else:
        valid = (
            not required
            and all(count == 1 for count in initial_owners.values())
            and all(count == 1 for count in final_owners.values())
        )
    if not valid:
        raise SemanticValidationError(
            f"contract holdings/contributors do not match {collective} semantics"
        )


def validate_semantic_schedule(
    schedule: CollectiveSchedule, *, verbose: bool = False
) -> dict[str, Any]:
    """Validate ownership, bytes, contributors, and final collective state."""
    contract = schedule.contract
    if contract.rank_size <= 0:
        raise SemanticValidationError("rank_size must be positive")
    if len(contract.initial_holdings) != contract.rank_size:
        raise SemanticValidationError("contract initial holdings must cover every rank")
    if len(contract.final_holdings) != contract.rank_size:
        raise SemanticValidationError("contract final holdings must cover every rank")
    if contract.input_bytes_per_rank < 0:
        raise SemanticValidationError("input_bytes_per_rank must be non-negative")

    chunks = {chunk.chunk_id: chunk for chunk in schedule.chunks}
    if len(chunks) != len(schedule.chunks):
        raise SemanticValidationError("chunk IDs must be unique")
    if any(chunk.byte_length <= 0 for chunk in schedule.chunks):
        raise SemanticValidationError("schedule chunks must have positive byte lengths")
    known_chunks = set(chunks)

    expected_chunk_bytes = dict(contract.expected_chunk_bytes)
    if len(expected_chunk_bytes) != len(contract.expected_chunk_bytes):
        raise SemanticValidationError("expected chunk byte IDs must be unique")
    actual_chunk_bytes = {
        chunk_id: chunk.byte_length for chunk_id, chunk in chunks.items()
    }
    if expected_chunk_bytes != actual_chunk_bytes:
        raise SemanticValidationError(
            "schedule chunk bytes do not match the collective contract"
        )

    chunk_offsets = {
        chunk_id: (send_offset, receive_offset)
        for chunk_id, send_offset, receive_offset in contract.chunk_offsets
    }
    if len(chunk_offsets) != len(contract.chunk_offsets):
        raise SemanticValidationError("chunk offset IDs must be unique")
    if set(chunk_offsets) - known_chunks or any(
        send_offset < 0 or receive_offset < 0
        for send_offset, receive_offset in chunk_offsets.values()
    ):
        raise SemanticValidationError(
            "chunk offsets must reference known chunks and be non-negative"
        )

    declared = set().union(*contract.initial_holdings, *contract.final_holdings)
    if declared != known_chunks:
        missing = sorted(declared - known_chunks)
        extra = sorted(known_chunks - declared)
        raise SemanticValidationError(
            f"contract and schedule chunks differ; missing {missing}, extra {extra}"
        )

    expected_input_bytes = contract.input_bytes_by_rank
    if expected_input_bytes is None:
        expected_input_bytes = (contract.input_bytes_per_rank,) * contract.rank_size
    if len(expected_input_bytes) != contract.rank_size or any(
        byte_length < 0 for byte_length in expected_input_bytes
    ):
        raise SemanticValidationError(
            "contract input_bytes_by_rank must cover every rank with non-negative bytes"
        )
    for rank, rank_chunks in enumerate(contract.initial_holdings):
        initial_bytes = sum(chunks[chunk_id].byte_length for chunk_id in rank_chunks)
        if initial_bytes != expected_input_bytes[rank]:
            raise SemanticValidationError(
                f"rank {rank} initial chunk bytes {initial_bytes} do not match "
                f"the input contract {expected_input_bytes[rank]}"
            )

    required_contributors = dict(contract.required_contributors)
    if len(required_contributors) != len(contract.required_contributors):
        raise SemanticValidationError("required contributor chunk IDs must be unique")
    if required_contributors and set(required_contributors) != known_chunks:
        missing = sorted(known_chunks - set(required_contributors))
        unknown = sorted(set(required_contributors) - known_chunks)
        raise SemanticValidationError(
            f"reduction contributor table differs; missing {missing}, unknown {unknown}"
        )
    rank_domain = set(range(contract.rank_size))
    for chunk_id, contributors in required_contributors.items():
        if not contributors or not set(contributors) <= rank_domain:
            raise SemanticValidationError(
                f"chunk {chunk_id} has invalid required contributors"
            )
    _validate_contract_shape(contract, known_chunks, required_contributors)

    reduction = bool(required_contributors)
    holdings = [
        {
            chunk_id: frozenset((rank,)) if reduction else frozenset()
            for chunk_id in rank_chunks
        }
        for rank, rank_chunks in enumerate(contract.initial_holdings)
    ]
    phase_trace = []

    for expected_phase, phase in enumerate(schedule.phases):
        if phase.phase != expected_phase:
            raise SemanticValidationError(
                f"phase IDs must be contiguous; expected {expected_phase}, got {phase.phase}"
            )
        phase_start = [dict(rank_chunks) for rank_chunks in holdings]
        incoming: list[dict[str, frozenset[int]]] = [
            {} for _ in range(contract.rank_size)
        ]
        releases = [set() for _ in range(contract.rank_size)]
        transfer_trace = []

        for transfer in phase.transfers:
            if not 0 <= transfer.sender < contract.rank_size:
                raise SemanticValidationError(
                    f"phase {phase.phase}: sender {transfer.sender} outside rank domain"
                )
            if not 0 <= transfer.receiver < contract.rank_size:
                raise SemanticValidationError(
                    f"phase {phase.phase}: receiver {transfer.receiver} outside rank domain"
                )
            if transfer.sender == transfer.receiver:
                raise SemanticValidationError(
                    f"phase {phase.phase}: network transfer cannot be local"
                )
            if transfer.operation is TransferOperation.REDUCE and not reduction:
                raise SemanticValidationError(
                    f"phase {phase.phase}: contract does not define reduction contributors"
                )
            if not transfer.chunk_ids or len(set(transfer.chunk_ids)) != len(
                transfer.chunk_ids
            ):
                raise SemanticValidationError(
                    f"phase {phase.phase}: transfer must name unique chunks"
                )
            unknown = sorted(set(transfer.chunk_ids) - known_chunks)
            if unknown:
                raise SemanticValidationError(
                    f"phase {phase.phase}: transfer references unknown chunks {unknown}"
                )
            unowned = sorted(
                set(transfer.chunk_ids) - set(phase_start[transfer.sender])
            )
            if unowned:
                raise SemanticValidationError(
                    f"phase {phase.phase}: rank {transfer.sender} sends unowned chunks {unowned}"
                )
            expected_bytes = sum(
                chunks[chunk_id].byte_length for chunk_id in transfer.chunk_ids
            )
            if transfer.byte_length != expected_bytes:
                raise SemanticValidationError(
                    f"phase {phase.phase}: transfer byte length {transfer.byte_length} "
                    f"does not match chunk bytes {expected_bytes}"
                )

            for chunk_id in transfer.chunk_ids:
                payload = phase_start[transfer.sender][chunk_id]
                receiver_payload = phase_start[transfer.receiver].get(chunk_id)
                pending_payload = incoming[transfer.receiver].get(chunk_id)
                if pending_payload is not None:
                    raise SemanticValidationError(
                        f"phase {phase.phase}: rank {transfer.receiver} receives "
                        f"chunk {chunk_id} more than once"
                    )
                if transfer.operation is TransferOperation.COPY:
                    if receiver_payload is not None:
                        raise SemanticValidationError(
                            f"phase {phase.phase}: rank {transfer.receiver} receives "
                            f"duplicate chunk {chunk_id}"
                        )
                    incoming[transfer.receiver][chunk_id] = payload
                    continue
                if chunk_id not in required_contributors:
                    raise SemanticValidationError(
                        f"phase {phase.phase}: chunk {chunk_id} is not reducible"
                    )
                existing = receiver_payload or frozenset()
                duplicate = sorted(set(payload) & set(existing))
                if duplicate:
                    raise SemanticValidationError(
                        f"phase {phase.phase}: rank {transfer.receiver} would reduce "
                        f"duplicate contributors {duplicate} into chunk {chunk_id}"
                    )
                incoming[transfer.receiver][chunk_id] = existing | payload

            if not transfer.source_retains:
                duplicate_releases = releases[transfer.sender] & set(transfer.chunk_ids)
                if duplicate_releases:
                    raise SemanticValidationError(
                        f"phase {phase.phase}: rank {transfer.sender} releases chunks "
                        f"more than once {sorted(duplicate_releases)}"
                    )
                releases[transfer.sender].update(transfer.chunk_ids)
            transfer_trace.append(
                {
                    "sender": transfer.sender,
                    "receiver": transfer.receiver,
                    "chunks": list(transfer.chunk_ids),
                    "bytes": transfer.byte_length,
                    "operation": transfer.operation.value,
                    "source_retains": transfer.source_retains,
                }
            )

        for rank in range(contract.rank_size):
            for chunk_id in releases[rank]:
                holdings[rank].pop(chunk_id)
            for chunk_id, contributors in incoming[rank].items():
                holdings[rank][chunk_id] = contributors
        phase_trace.append(
            {
                "phase": phase.phase,
                "label": phase.label,
                "transfers": transfer_trace,
                "holdings_after": _render_holdings(holdings, reduction=reduction),
            }
        )

    for rank, expected in enumerate(contract.final_holdings):
        actual = set(holdings[rank])
        if actual != set(expected):
            missing = sorted(set(expected) - actual)
            unexpected = sorted(actual - set(expected))
            raise SemanticValidationError(
                f"{contract.collective} postcondition failed for rank {rank}; "
                f"missing chunks {missing}, unexpected chunks {unexpected}"
            )
        for chunk_id in expected:
            required = required_contributors.get(chunk_id)
            if required is not None and holdings[rank][chunk_id] != required:
                missing = sorted(set(required) - set(holdings[rank][chunk_id]))
                unexpected = sorted(set(holdings[rank][chunk_id]) - set(required))
                raise SemanticValidationError(
                    f"{contract.collective} postcondition failed for rank {rank}, "
                    f"chunk {chunk_id}; missing contributors {missing}, "
                    f"unexpected contributors {unexpected}"
                )

    result = _compact_result(schedule)
    if verbose:
        result.update(
            {
                "contract": {
                    "input_bytes_per_rank": contract.input_bytes_per_rank,
                    "input_bytes_by_rank": list(expected_input_bytes),
                    "initial_holdings": {
                        str(rank): sorted(rank_chunks)
                        for rank, rank_chunks in enumerate(contract.initial_holdings)
                    },
                    "final_holdings": {
                        str(rank): sorted(rank_chunks)
                        for rank, rank_chunks in enumerate(contract.final_holdings)
                    },
                    "required_contributors": {
                        chunk_id: sorted(contributors)
                        for chunk_id, contributors in contract.required_contributors
                    },
                },
                "chunks": [
                    {
                        "id": chunk.chunk_id,
                        "bytes": chunk.byte_length,
                        **(
                            {
                                "send_offset": chunk_offsets[chunk.chunk_id][0],
                                "receive_offset": chunk_offsets[chunk.chunk_id][1],
                            }
                            if chunk.chunk_id in chunk_offsets
                            else {}
                        ),
                    }
                    for chunk in schedule.chunks
                ],
                "phases": phase_trace,
                "final_holdings": _render_holdings(holdings, reduction=reduction),
                "postcondition": "contract final holdings and contributors satisfied",
            }
        )
    return result


def validate_collective_schedule(
    collective: str,
    algorithm: str,
    rank_size: int,
    data_size: int,
    *,
    verbose: bool = False,
) -> dict[str, Any]:
    if collective == "alltoallv":
        raise SemanticValidationError(
            "AllToAllV validation requires its byte matrix schedule"
        )
    try:
        schedule = get_algorithm(collective, algorithm).build(rank_size, data_size)
    except KeyError as error:
        raise SemanticValidationError(
            f"unsupported algorithm {collective}/{algorithm}"
        ) from error
    if not isinstance(schedule, CollectiveSchedule):
        raise SemanticValidationError("algorithm did not produce a semantic schedule")
    return validate_semantic_schedule(schedule, verbose=verbose)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate collective schedules by tracking chunks at every rank."
    )
    parser.add_argument(
        "collective", choices=("allgather", "reduce-scatter", "allreduce", "alltoall")
    )
    parser.add_argument("algorithm")
    parser.add_argument("--rank-size", type=int, required=True)
    parser.add_argument("--data-size", type=int, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = validate_collective_schedule(
        args.collective,
        args.algorithm,
        args.rank_size,
        args.data_size,
    )
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (SemanticValidationError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, indent=2))
        raise SystemExit(1)
