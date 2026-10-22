from __future__ import annotations

import csv
from dataclasses import dataclass
from pathlib import Path
from .algorithms.all_to_all.pairwisev import (
    build_random_byte_matrix,
    build_uniform_byte_matrix,
)
from .algorithms.registry import get_algorithm
from .algorithms.writer import write_schedule_csv


@dataclass(frozen=True)
class TrafficRequest:
    collective: str
    data_size: int | None
    algorithm: str
    output_dir: Path
    host_count: int | None = None
    group_size: int | None = None
    group_layout: str | None = None
    placement_file: Path | None = None
    peers_per_phase: int | None = None
    distribution: str | None = None
    traffic_matrix: Path | None = None
    seed: int | None = None
    include_local: bool = False
    phase_delay: str = "0ns"


def _read_placement(path: Path) -> list[list[int]]:
    try:
        placement_file = path.open(newline="", encoding="utf-8")
    except OSError as error:
        raise ValueError(f"cannot read placement-file {path}: {error}") from error
    with placement_file:
        reader = csv.DictReader(placement_file)
        if reader.fieldnames != ["groupId", "rankId", "nodeId"]:
            raise ValueError("placement-file header must be groupId,rankId,nodeId")
        groups: dict[int, dict[int, int]] = {}
        for line_number, row in enumerate(reader, start=2):
            try:
                group_id = int(row["groupId"])
                rank_id = int(row["rankId"])
                node_id = int(row["nodeId"])
            except (TypeError, ValueError) as error:
                raise ValueError(
                    f"placement-file line {line_number} must contain integer IDs"
                ) from error
            if min(group_id, rank_id, node_id) < 0:
                raise ValueError(f"placement-file line {line_number} IDs must be non-negative")
            group = groups.setdefault(group_id, {})
            if rank_id in group:
                raise ValueError(f"duplicate rank {rank_id} in placement group {group_id}")
            if node_id in group.values():
                raise ValueError(f"duplicate node {node_id} in placement group {group_id}")
            group[rank_id] = node_id

    if not groups:
        raise ValueError("placement-file must contain at least one rank")
    sizes = {len(group) for group in groups.values()}
    if len(sizes) != 1:
        raise ValueError("placement-file has unequal groups")
    rank_groups: list[list[int]] = []
    for group_id in sorted(groups):
        group = groups[group_id]
        expected_ranks = list(range(len(group)))
        if sorted(group) != expected_ranks:
            raise ValueError(
                f"placement-file group {group_id} has a missing rank; expected {expected_ranks}"
            )
        rank_groups.append([group[rank] for rank in expected_ranks])
    return rank_groups


def _resolve_rank_groups(request: TrafficRequest) -> list[list[int]]:
    if request.placement_file is not None:
        if request.group_layout is not None:
            raise ValueError("group-layout and placement-file are mutually exclusive")
        rank_groups = _read_placement(request.placement_file)
        placement_host_count = len({node_id for group in rank_groups for node_id in group})
        placement_group_size = len(rank_groups[0])
        if request.host_count is not None and request.host_count != placement_host_count:
            raise ValueError(
                f"host-count {request.host_count} does not match placement-file host count "
                f"{placement_host_count}"
            )
        if request.group_size is not None and request.group_size != placement_group_size:
            raise ValueError(
                f"group-size {request.group_size} does not match placement-file group size "
                f"{placement_group_size}"
            )
        return rank_groups

    if request.host_count is None:
        raise ValueError("host-count is required unless placement-file is provided")
    group_size = request.host_count if request.group_size is None else request.group_size
    if request.host_count % group_size != 0:
        raise ValueError(
            "host-count must be divisible by group-size; "
            f"choose a group-size that divides {request.host_count}"
        )
    group_count = request.host_count // group_size
    layout = "contiguous" if request.group_layout is None else request.group_layout
    if layout == "contiguous":
        return [
            list(range(group_start, group_start + group_size))
            for group_start in range(0, request.host_count, group_size)
        ]
    if layout == "interleaved":
        return [
            [group_id + rank * group_count for rank in range(group_size)]
            for group_id in range(group_count)
        ]
    raise ValueError("group-layout must be contiguous or interleaved")


def _read_byte_matrix(path: Path) -> list[list[int]]:
    try:
        matrix_file = path.open(newline="", encoding="utf-8")
    except OSError as error:
        raise ValueError(f"cannot read traffic-matrix {path}: {error}") from error
    matrix: list[list[int]] = []
    with matrix_file:
        for line_number, row in enumerate(csv.reader(matrix_file), start=1):
            if not row:
                raise ValueError(f"traffic-matrix line {line_number} must not be empty")
            try:
                values = [int(value) for value in row]
            except ValueError as error:
                raise ValueError(
                    f"traffic-matrix line {line_number} must contain integer byte counts"
                ) from error
            if any(value < 0 for value in values):
                raise ValueError(
                    f"traffic-matrix line {line_number} contains a negative byte count"
                )
            matrix.append(values)
    if not matrix:
        raise ValueError("traffic-matrix must contain at least one row")
    return matrix


def build_alltoallv_schedule(request: TrafficRequest, group_size: int):
    distribution = request.distribution
    if distribution not in {"matrix", "uniform", "random"}:
        raise ValueError("distribution must be matrix, uniform, or random")
    if distribution == "matrix":
        if request.traffic_matrix is None:
            raise ValueError("traffic-matrix is required with --distribution matrix")
        if request.data_size is not None:
            raise ValueError("data-size is not used with --distribution matrix")
        if request.seed is not None:
            raise ValueError("seed is only valid with --distribution random")
        matrix = _read_byte_matrix(request.traffic_matrix)
    else:
        if request.traffic_matrix is not None:
            raise ValueError("traffic-matrix is only valid with --distribution matrix")
        if request.data_size is None or request.data_size < 0:
            raise ValueError(f"data-size is required with --distribution {distribution}")
        if distribution == "uniform":
            if request.seed is not None:
                raise ValueError("seed is only valid with --distribution random")
            matrix = build_uniform_byte_matrix(group_size, request.data_size)
        else:
            if request.seed is None:
                raise ValueError("seed is required with --distribution random")
            matrix = build_random_byte_matrix(
                group_size,
                request.data_size,
                seed=request.seed,
            )
    if len(matrix) != group_size:
        raise ValueError(
            f"traffic-matrix must be {group_size} x {group_size} for this group-size"
        )
    return get_algorithm("alltoallv", request.algorithm).build_matrix(
        matrix
    )


def write_traffic(request: TrafficRequest) -> Path:
    if request.host_count is not None and request.host_count <= 0:
        raise ValueError("host-count must be positive")
    if request.group_size is not None and request.group_size <= 0:
        raise ValueError("group-size must be positive")
    zero_byte_collective = request.collective in {
        "allgather",
        "allreduce",
        "reduce-scatter",
        "alltoall",
        "alltoallv",
    }
    if request.data_size is not None and (
        request.data_size < 0
        or (request.data_size == 0 and not zero_byte_collective)
    ):
        raise ValueError("data-size must be positive")
    try:
        algorithm = get_algorithm(request.collective, request.algorithm)
    except KeyError as error:
        raise ValueError(
            f"unsupported algorithm for {request.collective}: {request.algorithm}"
        ) from error
    rank_groups = _resolve_rank_groups(request)
    group_size = len(rank_groups[0])
    if request.collective == "alltoallv":
        if request.peers_per_phase is not None:
            raise ValueError("peers-per-phase is only valid with pairwise AllToAll")
        schedule = build_alltoallv_schedule(request, group_size)
    elif request.collective == "alltoall" and request.algorithm == "pairwise":
        peers_per_phase = 1 if request.peers_per_phase is None else request.peers_per_phase
        if peers_per_phase < 1 or (
            group_size > 1 and peers_per_phase >= group_size
        ):
            raise ValueError("peers-per-phase must satisfy 1 <= K < group-size")
        schedule = algorithm.build(
            group_size, request.data_size, peers_per_phase=peers_per_phase
        )
    else:
        if request.peers_per_phase is not None:
            raise ValueError("peers-per-phase is only valid with pairwise AllToAll")
        if request.data_size is None:
            raise ValueError("data-size is required")
        schedule = algorithm.build(group_size, request.data_size)

    output_path = request.output_dir / "traffic.csv"
    if output_path.exists():
        raise ValueError(
            f"traffic.csv already exists at {output_path}; choose an empty output-dir"
        )
    return write_schedule_csv(
        output_path,
        rank_groups,
        schedule,
        phase_delay=request.phase_delay,
        include_local=request.include_local,
    )
