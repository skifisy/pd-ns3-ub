"""Port placement invariants independent of batch timing and task numbering."""

import copy
import importlib.util
import json
from pathlib import Path
import unittest


SCRIPTS = Path(__file__).resolve().parents[1] / "generate_trace"


def module(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / (name + ".py"))
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


TRACE = module("mooncake_trace_to_dag_v15")
TRAFFIC = module("mooncake_dag_to_traffic_v15")
CONFIG = json.loads((SCRIPTS / "mooncake_pd_config_v15_glm51.json").read_text())


class StoragePortsTest(unittest.TestCase):
    def test_storage_node_mapping_is_unchanged_for_reads_and_writes(self):
        model = TRACE.Model(copy.deepcopy(CONFIG))
        for key in ["cached", "block-a", "block-b", "decode:7:output"]:
            slot = TRACE.stable_hash_int("kv-storage-endpoint", key) % 252
            expected = CONFIG["storage"]["nodes"][slot // 12]
            for stage in ["PREFIX_READ", "PREFIX_STORE_WRITE", "STORAGE_TO_D", "DECODE_WRITE"]:
                for rank in range(8):
                    node, port = model.storage_access_endpoint(key, 7, stage, 4, rank)
                    self.assertEqual(node, expected)
                    self.assertIn(port, CONFIG["storage"]["ports"])

    def test_rank_spread_layer_rotation_and_sparse_physical_ids(self):
        config = copy.deepcopy(CONFIG)
        config["storage"]["ports"] = [1, 3, 5, 7, 9, 11, 15, 17, 19, 21, 23, 25]
        model = TRACE.Model(config)
        for layer in range(12):
            ports = [model.storage_access_endpoint("a", 5, "PREFIX_STORE_WRITE", layer, r)[1]
                     for r in range(8)]
            self.assertEqual(len(set(ports)), 8)
        for rank in range(8):
            ports = {model.storage_access_endpoint("a", 5, "PREFIX_STORE_WRITE", layer, rank)[1]
                     for layer in range(12)}
            self.assertEqual(ports, set(config["storage"]["ports"]))

    def test_scoring_endpoints_match_nonempty_generated_members(self):
        model = TRACE.Model(copy.deepcopy(CONFIG))
        # Include zero-byte layers and ranks, as well as the full-size model.
        for size in [1, 17, 78, 79, 512 * model.kv_bytes_per_token]:
            for degree in [2, 8]:
                expected = set()
                for layer in range(model.num_layers):
                    layer_bytes = model.layer_bytes(size, layer)
                    for rank in range(degree):
                        if TRACE.split_exact(layer_bytes, degree, rank)[1]:
                            expected.add(model.storage_access_endpoint("a", 5, "PREFIX_READ", layer, rank))
                self.assertEqual(set(model.storage_read_endpoints("a", 5, "PREFIX_READ", degree, size)), expected)

    def test_placement_is_repeatable_and_does_not_depend_on_call_order(self):
        model = TRACE.Model(copy.deepcopy(CONFIG))
        arguments = [("a", req, "STORAGE_TO_D", layer, rank)
                     for req in range(4) for layer in range(12) for rank in range(2)]
        first = [model.storage_access_endpoint(*args) for args in arguments]
        reversed_result = [model.storage_access_endpoint(*args) for args in reversed(arguments)]
        self.assertEqual(first, list(reversed(reversed_result)))
        other = TRACE.Model(copy.deepcopy(CONFIG))
        self.assertEqual(first, [other.storage_access_endpoint(*args) for args in arguments])

    def test_read_swaps_both_nodes_and_ports(self):
        row = dict(src_node="20", src_port="9", dst_node="12", dst_port="6")
        self.assertEqual(TRAFFIC.ub_endpoints(row, "STORAGE_TO_D"), (12, 20, 6, 9))
        self.assertEqual(TRAFFIC.ub_endpoints(row, "PREFIX_READ"), (12, 20, 6, 9))
        self.assertEqual(TRAFFIC.ub_endpoints(row, "PREFIX_STORE_WRITE"), (20, 12, 9, 6))

    def test_missing_negative_or_unencodable_ports_are_rejected(self):
        for bad in [None, "", "-1", "255", "4294967295"]:
            with self.subTest(port=bad), self.assertRaises(ValueError):
                TRAFFIC.ub_endpoints(dict(src_node="20", dst_node="12", src_port=bad, dst_port="0"), "PREFIX_READ")


if __name__ == "__main__":
    unittest.main()
