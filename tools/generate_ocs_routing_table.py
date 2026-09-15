#!/usr/bin/env python3
"""Compatibility entry point for the Jupiter OCS routing-table generator.

New code should use ``generate_routing_table.py``.  This file remains so the
existing topology generator, tests, and any external scripts importing
``generate_ocs_routing_table`` continue to work.
"""

import argparse
from pathlib import Path

from generate_routing_table import generate_routing_table


__all__ = ["generate_routing_table"]


def main() -> None:
    # Preserve the original two-positional-argument command line interface.
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("topology", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate_routing_table(args.topology, args.output)
    print(f"Generated routing table: {args.output.resolve()}")


if __name__ == "__main__":
    main()
