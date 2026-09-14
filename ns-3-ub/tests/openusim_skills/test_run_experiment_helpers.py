import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
HELPER_ROOT = REPO_ROOT / ".codex/skills/openusim-run-experiment/scripts"
if str(HELPER_ROOT) not in sys.path:
    sys.path.insert(0, str(HELPER_ROOT))

from openusim_run_experiment.case_checker import check_case_files
from openusim_run_experiment.network_attribute_writer import validate_overrides_against_catalog


class OpenUSimRunExperimentHelpersTest(unittest.TestCase):
    def make_catalog(self, *entries):
        return {"entries": list(entries)}

    def test_validate_overrides_rejects_unknown_default_keys(self):
        catalog = self.make_catalog(
            {"parameter_key": "ns3::UbPort::UbDataRate", "kind": "AddAttribute"},
            {"parameter_key": "UB_TRACE_ENABLE", "kind": "GlobalValue"},
        )

        with self.assertRaises(ValueError) as ctx:
            validate_overrides_against_catalog(
                catalog,
                explicit_overrides={"ns3::UbPort::MissingAttr": "1"},
            )

        self.assertIn("ns3::UbPort::MissingAttr", str(ctx.exception))

    def test_validate_overrides_rejects_unknown_global_keys_when_globals_are_enumerated(self):
        catalog = self.make_catalog(
            {"parameter_key": "ns3::UbPort::UbDataRate", "kind": "AddAttribute"},
            {"parameter_key": "UB_TRACE_ENABLE", "kind": "GlobalValue"},
        )

        with self.assertRaises(ValueError) as ctx:
            validate_overrides_against_catalog(
                catalog,
                explicit_overrides={"UB_NOT_REAL": "true"},
            )

        self.assertIn("UB_NOT_REAL", str(ctx.exception))

    def test_validate_overrides_allows_known_fallback_globals_without_runtime_global_catalog(self):
        catalog = self.make_catalog(
            {"parameter_key": "ns3::UbPort::UbDataRate", "kind": "AddAttribute"},
        )

        result = validate_overrides_against_catalog(
            catalog,
            explicit_overrides={"UB_TRACE_ENABLE": "true"},
        )

        self.assertEqual(result["unknown_keys"], [])
        self.assertIn("UB_TRACE_ENABLE", result["accepted_keys"])

    def test_check_case_files_allows_missing_transport_channel_for_on_demand_mode(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            case_dir = Path(tmpdir)
            for filename in (
                "network_attribute.txt",
                "node.csv",
                "topology.csv",
                "routing_table.csv",
                "traffic.csv",
            ):
                (case_dir / filename).write_text("stub\n", encoding="utf-8")

            result = check_case_files(case_dir, transport_channel_mode="on-demand")
            self.assertEqual(result["status"], "ok")

    def test_check_case_files_defaults_to_on_demand(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            case_dir = Path(tmpdir)
            for filename in (
                "network_attribute.txt",
                "node.csv",
                "topology.csv",
                "routing_table.csv",
                "traffic.csv",
            ):
                (case_dir / filename).write_text("stub\n", encoding="utf-8")

            result = check_case_files(case_dir)
            self.assertEqual(result["status"], "ok")

    def test_check_case_files_rejects_legacy_on_demand_spelling(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            case_dir = Path(tmpdir)

            with self.assertRaises(ValueError) as ctx:
                check_case_files(case_dir, transport_channel_mode="on_demand")

            self.assertIn("Unsupported transport_channel_mode", str(ctx.exception))
