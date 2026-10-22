from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

from .all_reduce.nhr import build_nhr_schedule
from .all_reduce.common import build_allreduce_contract
from .all_reduce.recursive_hd import build_ar_rhd_schedule
from .all_reduce.ring import build_ar_ring_schedule
from .all_to_all.pairwise import build_a2a_pairwise_schedule
from .all_to_all.pairwise import build_a2a_pairwise_contract
from .all_to_all.pairwisev import build_a2a_pairwisev_from_byte_matrix
from .all_gather import (
    build_ag_nhr_schedule,
    build_ag_recursive_doubling_schedule,
    build_ag_ring_schedule,
)
from .collective_ir import build_allgather_contract
from .reduce_scatter import (
    build_reduce_scatter_contract,
    build_rs_nhr_schedule,
    build_rs_recursive_halving_schedule,
    build_rs_ring_schedule,
)
ScheduleBuilder = Callable[..., object]


@dataclass(frozen=True)
class AlgorithmSpec:
    collective: str
    name: str
    legacy_name: str | None
    builder: ScheduleBuilder
    legacy_options: tuple[tuple[str, object], ...] = ()
    contract_factory: Callable[[int, int], object] | None = None

    def build(self, rank_size: int, data_size: int, **options: object) -> object:
        if self.contract_factory is None:
            raise ValueError(f"{self.collective} requires matrix-defined input")
        return self.builder(self.contract_factory(rank_size, data_size), **options)

    def build_matrix(self, matrix: object, **options: object) -> object:
        if self.contract_factory is not None:
            raise ValueError(f"{self.collective} requires scalar data-size input")
        return self.builder(matrix, **options)


ALGORITHM_REGISTRY = (
    AlgorithmSpec(
        "allreduce", "ring", "ar_ring", build_ar_ring_schedule,
        contract_factory=build_allreduce_contract,
    ),
    AlgorithmSpec(
        "allreduce", "nhr", "ar_nhr", build_nhr_schedule,
        contract_factory=build_allreduce_contract,
    ),
    AlgorithmSpec(
        "allreduce",
        "recursive-halving-doubling",
        "ar_rhd",
        build_ar_rhd_schedule,
        contract_factory=build_allreduce_contract,
    ),
    AlgorithmSpec(
        "reduce-scatter",
        "ring",
        None,
        build_rs_ring_schedule,
        contract_factory=build_reduce_scatter_contract,
    ),
    AlgorithmSpec(
        "reduce-scatter",
        "nhr",
        None,
        build_rs_nhr_schedule,
        contract_factory=build_reduce_scatter_contract,
    ),
    AlgorithmSpec(
        "reduce-scatter",
        "recursive-halving",
        None,
        build_rs_recursive_halving_schedule,
        contract_factory=build_reduce_scatter_contract,
    ),
    AlgorithmSpec(
        "allgather",
        "ring",
        None,
        build_ag_ring_schedule,
        contract_factory=build_allgather_contract,
    ),
    AlgorithmSpec(
        "allgather",
        "nhr",
        None,
        build_ag_nhr_schedule,
        contract_factory=build_allgather_contract,
    ),
    AlgorithmSpec(
        "allgather",
        "recursive-doubling",
        None,
        build_ag_recursive_doubling_schedule,
        contract_factory=build_allgather_contract,
    ),
    AlgorithmSpec(
        "alltoall",
        "pairwise",
        "a2a_pairwise",
        build_a2a_pairwise_schedule,
        contract_factory=build_a2a_pairwise_contract,
    ),
    AlgorithmSpec(
        "alltoallv",
        "pairwise",
        None,
        build_a2a_pairwisev_from_byte_matrix,
    ),
)


def collective_names() -> tuple[str, ...]:
    return tuple(dict.fromkeys(entry.collective for entry in ALGORITHM_REGISTRY))


def algorithm_names(collective: str) -> tuple[str, ...]:
    return tuple(entry.name for entry in ALGORITHM_REGISTRY if entry.collective == collective)


def legacy_algorithm_names() -> tuple[str, ...]:
    return tuple(
        entry.legacy_name for entry in ALGORITHM_REGISTRY if entry.legacy_name is not None
    )


def get_algorithm(collective: str, name: str) -> AlgorithmSpec:
    for entry in ALGORITHM_REGISTRY:
        if entry.collective == collective and entry.name == name:
            return entry
    raise KeyError((collective, name))


def get_legacy_algorithm(name: str) -> AlgorithmSpec:
    for entry in ALGORITHM_REGISTRY:
        if entry.legacy_name == name:
            return entry
    raise KeyError(name)
