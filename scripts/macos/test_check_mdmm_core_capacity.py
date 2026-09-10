#!/usr/bin/env python3

from __future__ import annotations

import csv
import pathlib
import tempfile
import unittest

import check_mdmm_core_capacity as capacity_check


class CoreCapacityCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def write_blocks(self, durations: list[float], late_ms: float = 0.0) -> pathlib.Path:
        path = self.root / "capture.blocks.csv"
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(
                ["sample", "count", "late_ms", "render_ms", "reported_latency_samples"]
            )
            for index, duration in enumerate(durations):
                writer.writerow([index * 128, 128, late_ms, duration, 0])
        return path

    def test_explicit_host_architecture_is_recordable_for_single_slice_builds(self) -> None:
        args = capacity_check.parse_args(
            [
                "--host",
                "host",
                "--host-architecture",
                "x86_64",
                "--md-plugin",
                "md.vst3",
                "--mm-plugin",
                "mm.vst3",
                "--md-firmware",
                "md.bin",
                "--mm-firmware",
                "mm.bin",
                "--work-root",
                "work",
                "--output",
                "receipt.json",
            ]
        )

        self.assertEqual(args.host_architecture, "x86_64")

    def test_block_analysis_separates_duration_and_scheduler_lateness(self) -> None:
        result = capacity_check.analyze_blocks(
            self.write_blocks([2.0, 2.2, 2.4], late_ms=1.0),
            rate=48000,
            warm_start_seconds=0,
        )

        self.assertEqual(result["over_budget"], 0)
        self.assertEqual(result["completion_after_deadline"], 3)
        self.assertAlmostEqual(result["p50_budget_fraction"], 0.825)

    def test_analysis_rejects_callback_timeline_gaps(self) -> None:
        path = self.write_blocks([1.0, 1.0])
        rows = path.read_text(encoding="utf-8").replace("128,128", "129,128")
        path.write_text(rows, encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "timeline has a gap"):
            capacity_check.analyze_blocks(path, rate=48000, warm_start_seconds=0)

    def test_repeated_medians_reject_the_ordinary_md_capacity_regression(self) -> None:
        def run(p50: float, p99: float, over: float = 0.0) -> dict[str, float]:
            return {
                "p50_budget_fraction": p50,
                "p99_budget_fraction": p99,
                "over_budget": int(over > 0),
                "over_budget_fraction": over,
            }

        result = capacity_check.aggregate(
            [run(1.01, 1.03), run(1.02, 1.04), run(1.01, 1.05)],
            [run(1.01, 1.04, 0.02), run(1.02, 1.05, 0.02), run(1.01, 1.03, 0.01)],
            capacity_p50_limit=0.90,
        )

        self.assertFalse(result["core_microgate_passed"])
        self.assertFalse(result["paced_tail_qualification_passed"])

    def test_scheduler_lateness_is_not_a_duration_gate(self) -> None:
        healthy = {
            "p50_budget_fraction": 0.80,
            "p99_budget_fraction": 0.90,
            "over_budget": 0,
            "over_budget_fraction": 0.0,
            "completion_after_deadline_fraction": 1.0,
        }
        result = capacity_check.aggregate(
            [healthy, healthy, healthy],
            [healthy, healthy, healthy],
            capacity_p50_limit=0.90,
        )

        self.assertTrue(result["core_microgate_passed"])
        self.assertTrue(result["paced_tail_qualification_passed"])

    def test_any_render_overrun_keeps_the_paced_tail_unqualified(self) -> None:
        clean = {
            "p50_budget_fraction": 0.80,
            "p99_budget_fraction": 0.90,
            "over_budget": 0,
            "over_budget_fraction": 0.0,
        }
        overrun = {**clean, "over_budget": 1, "over_budget_fraction": 1 / 3000}

        result = capacity_check.aggregate(
            [clean, clean, clean], [clean, overrun, clean], capacity_p50_limit=0.90
        )

        self.assertTrue(result["core_microgate_passed"])
        self.assertFalse(result["paced_tail_qualification_passed"])
        self.assertEqual(result["paced_render_overruns"], 1)


if __name__ == "__main__":
    unittest.main()
