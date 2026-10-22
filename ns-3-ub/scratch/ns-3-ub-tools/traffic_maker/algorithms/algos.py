from typing import List, Tuple

from .collective_ir import CollectiveSchedule
from .registry import get_legacy_algorithm, legacy_algorithm_names

# (sender_rank, receiver_rank, byte_count)
LogicCommPair = Tuple[int, int, int]
# A schedule is a list of phases, each phase is a list of `LogicCommPair`s.
LogicCommPairsByPhase = List[List[LogicCommPair]]


AVAILABLE_ALGOS = list(legacy_algorithm_names())


def print_logic_comm_pairs_by_phase(logic_comm_pairs: LogicCommPairsByPhase, *, title: str | None = None) -> None:
    """Print per-phase (sender, receiver, bytes) pairs and traffic volume."""
    if title:
        print(title)

    total_bytes_all = 0
    for phase_id, phase_pairs in enumerate(logic_comm_pairs):
        phase_bytes = sum(int(p[2]) for p in phase_pairs)
        total_bytes_all += phase_bytes
        print(f"\n[phase {phase_id}] pairs={len(phase_pairs)} phase_bytes={phase_bytes}")
        for sender, receiver, trans_byte in phase_pairs:
            print(f"  ({sender} -> {receiver}, {int(trans_byte)} bytes)")

    print(f"\n[total] phases={len(logic_comm_pairs)} total_bytes={total_bytes_all}")


def schedule_to_logic_comm_pairs(
    schedule: CollectiveSchedule,
) -> LogicCommPairsByPhase:
    """Project a semantic schedule to legacy display tuples."""
    return [
        [
            (transfer.sender, transfer.receiver, transfer.byte_length)
            for transfer in phase.transfers
        ]
        for phase in schedule.phases
    ]


def generate_ar_ring_logic_comm_pairs(n_comm_size: int, n_byte: int) -> LogicCommPairsByPhase:
    """Ring AllReduce schedule (logical ranks) - delegates to ring.py."""
    schedule = get_legacy_algorithm("ar_ring").build(n_comm_size, n_byte)
    return schedule_to_logic_comm_pairs(schedule)


def generate_ar_RHD_logic_comm_pairs(n_comm_size: int, n_byte: int) -> LogicCommPairsByPhase:
    """Recursive halving/doubling (RHD) AllReduce - delegates to recursive_hd.py."""
    schedule = get_legacy_algorithm("ar_rhd").build(n_comm_size, n_byte)
    return schedule_to_logic_comm_pairs(schedule)


def generate_ar_NHR_logic_comm_pairs(n_comm_size: int, n_byte: int) -> LogicCommPairsByPhase:
    """NHR AllReduce (ReduceScatter + AllGather) - delegates to nhr.py."""
    schedule = get_legacy_algorithm("ar_nhr").build(n_comm_size, n_byte)
    return schedule_to_logic_comm_pairs(schedule)


def generate_a2a_pairwise_logic_comm_pairs(n_comm_size: int, n_byte: int) -> LogicCommPairsByPhase:
    """Pairwise All2All schedule (logical ranks)."""
    schedule = get_legacy_algorithm("a2a_pairwise").build(n_comm_size, n_byte)
    return schedule_to_logic_comm_pairs(schedule)


def generate_logic_comm_pairs(
    *, algo: str, comm_size: int, comm_bytes: int
) -> LogicCommPairsByPhase:
    """Unified entry: generate per-phase logical comm pairs for an algo."""
    if algo not in AVAILABLE_ALGOS:
        raise ValueError(f"Unsupported algo: {algo}. Supported: {AVAILABLE_ALGOS}")

    if algo == "ar_ring":
        return generate_ar_ring_logic_comm_pairs(comm_size, comm_bytes)
    if algo == "a2a_pairwise":
        return generate_a2a_pairwise_logic_comm_pairs(comm_size, comm_bytes)
    if algo == "ar_nhr":
        return generate_ar_NHR_logic_comm_pairs(comm_size, comm_bytes)
    if algo == "ar_rhd":
        return generate_ar_RHD_logic_comm_pairs(comm_size, comm_bytes)

    raise ValueError(f"Unsupported algo: {algo}")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Generate sample logic comm pairs from an algorithm")
    parser.add_argument("-a", "--algo", choices=AVAILABLE_ALGOS, default="ar_ring",
                        help="Algorithm to build sample comm pairs for")
    parser.add_argument("-n", "--comm-size", type=int, default=4,
                        help="Logical communicator size to simulate")
    parser.add_argument("-b", "--bytes", type=int, default=1024,
                        help="Total bytes per rank to distribute")
    args = parser.parse_args()

    pairs = generate_logic_comm_pairs(
        algo=args.algo,
        comm_size=int(args.comm_size),
        comm_bytes=int(args.bytes),
    )
    print_logic_comm_pairs_by_phase(pairs, title=f"Sample {args.algo} comm pairs")
