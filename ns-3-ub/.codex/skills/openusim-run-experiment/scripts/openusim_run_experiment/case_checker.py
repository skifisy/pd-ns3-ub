from __future__ import annotations

from pathlib import Path


BASE_REQUIRED_FILES = (
    "network_attribute.txt",
    "node.csv",
    "topology.csv",
    "routing_table.csv",
    "traffic.csv",
)

TRANSPORT_CHANNEL_MODE_ON_DEMAND = "on-demand"
TRANSPORT_CHANNEL_MODE_PRECOMPUTED = "precomputed"
TRANSPORT_CHANNEL_MODES = frozenset(
    {
        TRANSPORT_CHANNEL_MODE_ON_DEMAND,
        TRANSPORT_CHANNEL_MODE_PRECOMPUTED,
    }
)


def check_case_files(
    case_dir: Path, transport_channel_mode: str = TRANSPORT_CHANNEL_MODE_ON_DEMAND
) -> dict:
    if transport_channel_mode not in TRANSPORT_CHANNEL_MODES:
        raise ValueError(f"Unsupported transport_channel_mode: {transport_channel_mode}")

    case_dir = Path(case_dir)
    required_files = list(BASE_REQUIRED_FILES)
    if transport_channel_mode == TRANSPORT_CHANNEL_MODE_PRECOMPUTED:
        required_files.append("transport_channel.csv")

    missing_files = [name for name in required_files if not (case_dir / name).is_file()]
    if missing_files:
        return {
            "status": "missing_files",
            "missing_files": missing_files,
            "next_action": "stop",
        }

    return {
        "status": "ok",
        "missing_files": [],
        "next_action": "ready",
    }
