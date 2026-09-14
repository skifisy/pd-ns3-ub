from pathlib import Path


def repo_root() -> Path:
    """Walk up from this file to find the repo root (directory containing the ns3 launcher)."""
    current = Path(__file__).resolve().parent
    while current != current.parent:
        if (current / "ns3").is_file():
            return current
        current = current.parent
    raise FileNotFoundError("Cannot find repo root (no 'ns3' launcher found)")
