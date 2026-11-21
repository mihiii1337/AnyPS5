import gzip
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import trace_diff


def complete(tid, name, ts, dur):
    return {"name": name, "ph": "X", "ts": ts, "dur": dur, "pid": 7, "tid": tid}


def events(draw_us, extra_thread=False):
    items = [
        {"name": "thread_name", "ph": "M", "ts": 0, "pid": 7, "tid": 1, "args": {"name": "queue worker 0x0"}},
        {"name": "thread_name", "ph": "M", "ts": 0, "pid": 7, "tid": 2, "args": {"name": "presenter"}},
        complete(1, "Driver.Packet", 0, draw_us),
        complete(2, "Driver.Present", 0, 2000),
        {"name": "present", "ph": "i", "ts": 0, "pid": 7, "tid": 2, "s": "g", "args": {"present": 600}},
        {"name": "present", "ph": "i", "ts": 20000, "pid": 7, "tid": 2, "s": "g", "args": {"present": 601}},
    ]
    if extra_thread:
        items.insert(2, {"name": "thread_name", "ph": "M", "ts": 0, "pid": 7, "tid": 3, "args": {"name": "gpu"}})
        items.append(complete(3, "Vulkan.Submit", 0, 1000))
    return items


def write_trace(directory, name, items):
    path = Path(directory) / name
    with gzip.open(path, "wt", encoding="utf-8") as handle:
        json.dump({"traceEvents": items}, handle)
    return path


class DiffTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def diff(self, a_items, b_items):
        a = write_trace(self.tmp.name, "a.json.gz", a_items)
        b = write_trace(self.tmp.name, "b.json.gz", b_items)
        return trace_diff.compare(a, b, top=10)

    def test_identical_traces_have_zero_deltas(self):
        result = self.diff(events(10000), events(10000))
        for row in result["threads"]:
            self.assertAlmostEqual(row["busy_delta_ms_per_frame"], 0.0)
            self.assertAlmostEqual(row["wait_delta_ms_per_frame"], 0.0)
        for row in result["slices"]:
            self.assertAlmostEqual(row["delta_ms_per_frame"], 0.0)

    def test_busier_side_shows_a_positive_delta(self):
        result = self.diff(events(10000), events(16000))
        packet = next(row for row in result["slices"] if row["name"] == "Driver.Packet")
        self.assertGreater(packet["delta_ms_per_frame"], 0.0)
        self.assertAlmostEqual(packet["b_ms_per_frame"] - packet["a_ms_per_frame"], packet["delta_ms_per_frame"])

    def test_slice_deltas_use_both_timings_before_the_top_cutoff(self):
        frame = [item for item in events(0) if item["ph"] != "X"]
        frame[-1]["ts"] = 120000
        durations = [10000 - 500 * index for index in range(11)]

        def capture(values):
            return frame + [complete(1, f"S{index:02d}", index * 10000, duration) for index, duration in enumerate(values)]

        rows = self.diff(capture(durations), capture(durations[:9] + [4000, 9800]))["slices"]
        self.assertEqual(len(rows), 10)
        self.assertEqual([row["name"] for row in rows[:2]], ["S10", "S09"])
        for row, (before, after, delta) in zip(rows, [(5.0, 9.8, 4.8), (5.5, 4.0, -1.5)]):
            self.assertEqual((row["a_ms_per_frame"], row["b_ms_per_frame"]), (before, after))
            self.assertAlmostEqual(row["delta_ms_per_frame"], delta)

    def test_a_thread_present_on_one_side_counts_as_zero_on_the_other(self):
        result = self.diff(events(10000), events(10000, extra_thread=True))
        gpu = next(row for row in result["threads"] if row["thread"] == "gpu")
        self.assertEqual(gpu["a_busy_ms_per_frame"], 0.0)
        self.assertGreater(gpu["b_busy_ms_per_frame"], 0.0)
        self.assertEqual(gpu["busy_delta_ms_per_frame"], gpu["b_busy_ms_per_frame"])

    def test_the_cli_writes_json_and_rejects_a_missing_file(self):
        a = write_trace(self.tmp.name, "a.json.gz", events(10000))
        b = write_trace(self.tmp.name, "b.json.gz", events(12000))
        tool = Path(trace_diff.__file__)
        done = subprocess.run([sys.executable, str(tool), str(a), str(b), "--json"], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("threads", json.loads(done.stdout))
        missing = subprocess.run([sys.executable, str(tool), str(a), str(Path(self.tmp.name) / "none.json.gz")], capture_output=True, text=True)
        self.assertEqual(missing.returncode, 2)
        self.assertIn("FAIL", missing.stderr)


if __name__ == "__main__":
    unittest.main()
