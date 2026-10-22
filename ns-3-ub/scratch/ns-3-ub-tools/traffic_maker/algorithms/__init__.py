"""Algorithm schedule builders for ns-3-ub traffic generation.

This package is the canonical home for algorithm modeling code.
"""

from .algos import (  # noqa: F401
    AVAILABLE_ALGOS,
    generate_a2a_pairwise_logic_comm_pairs,
    generate_ar_NHR_logic_comm_pairs,
    generate_ar_RHD_logic_comm_pairs,
    generate_ar_ring_logic_comm_pairs,
    generate_logic_comm_pairs,
    print_logic_comm_pairs_by_phase,
)

from .collective_ir import (  # noqa: F401
    AtomicPhase,
    Chunk,
    CollectiveContract,
    CollectiveSchedule,
    Transfer,
    TransferOperation,
)

from .all_reduce.nhr import (  # noqa: F401
    HCCL_MIN_SLICE_ALIGN,
    build_nhr_schedule,
    dump_nhr_debug,
    hccl_calc_slices,
    nhr_get_rank_mapping,
    nhr_get_step_num,
    pretty_print_nhr_schedule,
)

from .all_reduce.ring import (  # noqa: F401
    build_ar_ring_schedule,
    pretty_print_ar_ring_schedule,
)

from .all_reduce.recursive_hd import (  # noqa: F401
    build_ar_rhd_schedule,
    pretty_print_ar_rhd_schedule,
)

from .all_to_all.pairwise import (  # noqa: F401
    build_a2a_pairwise_schedule,
    pretty_print_a2a_pairwise_schedule,
)

from .all_to_all.pairwisev import (  # noqa: F401
    build_a2a_pairwisev_schedule,
    build_a2av_pairwise_schedule,
    pretty_print_a2a_pairwisev_schedule,
    pretty_print_a2av_pairwise_schedule,
)
