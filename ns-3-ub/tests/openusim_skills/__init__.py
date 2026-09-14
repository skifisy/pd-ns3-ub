import sys
from pathlib import Path
import unittest


SCRIPT_ROOT = (
    Path(__file__).resolve().parents[2]
    / ".codex"
    / "skills"
    / "openusim-run-experiment"
    / "scripts"
)

if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))


def load_tests(loader, standard_tests, pattern):
    package_dir = Path(__file__).resolve().parent
    return loader.discover(str(package_dir), pattern or "test*.py")
