#!/usr/bin/env python3
"""Run ROM-dependent confidence probes; retain logs, hashes, and exit statuses.

No ROMs, caches, or public banks are downloaded or redistributed by this runner.
Nonzero probe results remain failures, including known firmware incompatibility
and early-boot data-loss probes. The report never relabels them as import passes.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("corpus", "fault", "readiness", "all", "workflow", "storage-readiness", "next"), default="all")
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--md-rom", type=Path)
    parser.add_argument("--md-cache", type=Path)
    parser.add_argument("--mm-rom", type=Path)
    parser.add_argument("--mm-patch", type=Path)
    parser.add_argument("--corpus", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", action="append", dest="selected_cases", default=[],
                        help="run only this exact case label; repeat to select several")
    parser.add_argument("--timeout-seconds", type=int, default=600,
                        help="external per-case deadline, including a stuck emulation advance")
    args = parser.parse_args()
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / "report.json").exists():
        parser.error("use a new output directory; an existing report will not be overwritten")
    binary = args.bin_dir.resolve()
    if args.suite != "workflow" and not all((args.md_rom, args.md_cache)):
        parser.error("this suite requires --md-rom and --md-cache")
    md_rom = str(args.md_rom.resolve()) if args.md_rom else ""
    md_cache = str(args.md_cache.resolve()) if args.md_cache else ""
    cases = []
    sds = [str(binary / "mdSdsFirmwareTest"), md_rom, "--generated", md_cache]
    if args.suite in ("corpus", "all"):
        if not all((args.corpus, args.mm_rom, args.mm_patch)):
            parser.error("corpus testing requires --corpus, --mm-rom, and --mm-patch")
        corpus = args.corpus.resolve()
        md_files = sorted(corpus.glob("md-*.syx"))
        md_files += sorted((corpus / "md-official-selected").glob("*SONG*.syx"))
        md_files += sorted((corpus / "md-official-selected").glob("MD_presets*.syx"))
        mm_files = sorted((corpus / "mm-digipro").glob("*.syx"))
        sample_files = sorted((corpus / "md-official-selected").glob("01_*_BD01.syx"))
        if (len(md_files), len(mm_files), len(sample_files)) != (8, 24, 2):
            parser.error("expected the documented 8 MD + 24 MM + 2 SDS corpus files")
        for file in md_files:
            cases.append(("md-" + file.stem, [str(binary / "mdUserSysexFirmwareTest"), "md", md_rom, str(file)], file))
        for file in mm_files:
            cases.append(("mm-" + file.stem, [str(binary / "mdUserSysexFirmwareTest"), "mm", str(args.mm_rom.resolve()), str(args.mm_patch.resolve()), str(file)], file))
        for file in sample_files:
            cases.append(("sds-" + file.stem, [sds[0], md_rom, str(file), md_cache], file))
    if args.suite in ("fault", "all"):
        for mode in ("none", "corrupt-packet", "drop-ack", "delay-ack", "duplicate-ack", "wait", "silence", "drop-header-ack", "drop-final-ack"):
            cases.append(("fault-" + mode, sds + [mode], None))
    if args.suite in ("readiness", "all"):
        for mode in ("boot", "repeat", "cancel"):
            for delay in (0, 1, 5, 20):
                cases.append((f"ready-{mode}-{delay}", sds + [f"{mode}:{delay}"], None))
    if args.suite in ("storage-readiness", "next"):
        for mode in ("boot", "restored", "uncached", "cold"):
            for delay in (0, 5):
                cases.append((f"storage-{mode}-{delay}", sds + [f"{mode}:{delay}"], None))
        for mode in ("repeat:0", "cancel:0", "quiet:2", "pc:0"):
            cases.append(("storage-" + mode.replace(":", "-"), sds + [mode], None))
    if args.suite in ("workflow", "next"):
        if not all((args.corpus, args.mm_rom, args.mm_patch)):
            parser.error("workflow testing requires --corpus, --mm-rom, and --mm-patch")
        workflow = [str(binary / "mmSysexWorkflowTest"), str(args.mm_rom.resolve()), str(args.mm_patch.resolve())]
        cases.append(("mm-import-audio-oracle", [workflow[0], "--audio-oracle"], None))
        bank_dir = args.corpus.resolve() / "mm-digipro"
        for name in ("1 SINE-EXT.syx", "2 TRI--INV.syx", "5 FM---INV.syx"):
            file = bank_dir / name
            if not file.is_file():
                parser.error(f"missing documented workflow bank: {file}")
            cases.append(("workflow-" + file.stem, workflow + [str(file)], file))
        file = bank_dir / "2 TRI--INV.syx"
        for mode in ("mixed", "cancel-before-digipro", "cancel-before-general"):
            cases.append(("workflow-" + mode, workflow + [str(file), mode], file))
    if args.selected_cases:
        requested = set(args.selected_cases)
        unknown = requested - {label for label, _, _ in cases}
        if unknown:
            parser.error("unknown case label(s) for this suite: " + ", ".join(sorted(unknown)))
        cases = [case for case in cases if case[0] in requested]
    fixtures = [args.md_rom, args.md_cache, args.mm_rom, args.mm_patch]
    executables = {Path(command[0]) for _, command, _ in cases}
    report = {"suite": args.suite, "selected_cases": args.selected_cases,
              "timeout_seconds": args.timeout_seconds,
              "runtime_environment": {key: os.environ.get(key) for key in
                                      ("MM_SYSEX_BLOCK_PROFILE", "GEARMULATOR_MDMM_BOUNDED_JIT")},
              "fixtures": {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in fixtures if p},
              "executables": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(executables)}, "results": []}
    for label, command, fixture in cases:
        started = time.monotonic()
        log = args.output / (label + ".log")
        with log.open("x") as output:
            try:
                completed = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                           timeout=args.timeout_seconds, check=False)
                status = completed.returncode
            except subprocess.TimeoutExpired:
                status = 124
        result = {"case": label, "command": command, "exit_code": status,
                  "wall_seconds": round(time.monotonic() - started, 3), "log": str(log)}
        if fixture:
            result["input_sha256"] = hashlib.sha256(fixture.read_bytes()).hexdigest()
        report["results"].append(result)
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"{'PASS' if status == 0 else 'FAIL'} {label} ({result['wall_seconds']}s)", flush=True)
    return int(any(result["exit_code"] != 0 for result in report["results"]))


if __name__ == "__main__":
    raise SystemExit(main())
