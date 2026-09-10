#!/usr/bin/env python3
"""Run the native, output-only MD/MM core-capacity release microgate."""

from __future__ import annotations

import argparse
import atexit
import csv
import hashlib
import json
import math
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import sys
import xml.etree.ElementTree as ET
from typing import Iterable, Optional


FIRMWARE_SHA256 = {
    "MD": "68542e30917b9918ccaee2b2237df62c8a00479938680b85aca93ce4fbca44c8",
    "MM": "369849175602e20a9dd2b6e0ad8ac404b76f82718b14afbf1cbc01b7acabec7e",
}
PRODUCT_FOLDER = {"MD": "Machinedrum", "MM": "Monomachine"}
NOTE_NUMBER = {"MD": 36, "MM": 60}
RECEIPT_SCHEMA = "gearmulator-mdmm-core-capacity-v1"
SCOPE = "executed VST3 output-only core callback duration"


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bundle_module(bundle: pathlib.Path) -> pathlib.Path:
    modules = [path for path in (bundle / "Contents" / "MacOS").iterdir() if path.is_file()]
    if len(modules) != 1:
        raise RuntimeError(f"expected one executable module in {bundle}, found {len(modules)}")
    return modules[0]


def percentile(values: Iterable[float], quantile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise RuntimeError("cannot calculate a percentile of no values")
    position = (len(ordered) - 1) * quantile
    below = int(position)
    above = min(below + 1, len(ordered) - 1)
    fraction = position - below
    return ordered[below] * (1 - fraction) + ordered[above] * fraction


def analyze_blocks(path: pathlib.Path, rate: int, warm_start_seconds: int) -> dict[str, object]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise RuntimeError(f"callback capture is empty: {path}")

    position = 0
    warm = []
    for row in rows:
        sample = int(row["sample"])
        count = int(row["count"])
        if sample != position or count <= 0:
            raise RuntimeError(f"callback timeline has a gap, overlap, or empty block: {path}")
        position += count
        if sample >= warm_start_seconds * rate:
            period_ms = count * 1000.0 / rate
            render_ms = float(row["render_ms"])
            late_ms = float(row["late_ms"])
            if render_ms < 0 or not math.isfinite(render_ms) or not math.isfinite(late_ms):
                raise RuntimeError(f"callback capture contains invalid timing data: {path}")
            warm.append((render_ms, late_ms, period_ms))
    if not warm:
        raise RuntimeError(f"callback capture has no warm samples: {path}")

    durations = [row[0] for row in warm]
    scheduler_lateness = [row[1] for row in warm]
    budget_fractions = [row[0] / row[2] for row in warm]
    over_budget = sum(row[0] > row[2] for row in warm)
    completed_late = sum(row[0] + row[1] > row[2] for row in warm)
    return {
        "total_callbacks": len(rows),
        "total_samples": position,
        "warm_callbacks": len(warm),
        "p50_ms": percentile(durations, 0.50),
        "p99_ms": percentile(durations, 0.99),
        "max_ms": max(durations),
        "p50_budget_fraction": percentile(budget_fractions, 0.50),
        "p99_budget_fraction": percentile(budget_fractions, 0.99),
        "max_budget_fraction": max(budget_fractions),
        "over_budget": over_budget,
        "over_budget_fraction": over_budget / len(warm),
        # Scheduler arrival time is reported separately from plug-in duration.
        # It is useful live-system evidence but is not a portable CPU-capacity gate.
        "scheduler_late_p99_ms": percentile(scheduler_lateness, 0.99),
        "completion_after_deadline": completed_late,
        "completion_after_deadline_fraction": completed_late / len(warm),
    }


def aggregate(
    capacity: list[dict[str, object]],
    paced: list[dict[str, object]],
    capacity_p50_limit: float,
) -> dict[str, object]:
    capacity_p50 = statistics.median(float(run["p50_budget_fraction"]) for run in capacity)
    paced_p99 = statistics.median(float(run["p99_budget_fraction"]) for run in paced)
    paced_over_budget = statistics.median(
        float(run["over_budget_fraction"]) for run in paced
    )
    if not all(
        math.isfinite(value)
        for value in (capacity_p50, paced_p99, paced_over_budget)
    ):
        raise RuntimeError("core-capacity check contains non-finite measurements")
    core_microgate_passed = capacity_p50 <= capacity_p50_limit
    paced_render_overruns = sum(int(run["over_budget"]) for run in paced)
    # A paced tail is called qualified only when every measured callback met
    # its render-duration budget. Scheduler wake-up lateness remains separate.
    paced_tail_qualification_passed = paced_render_overruns == 0
    return {
        "capacity_median_p50_budget_fraction": capacity_p50,
        "capacity_limit": capacity_p50_limit,
        "core_microgate_passed": core_microgate_passed,
        "paced_median_p99_budget_fraction": paced_p99,
        "paced_median_over_budget_fraction": paced_over_budget,
        "paced_render_overruns": paced_render_overruns,
        "paced_tail_qualification_rule": "zero render-duration overruns in every paced run",
        "paced_tail_qualification_passed": paced_tail_qualification_passed,
    }


def write_settings(data_root: pathlib.Path, model: str) -> None:
    config = (
        data_root
        / "Gearmulator Preview"
        / PRODUCT_FOLDER[model]
        / "config"
        / f"Gearmulator {model}.xml"
    )
    config.parent.mkdir(parents=True)
    tree = ET.ElementTree(ET.Element("PROPERTIES"))
    ET.SubElement(tree.getroot(), "VALUE", name="enableMcpServer", val="0")
    ET.SubElement(tree.getroot(), "VALUE", name="latencyBlocks", val="0")
    tree.write(config, encoding="utf-8", xml_declaration=True)


def run_capture(
    host: pathlib.Path,
    plugin: pathlib.Path,
    firmware: pathlib.Path,
    work_root: pathlib.Path,
    model: str,
    mode: str,
    repetition: int,
    rate: int,
    block: int,
    seconds: int,
    warm_start_seconds: int,
) -> dict[str, object]:
    case = work_root / f"{model.lower()}-{mode}-{repetition}"
    data_root = case / "data"
    home = case / "home"
    rom_dir = data_root / "Gearmulator Preview" / PRODUCT_FOLDER[model] / "roms"
    rom_dir.mkdir(parents=True)
    home.mkdir(parents=True)
    shutil.copyfile(firmware, rom_dir / f"validated-{model.lower()}.bin")
    write_settings(data_root, model)

    prefix = case / "capture"
    command = [
        str(host),
        str(plugin),
        str(prefix),
        str(rate),
        str(block),
        str(seconds),
        "-1",
        "fixed",
        "0" if mode == "capacity" else "-1",
        "fast" if mode == "capacity" else "paced",
        str(NOTE_NUMBER[model]),
        str(block - 1),
        "notes",
        "messages",
        "-1",
    ]
    environment = {
        key: value for key, value in os.environ.items() if not key.startswith("GEARMULATOR_")
    }
    environment["HOME"] = str(home)
    environment["GEARMULATOR_DATA_ROOT"] = str(data_root)
    log_path = case / "host.log"
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run(
            command,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=max(180, seconds * 4),
        )
    if result.returncode:
        sys.stderr.write(log_path.read_text(encoding="utf-8", errors="replace"))
        raise RuntimeError(
            f"{model} {mode} core-capacity capture {repetition} failed with {result.returncode}"
        )
    metadata_path = prefix.with_suffix(".json")
    blocks_path = prefix.with_suffix(".blocks.csv")
    audio_path = prefix.with_suffix(".wav")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    expected_metadata = {
        "format": "VST3",
        "sample_rate": float(rate),
        "block_size": block,
        "seconds": float(seconds),
        "variable_blocks": False,
        "reprepare_seconds": -1.0,
        "offline_after_seconds": 0.0 if mode == "capacity" else -1.0,
        "pace_offline": mode == "paced",
        "note_number": NOTE_NUMBER[model],
        "note_phase": block - 1,
        "scenario": "notes",
        "suppress_message_loop": False,
        "audio_finite_before_quantization": True,
    }
    actual_metadata = {key: metadata.get(key) for key in expected_metadata}
    if actual_metadata != expected_metadata:
        raise RuntimeError(
            f"{model} {mode} capture metadata mismatch: "
            f"expected {expected_metadata}, found {actual_metadata}"
        )
    audio_peak = float(metadata.get("audio_peak_before_quantization", 0.0))
    if not math.isfinite(audio_peak) or audio_peak <= 1e-5:
        raise RuntimeError(f"{model} {mode} capture produced no finite audible output")

    measurement = analyze_blocks(blocks_path, rate, warm_start_seconds)
    if measurement["total_samples"] != round(rate * seconds):
        raise RuntimeError(f"{model} {mode} capture length does not match the workload")
    measurement["audio_peak_before_quantization"] = audio_peak
    measurement["capture_sha256"] = {
        "capture.blocks.csv": sha256(blocks_path),
        "capture.json": sha256(metadata_path),
        "capture.wav": sha256(audio_path),
        "host.log": sha256(log_path),
    }
    # The public receipt retains hashes and measurements, never the private ROM
    # copy or rendered audio. Bound disk use by removing each case immediately.
    shutil.rmtree(case)
    return measurement


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=pathlib.Path, required=True)
    parser.add_argument(
        "--host-architecture", choices=("arm64", "x86_64"), default=platform.machine()
    )
    parser.add_argument("--md-plugin", type=pathlib.Path, required=True)
    parser.add_argument("--mm-plugin", type=pathlib.Path, required=True)
    parser.add_argument("--md-firmware", type=pathlib.Path, required=True)
    parser.add_argument("--mm-firmware", type=pathlib.Path, required=True)
    parser.add_argument("--work-root", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--rate", type=int, default=48000)
    parser.add_argument("--block", type=int, default=128)
    parser.add_argument("--seconds", type=int, default=20)
    parser.add_argument("--warm-start-seconds", type=int, default=12)
    parser.add_argument("--capacity-repeats", type=int, default=3)
    parser.add_argument("--paced-repeats", type=int, default=3)
    parser.add_argument("--capacity-p50-limit", type=float, default=0.90)
    return parser.parse_args(argv)


def main() -> None:
    args = parse_args()
    if (
        args.rate < 8000
        or args.block < 1
        or args.seconds < 20
        or args.warm_start_seconds < 0
        or args.warm_start_seconds >= args.seconds
        or args.capacity_repeats < 1
        or args.paced_repeats < 1
    ):
        raise RuntimeError("invalid core-capacity timing or repetition count")
    if args.capacity_p50_limit < 0:
        raise RuntimeError("core-capacity limit must be nonnegative")

    host = args.host.resolve(strict=True)
    plugins = {
        "MD": args.md_plugin.resolve(strict=True),
        "MM": args.mm_plugin.resolve(strict=True),
    }
    firmware = {
        "MD": args.md_firmware.resolve(strict=True),
        "MM": args.mm_firmware.resolve(strict=True),
    }
    for model in ("MD", "MM"):
        if sha256(firmware[model]) != FIRMWARE_SHA256[model]:
            raise RuntimeError(f"{model} firmware hash does not match the pinned release image")
    work_root = args.work_root.resolve()
    work_root.mkdir(parents=True, exist_ok=False)
    # Captures contain private firmware and rendered audio. The build wrapper
    # has its own trap as a second line of defense, but direct runs clean too.
    atexit.register(shutil.rmtree, work_root, ignore_errors=True)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise RuntimeError(f"refusing to replace core-capacity receipt: {output}")

    models = {}
    for model in ("MD", "MM"):
        capacity = [
            run_capture(
                host,
                plugins[model],
                firmware[model],
                work_root,
                model,
                "capacity",
                repetition,
                args.rate,
                args.block,
                args.seconds,
                args.warm_start_seconds,
            )
            for repetition in range(1, args.capacity_repeats + 1)
        ]
        paced = [
            run_capture(
                host,
                plugins[model],
                firmware[model],
                work_root,
                model,
                "paced",
                repetition,
                args.rate,
                args.block,
                args.seconds,
                args.warm_start_seconds,
            )
            for repetition in range(1, args.paced_repeats + 1)
        ]
        models[model] = {
            "capacity_runs": capacity,
            "paced_runs": paced,
            "gate": aggregate(
                capacity,
                paced,
                args.capacity_p50_limit,
            ),
        }

    core_microgate_passed = all(
        model["gate"]["core_microgate_passed"] for model in models.values()
    )
    paced_tail_qualification_passed = all(
        model["gate"]["paced_tail_qualification_passed"] for model in models.values()
    )
    receipt = {
        "schema": RECEIPT_SCHEMA,
        "scope": SCOPE,
        "core_microgate_passed": core_microgate_passed,
        "paced_tail_qualification_passed": paced_tail_qualification_passed,
        "limitations": [
            "headless host; no standalone editor or renderer",
            "zero input channels; physical and plug-in audio input are not exercised",
            "only the selected architecture executed by the host is measured",
            "scheduler lateness is reported separately from plug-in render duration",
        ],
        "host_architecture": args.host_architecture,
        "host_os": platform.platform(),
        "host_name": host.name,
        "sample_rate": args.rate,
        "block_size": args.block,
        "period_ms": args.block * 1000.0 / args.rate,
        "seconds": args.seconds,
        "warm_start_seconds": args.warm_start_seconds,
        "repetitions": {"capacity": args.capacity_repeats, "paced": args.paced_repeats},
        "workload": {
            "plugin_format": "VST3",
            "audio_input_channels": 0,
            "audio_output_channels": 2,
            "scenario": "notes",
            "fixed_block_size": True,
            "note_phase": args.block - 1,
            "message_loop": True,
            "capacity_mode": "unpaced offline render",
            "paced_mode": "paced render",
        },
        "plugin_module_sha256": {
            plugins[model].name: sha256(bundle_module(plugins[model]))
            for model in ("MD", "MM")
        },
        "host_sha256": sha256(host),
        "firmware_sha256": FIRMWARE_SHA256,
        "models": models,
    }
    output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2, sort_keys=True))
    if not paced_tail_qualification_passed:
        print(
            "Paced tail observation is not qualified: at least one callback exceeded "
            "its render-duration budget",
            file=sys.stderr,
        )
    if not core_microgate_passed:
        raise RuntimeError("MD/MM output-only core-capacity microgate failed")


if __name__ == "__main__":
    main()
