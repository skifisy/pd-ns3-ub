import unittest
from importlib import import_module
from pathlib import Path
from unittest.mock import MagicMock, call, patch

from topology_source import load_topology_source, topology_summary


ROOT = Path(__file__).resolve().parent


class UserTopologyBuilderTest(unittest.TestCase):
    def test_original_topology_scripts_are_structured_sources(self):
        expectations = {
            "user_topo_example.py": {
                "nodes": 8, "links": 8, "devices": 4, "switches": 4,
            },
            "user_topo_2layer_clos.py": {
                "nodes": 560, "links": 1024, "devices": 512, "switches": 48,
            },
            "user_topo_4x4_2DFM.py": {
                "nodes": 16, "links": 48, "devices": 16, "switches": 0,
            },
        }

        for script, expected in expectations.items():
            with self.subTest(script=script):
                graph = load_topology_source(ROOT / script)
                self.assertEqual(topology_summary(graph), expected)

    def test_direct_execution_keeps_the_original_csv_generation_steps(self):
        cases = (
            ("user_topo_example", call()),
            ("user_topo_2layer_clos", call()),
            ("user_topo_4x4_2DFM", call(print_groups=True)),
        )

        for module_name, expected_builder_call in cases:
            with self.subTest(module=module_name):
                module = import_module(module_name)
                graph = MagicMock()
                with patch.object(module, "build_topology", return_value=graph) as builder:
                    module.main()

                self.assertEqual(builder.call_args, expected_builder_call)
                self.assertEqual(
                    graph.method_calls,
                    [
                        call.build_graph_config(),
                        call.gen_compressed_route_table(
                            path_finding_algo=module.all_shortest_paths,
                            multiple_workers=4,
                        ),
                        call.write_config(include_transport=False),
                    ],
                )


if __name__ == "__main__":
    unittest.main()
