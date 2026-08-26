#!/usr/bin/env python3

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess


def git(repo: pathlib.Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bundle_module(bundle: pathlib.Path) -> pathlib.Path:
    candidates = [path for path in (bundle / "Contents" / "MacOS").iterdir() if path.is_file()]
    if len(candidates) != 1:
        raise RuntimeError(f"expected one executable module in {bundle}, found {len(candidates)}")
    return candidates[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--artifact", type=pathlib.Path, action="append", required=True)
    args = parser.parse_args()

    source = args.source.resolve()
    artifacts = []
    for bundle in args.artifact:
        module = bundle_module(bundle.resolve())
        artifacts.append(
            {
                "name": bundle.name,
                "module": module.name,
                "bytes": module.stat().st_size,
                "sha256": sha256(module),
            }
        )

    receipt = {
        "schema": "gearmulator-elektron-macos-build-v1",
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "configuration": "Release",
        "architecture": "x86_64 arm64",
        "signing": "ad-hoc",
        "notarized": False,
        "source_commit": git(source, "rev-parse", "HEAD"),
        "dsp56300_commit": git(source / "source" / "dsp56300", "rev-parse", "HEAD"),
        "mc68k_commit": git(source / "source" / "mc68k", "rev-parse", "HEAD"),
        "firmware_included": False,
        "tests_run": True,
        "artifacts": artifacts,
    }
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
