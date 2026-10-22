import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("task_statistics.py")
SPEC = importlib.util.spec_from_file_location("task_statistics", MODULE_PATH)
task_statistics = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(task_statistics)


class FilterRedundantDataTest(unittest.TestCase):
    def test_uses_earliest_first_send_and_latest_last_ack(self):
        packet_data = {
            "403": [
                {"event": "First Packet Sends", "timestamp": "212.970000"},
                {"event": "Last Packet ACKs", "timestamp": "700.000000"},
                {"event": "First Packet Sends", "timestamp": "0.000000"},
                {"event": "Last Packet ACKs", "timestamp": "784.438860"},
            ]
        }

        reduced = task_statistics.filterRedundantData(packet_data)

        self.assertEqual(reduced["403"]["First Packet Sends"], 0.0)
        self.assertEqual(reduced["403"]["Last Packet ACKs"], 784.43886)


if __name__ == "__main__":
    unittest.main()
