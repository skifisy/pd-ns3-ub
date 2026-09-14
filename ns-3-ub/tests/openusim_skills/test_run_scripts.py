import tempfile
import unittest
from pathlib import Path
from unittest import mock

from openusim_run_experiment import case_checker, network_attribute_writer


class OpenUSimRunScriptsTest(unittest.TestCase):
    def fake_parameter_catalog(self):
        return {
            "source_root": "scratch/ub-quick-example",
            "extractor_version": "test",
            "entry_count": 4,
            "entries": [
                {
                    "parameter_key": "ns3::UbPort::UbDataRate",
                    "kind": "AddAttribute",
                    "value_type": "ns3::DataRateValue",
                    "default_value": "400000000000bps",
                    "module": "UbPort",
                    "category": "general",
                    "safety_sensitivity": "medium",
                    "tuning_stage": "guided",
                },
                {
                    "parameter_key": "UB_TRACE_ENABLE",
                    "kind": "GlobalValue",
                    "value_type": "runtime-unavailable",
                    "default_value": "false",
                    "module": "GlobalValue",
                    "category": "trace",
                    "safety_sensitivity": "low",
                    "tuning_stage": "instrumentation",
                },
                {
                    "parameter_key": "UB_QUEUE_TRACE_ENABLE",
                    "kind": "GlobalValue",
                    "value_type": "runtime-unavailable",
                    "default_value": "false",
                    "module": "GlobalValue",
                    "category": "trace",
                    "safety_sensitivity": "low",
                    "tuning_stage": "instrumentation",
                },
                {
                    "parameter_key": "UB_PYTHON_SCRIPT_PATH",
                    "kind": "GlobalValue",
                    "value_type": "runtime-unavailable",
                    "default_value": "unset",
                    "module": "GlobalValue",
                    "category": "trace",
                    "safety_sensitivity": "low",
                    "tuning_stage": "instrumentation",
                },
            ],
        }

    def test_writer_emits_full_snapshot_from_query_catalog(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            case_dir = Path(temp_dir) / "case"
            with mock.patch.object(
                network_attribute_writer,
                "load_or_build_parameter_catalog",
                return_value=(self.fake_parameter_catalog(), Path("/tmp/catalog.json")),
            ):
                result = network_attribute_writer.write_network_attributes(
                    case_dir,
                    explicit_overrides={"ns3::UbPort::UbDataRate": "800Gbps"},
                    observability_overrides={"UB_TRACE_ENABLE": "true"},
                )
            text = Path(result["path"]).read_text(encoding="utf-8")
            self.assertIn('default ns3::UbPort::UbDataRate "800Gbps"', text)
            self.assertIn('global UB_TRACE_ENABLE "true"', text)
            self.assertIn('global UB_PYTHON_SCRIPT_PATH "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"', text)
            self.assertEqual(result["resolution_mode"], "full-catalog-snapshot")
            self.assertGreaterEqual(result["resolved_entry_count"], 3)
            self.assertEqual(result["applied_overrides"]["ns3::UbPort::UbDataRate"], "800Gbps")

    def test_empty_parameter_catalog_cache_is_stale(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cache_path = Path(temp_dir) / "parameter-catalog.json"
            cache_path.write_text('{"entries": []}\n', encoding="utf-8")
            self.assertTrue(network_attribute_writer._cache_is_stale(cache_path))

    def test_parameter_catalog_query_rejects_empty_results(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            cache_path = Path(temp_dir) / "parameter-catalog.json"
            with (
                mock.patch.object(
                    network_attribute_writer,
                    "_catalog_cache_path",
                    return_value=cache_path,
                ),
                mock.patch.object(
                    network_attribute_writer,
                    "_runtime_attribute_entries",
                    return_value=[],
                ),
                mock.patch.object(
                    network_attribute_writer,
                    "_runtime_global_entries",
                    return_value=[],
                ),
            ):
                with self.assertRaisesRegex(RuntimeError, "returned no entries"):
                    network_attribute_writer.load_or_build_parameter_catalog()

    def test_observability_preset_returns_valid_tiers(self):
        for tier in ("minimal", "balanced", "detailed"):
            overrides = network_attribute_writer.observability_preset(tier)
            self.assertIsInstance(overrides, dict)
            self.assertEqual(overrides["UB_TRACE_ENABLE"], "true")
            self.assertEqual(overrides["UB_PARSE_TRACE_ENABLE"], "true")

    def test_runtime_global_query_failure_is_not_silenced(self):
        with mock.patch.object(
            network_attribute_writer,
            "_run_query",
            side_effect=RuntimeError("global query failed"),
        ):
            with self.assertRaisesRegex(RuntimeError, "global query failed"):
                network_attribute_writer._runtime_global_entries()

    def test_observability_preset_minimal_disables_heavy_traces(self):
        overrides = network_attribute_writer.observability_preset("minimal")
        self.assertEqual(overrides["UB_PACKET_TRACE_ENABLE"], "false")
        self.assertEqual(overrides["UB_PORT_TRACE_ENABLE"], "false")
        self.assertEqual(overrides["UB_QUEUE_TRACE_ENABLE"], "false")
        self.assertEqual(overrides["UB_RECORD_PKT_TRACE"], "false")

    def test_observability_preset_balanced_enables_packet_and_port(self):
        overrides = network_attribute_writer.observability_preset("balanced")
        self.assertEqual(overrides["UB_PACKET_TRACE_ENABLE"], "true")
        self.assertEqual(overrides["UB_PORT_TRACE_ENABLE"], "true")
        self.assertEqual(overrides["UB_QUEUE_TRACE_ENABLE"], "true")
        self.assertEqual(overrides["UB_RECORD_PKT_TRACE"], "false")

    def test_observability_preset_detailed_enables_all(self):
        overrides = network_attribute_writer.observability_preset("detailed")
        self.assertEqual(overrides["UB_QUEUE_TRACE_ENABLE"], "true")
        self.assertEqual(overrides["UB_RECORD_PKT_TRACE"], "true")

    def test_observability_preset_rejects_unknown_tier(self):
        with self.assertRaises(ValueError):
            network_attribute_writer.observability_preset("turbo")

    def test_observability_preset_returns_copy(self):
        a = network_attribute_writer.observability_preset("balanced")
        b = network_attribute_writer.observability_preset("balanced")
        a["UB_TRACE_ENABLE"] = "false"
        self.assertEqual(b["UB_TRACE_ENABLE"], "true")

    def test_checker_only_reports_missing_major_files(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            case_dir = Path(temp_dir) / "case"
            case_dir.mkdir()
            (case_dir / "network_attribute.txt").write_text("", encoding="utf-8")
            report = case_checker.check_case_files(case_dir, transport_channel_mode="on-demand")
            self.assertEqual(report["status"], "missing_files")
            self.assertIn("node.csv", report["missing_files"])
            self.assertNotIn("transport_channel.csv", report["missing_files"])
            self.assertEqual(report["next_action"], "stop")
