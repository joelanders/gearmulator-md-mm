#!/usr/bin/env python3

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import shutil
import subprocess


RELEASE_ROOT_MARKER = ".gearmulator-mdmm-release-root"
RELEASE_ROOT_MARKER_VERSION = "gearmulator-mdmm-release-root-v1"


def git(repo: pathlib.Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


def _git_paths(source: pathlib.Path) -> tuple[tuple[int, pathlib.PurePosixPath], ...]:
    records = subprocess.check_output(
        ["git", "-C", str(source), "ls-files", "--stage", "-z"]
    ).split(b"\0")
    result = []
    for record in records:
        if not record:
            continue
        metadata, raw_path = record.split(b"\t", 1)
        mode = int(metadata.split(b" ", 1)[0], 8)
        path = pathlib.PurePosixPath(raw_path.decode("utf-8", errors="surrogateescape"))
        result.append((mode, path))
    return tuple(result)


def validate_cleanup_root(
    source: pathlib.Path, candidate: pathlib.Path, label: str
) -> pathlib.Path:
    source = source.resolve()
    lexical_root = pathlib.Path(os.path.abspath(os.fspath(candidate)))
    root = candidate.resolve()
    tracked_paths = _git_paths(source)
    lexical_inside_source = False
    for location in dict.fromkeys((lexical_root, root)):
        if location == source or location in source.parents:
            raise RuntimeError(f"unsafe {label} contains the source tree: {location}")

        try:
            relative = location.relative_to(source)
        except ValueError:
            continue
        if location == lexical_root:
            lexical_inside_source = True

        if relative.parts and relative.parts[0] == ".git":
            raise RuntimeError(f"unsafe {label} overlaps Git metadata: {location}")

        relative_git = pathlib.PurePosixPath(relative.as_posix())
        for mode, tracked in tracked_paths:
            if tracked == relative_git or relative_git in tracked.parents:
                raise RuntimeError(f"unsafe {label} contains tracked path: {tracked}")
            if mode == 0o160000 and tracked in relative_git.parents:
                raise RuntimeError(f"unsafe {label} is inside tracked submodule: {tracked}")
    if lexical_inside_source and lexical_root != root:
        raise RuntimeError(f"unsafe {label} follows a source-tree symlink: {lexical_root}")
    return root


def validate_release_directories(
    source: pathlib.Path, build: pathlib.Path, output: pathlib.Path
) -> tuple[pathlib.Path, pathlib.Path]:
    build = validate_cleanup_root(source, build, "build directory")
    output = validate_cleanup_root(source, output, "output directory")
    if build == output or build in output.parents or output in build.parents:
        raise RuntimeError(
            f"build and output directories must not overlap: {build}, {output}"
        )
    return build, output


def _release_root_marker_contents(root: pathlib.Path) -> str:
    return f"{RELEASE_ROOT_MARKER_VERSION}\n{root}\n"


def _require_owned_or_absent(root: pathlib.Path, label: str) -> None:
    if not root.exists():
        return
    if not root.is_dir():
        raise RuntimeError(f"refusing existing non-directory {label}: {root}")
    marker = root / RELEASE_ROOT_MARKER
    if not marker.is_file():
        raise RuntimeError(
            f"refusing existing unowned {label}: {root} "
            f"(missing {RELEASE_ROOT_MARKER})"
        )
    expected = _release_root_marker_contents(root)
    if marker.read_text(encoding="utf-8") != expected:
        raise RuntimeError(f"refusing {label} with invalid ownership marker: {root}")


def _reset_owned_root(root: pathlib.Path) -> None:
    if not root.exists():
        root.mkdir(parents=True)
        (root / RELEASE_ROOT_MARKER).write_text(
            _release_root_marker_contents(root), encoding="utf-8"
        )
        return

    for child in root.iterdir():
        if child.name == RELEASE_ROOT_MARKER:
            continue
        if child.is_symlink() or child.is_file():
            child.unlink()
        else:
            shutil.rmtree(child)


def prepare_release_directories(
    source: pathlib.Path, build: pathlib.Path, output: pathlib.Path
) -> tuple[pathlib.Path, pathlib.Path]:
    build, output = validate_release_directories(source, build, output)
    # Check ownership of both roots before deleting anything from either one.
    _require_owned_or_absent(build, "build directory")
    _require_owned_or_absent(output, "output directory")
    _reset_owned_root(build)
    _reset_owned_root(output)
    return build, output


def require_clean(
    source: pathlib.Path,
    include_untracked: bool,
    allowed_untracked_roots: tuple[pathlib.Path, ...] = (),
) -> None:
    status = git(
        source,
        "status",
        "--porcelain=v1",
        "--untracked-files=no",
        "--ignore-submodules=none",
    )
    if status:
        raise RuntimeError(f"refusing dirty source tree:\n{status}")
    if not include_untracked:
        return

    source = source.resolve()
    allowed = []
    for candidate in allowed_untracked_roots:
        root = candidate.resolve()
        if root == source or root in source.parents:
            raise RuntimeError(f"allowed untracked root contains source tree: {root}")
        try:
            root.relative_to(source)
        except ValueError:
            continue
        allowed.append(root)

    untracked = subprocess.check_output(
        ["git", "-C", str(source), "ls-files", "--others", "--exclude-standard", "-z"]
    ).split(b"\0")
    unexpected = []
    for raw_path in untracked:
        if not raw_path:
            continue
        relative = pathlib.PurePosixPath(raw_path.decode("utf-8", errors="surrogateescape"))
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError(f"unsafe untracked Git path: {relative}")
        path = source.joinpath(*relative.parts)
        if any(path == root or root in path.parents for root in allowed):
            continue
        unexpected.append(relative.as_posix())
    if unexpected:
        raise RuntimeError(
            "refusing untracked source files:\n" + "\n".join(f"?? {path}" for path in unexpected)
        )


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
    parser.add_argument("--archive", type=pathlib.Path)
    parser.add_argument("--check-source-only", action="store_true")
    parser.add_argument("--firmware-tests-required", action="store_true")
    parser.add_argument("--expected-source-tuple")
    parser.add_argument("--allow-untracked-root", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--validate-build-root", type=pathlib.Path)
    parser.add_argument("--validate-output-root", type=pathlib.Path)
    parser.add_argument("--prepare-build-root", type=pathlib.Path)
    parser.add_argument("--prepare-output-root", type=pathlib.Path)
    args = parser.parse_args()

    source = args.source.resolve()
    if args.validate_build_root is not None or args.validate_output_root is not None:
        if args.validate_build_root is None or args.validate_output_root is None:
            parser.error("--validate-build-root and --validate-output-root are required together")
        validate_release_directories(source, args.validate_build_root, args.validate_output_root)
        return
    if args.prepare_build_root is not None or args.prepare_output_root is not None:
        if args.prepare_build_root is None or args.prepare_output_root is None:
            parser.error("--prepare-build-root and --prepare-output-root are required together")
        prepare_release_directories(source, args.prepare_build_root, args.prepare_output_root)
        return
    require_clean(
        source,
        include_untracked=args.check_source_only,
        allowed_untracked_roots=tuple(args.allow_untracked_root),
    )
    commits = source_tuple(source)
    expected_commits = None
    if args.expected_source_tuple is not None:
        expected_commits = json.loads(args.expected_source_tuple)
        if expected_commits != commits:
            raise RuntimeError(
                "source/dependency commits changed before the build receipt was written: "
                f"expected {expected_commits}, found {commits}"
            )
    if args.check_source_only:
        print(json.dumps(commits, sort_keys=True))
        return
    if args.output is None or not args.artifact or args.archive is None:
        parser.error("--output, --archive, and at least one --artifact are required when writing a receipt")

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

    archive = args.archive.resolve()
    require_clean(source, include_untracked=True, allowed_untracked_roots=tuple(args.allow_untracked_root))
    final_commits = source_tuple(source)
    if final_commits != commits or (expected_commits is not None and final_commits != expected_commits):
        raise RuntimeError(
            "source/dependency commits changed while hashing release artifacts: "
            f"started with {commits}, finished with {final_commits}"
        )
    receipt = {
        "schema": "gearmulator-elektron-macos-build-v1",
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "configuration": "Release",
        "architecture": "x86_64 arm64",
        "signing": "ad-hoc",
        "notarized": False,
        **final_commits,
        "source_tree_clean": True,
        "firmware_included": False,
        "tests_run": True,
        "firmware_tests_required": args.firmware_tests_required,
        "packaged_firmware_smoke": {
            "required": args.firmware_tests_required,
            "fixture_format": "external complete 8 MiB .bin",
            "fixture_sha256": {
                "md": "68542e30917b9918ccaee2b2237df62c8a00479938680b85aca93ce4fbca44c8",
                "mm": "369849175602e20a9dd2b6e0ad8ac404b76f82718b14afbf1cbc01b7acabec7e",
            },
            "audio_callback_blocks": 256 if args.firmware_tests_required else 16,
            "scope": "exact extracted VST3 load, device initialization, and callback execution",
        },
        "archive": {
            "name": archive.name,
            "bytes": archive.stat().st_size,
            "sha256": sha256(archive),
        },
        "artifacts": artifacts,
    }
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
