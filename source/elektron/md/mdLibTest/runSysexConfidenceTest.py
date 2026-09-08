#!/usr/bin/env python3
"""ROM-free tests of suite composition and honest failure reporting."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import runSysexConfidence


class RunnerTest(unittest.TestCase):
    def run_suite(self, suite, fail=False, timeout=False, selected=(), timeout_seconds=600):
        with tempfile.TemporaryDirectory(prefix="sysex-runner-test-") as folder:
            root = Path(folder)
            banks = root / "mm-digipro"
            banks.mkdir()
            for name in ("1 SINE-EXT.syx", "2 TRI--INV.syx", "5 FM---INV.syx",
                         "mdSdsFirmwareTest", "mmSysexWorkflowTest", "md-rom", "cache", "mm-rom", "patch"):
                (banks / name if name.endswith(".syx") else root / name).write_bytes(b"test fixture")
            args = ["runner", "--suite", suite, "--bin-dir", str(root), "--corpus", str(root),
                    "--mm-rom", str(root / "mm-rom"), "--mm-patch", str(root / "patch"),
                    "--output", str(root / "output")]
            if suite != "workflow":
                args += ["--md-rom", str(root / "md-rom"), "--md-cache", str(root / "cache")]
            args += ["--timeout-seconds", str(timeout_seconds)]
            for label in selected:
                args += ["--case", label]

            def execute(command, **kwargs):
                self.assertFalse(kwargs["check"])
                self.assertEqual(kwargs["timeout"], timeout_seconds)
                if timeout:
                    raise subprocess.TimeoutExpired(command, kwargs["timeout"])
                return subprocess.CompletedProcess(command, int(fail))

            with patch.object(sys, "argv", args), patch("subprocess.run", side_effect=execute), patch("builtins.print"):
                status = runSysexConfidence.main()
            report = json.loads((root / "output" / "report.json").read_text())
            self.assertEqual(status, int(fail or timeout))
            self.assertTrue(all(r["exit_code"] == (124 if timeout else int(fail)) for r in report["results"]))
            self.assertTrue(all(value == hashlib.sha256(b"test fixture").hexdigest() for value in report["executables"].values()))
            return report["results"]

    def test_workflow_without_md_fixtures(self):
        results = self.run_suite("workflow")
        self.assertEqual(len(results), 7)  # oracle, three banks, three mixed modes
        self.assertEqual(sum("input_sha256" in r for r in results), 6)
        self.assertTrue(any(r["command"][-1] == "cancel-before-general" for r in results))

    def test_storage_failures_are_not_relabelled(self):
        results = self.run_suite("storage-readiness", fail=True)
        self.assertEqual(len(results), 12)
        self.assertEqual({r["command"][-1] for r in results}, {
            "boot:0", "boot:5", "restored:0", "restored:5", "uncached:0", "uncached:5",
            "cold:0", "cold:5", "repeat:0", "cancel:0", "quiet:2", "pc:0"})

    def test_timeouts_and_combined_suite(self):
        self.assertEqual(len(self.run_suite("next", timeout=True)), 19)

    def test_exact_subset_with_external_deadline(self):
        labels = ("workflow-2 TRI--INV", "workflow-mixed")
        results = self.run_suite("workflow", selected=labels, timeout_seconds=120)
        self.assertEqual([r["case"] for r in results], list(labels))
        timed_out = self.run_suite("workflow", selected=(labels[1],), timeout=True, timeout_seconds=120)
        self.assertEqual([r["exit_code"] for r in timed_out], [124])

    def test_unknown_case_does_not_silently_pass(self):
        with self.assertRaises(SystemExit) as error:
            self.run_suite("workflow", selected=("workflow-mixde",))
        self.assertEqual(error.exception.code, 2)

    def test_nonpositive_deadline_rejected(self):
        with self.assertRaises(SystemExit) as error:
            self.run_suite("workflow", timeout_seconds=0)
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
