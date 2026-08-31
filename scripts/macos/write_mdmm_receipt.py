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


def submodule_commit(source: pathlib.Path, name: str) -> str:
    relative = pathlib.PurePosixPath(
        git(source, "config", "-f", ".gitmodules", "--get", f"submodule.{name}.path")
    )
    if relative.is_absolute() or ".." in relative.parts:
        raise RuntimeError(f"unsafe path for submodule {name}: {relative}")

    checkout = (source / pathlib.Path(*relative.parts)).resolve()
    try:
        checkout.relative_to(source)
    except ValueError as error:
        raise RuntimeError(f"submodule {name} resolves outside the source tree: {checkout}") from error

    expected = git(source, "rev-parse", f"HEAD:{relative.as_posix()}")
    actual_root = pathlib.Path(git(checkout, "rev-parse", "--show-toplevel")).resolve()
    if actual_root != checkout:
        raise RuntimeError(
            f"submodule {name} is not initialized at {relative} "
            f"(Git resolved it to {actual_root})"
        )

    actual = git(checkout, "rev-parse", "HEAD")
    if actual != expected:
        raise RuntimeError(
            f"submodule {name} checkout does not match the parent gitlink: "
            f"expected {expected}, found {actual}"
        )
    return actual


def source_tuple(source: pathlib.Path) -> dict[str, str]:
    return {
        "source_commit": git(source, "rev-parse", "HEAD"),
        "dsp56300_commit": submodule_commit(source, "source/dsp56300"),
        "mc68k_commit": submodule_commit(source, "source/mc68k"),
    }


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
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--artifact", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--package-file", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--check-source-only", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    commits = source_tuple(source)
    if args.check_source_only:
        print(json.dumps(commits, sort_keys=True))
        return
    if args.output is None or not args.artifact:
        parser.error("--output and at least one --artifact are required when writing a receipt")

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

    package_files = []
    for package_file in args.package_file:
        package_file = package_file.resolve()
        package_files.append(
            {
                "name": package_file.name,
                "bytes": package_file.stat().st_size,
                "sha256": sha256(package_file),
            }
        )

    receipt = {
        "schema": "gearmulator-elektron-macos-build-v1",
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "configuration": "Release",
        "architecture": "x86_64 arm64",
        "signing": "ad-hoc",
        "notarized": False,
        **commits,
        "firmware_included": False,
        "tests_run": True,
        "artifacts": artifacts,
        "package_files": package_files,
    }
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
