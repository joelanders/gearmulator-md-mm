#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

import write_mdmm_receipt as receipt


class ReceiptPathSafetyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        self.git("init", "--quiet")
        self.git("config", "user.email", "release-test@example.invalid")
        self.git("config", "user.name", "Release Test")
        (self.source / "tracked.txt").write_text("tracked\n", encoding="utf-8")
        (self.source / ".gitignore").write_text("ignored-build\n", encoding="utf-8")
        tracked_dir = self.source / "tracked-dir"
        tracked_dir.mkdir()
        (tracked_dir / "input.txt").write_text("input\n", encoding="utf-8")
        self.git("add", ".gitignore", "tracked.txt", "tracked-dir/input.txt")
        self.git("commit", "--quiet", "-m", "fixture")

    def git(self, *args: str) -> None:
        subprocess.run(
            ["git", "-C", str(self.source), *args],
            check=True,
            stdout=subprocess.DEVNULL,
        )

    def git_output(self, *args: str) -> str:
        return subprocess.check_output(
            ["git", "-C", str(self.source), *args], text=True
        ).strip()

    def test_exact_untracked_package_and_archive_are_allowed(self) -> None:
        package = self.source / "artifacts" / "package"
        package.mkdir(parents=True)
        (package / "plugin.bin.test").write_text("plugin\n", encoding="utf-8")
        archive = self.source / "artifacts" / "package.zip"
        archive.write_text("archive\n", encoding="utf-8")

        receipt.require_clean(self.source, True, (package, archive))

    def test_unexpected_output_sibling_is_rejected(self) -> None:
        package = self.source / "artifacts" / "package"
        package.mkdir(parents=True)
        (package / "plugin").write_text("plugin\n", encoding="utf-8")
        (package.parent / "unexpected.syx").write_text("fixture\n", encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "unexpected.syx"):
            receipt.require_clean(self.source, True, (package,))

    def test_source_or_ancestor_allow_root_is_rejected(self) -> None:
        for root in (self.source, self.source.parent, self.source / "artifacts" / ".."):
            with self.subTest(root=root), self.assertRaisesRegex(RuntimeError, "contains source tree"):
                receipt.require_clean(self.source, True, (root,))

    def test_untracked_symlink_cannot_borrow_allowed_target(self) -> None:
        package = self.source / "artifacts" / "package"
        package.mkdir(parents=True)
        target = package / "plugin"
        target.write_text("plugin\n", encoding="utf-8")
        (self.source / "outside-link").symlink_to(target)

        with self.assertRaisesRegex(RuntimeError, "outside-link"):
            receipt.require_clean(self.source, True, (package,))

    def test_tracked_changes_remain_rejected(self) -> None:
        (self.source / "tracked.txt").write_text("changed\n", encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "dirty source tree"):
            receipt.require_clean(self.source, True, (self.source / "artifacts",))

    def test_cleanup_root_rejects_source_ancestor_and_tracked_content(self) -> None:
        for root in (self.source, self.source.parent, self.source / "tracked-dir"):
            with self.subTest(root=root), self.assertRaises(RuntimeError):
                receipt.validate_cleanup_root(self.source, root, "test directory")

    def test_cleanup_root_rejects_git_metadata(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "Git metadata"):
            receipt.validate_cleanup_root(self.source, self.source / ".git" / "release", "test directory")

    def test_cleanup_root_rejects_tracked_submodule_interior(self) -> None:
        commit = self.git_output("rev-parse", "HEAD")
        self.git("update-index", "--add", "--cacheinfo", f"160000,{commit},vendor")

        with self.assertRaisesRegex(RuntimeError, "inside tracked submodule"):
            receipt.validate_cleanup_root(self.source, self.source / "vendor" / "build", "test directory")

    def test_cleanup_root_rejects_tracked_symlink_before_resolving_it(self) -> None:
        external = self.root / "external"
        external.mkdir()
        link = self.source / "tracked-output-link"
        link.symlink_to(external, target_is_directory=True)
        self.git("add", "tracked-output-link")

        with self.assertRaisesRegex(RuntimeError, "contains tracked path"):
            receipt.validate_cleanup_root(self.source, link, "test directory")

    def test_cleanup_root_rejects_ignored_source_symlink(self) -> None:
        external = self.root / "external"
        external.mkdir()
        link = self.source / "ignored-build"
        link.symlink_to(external, target_is_directory=True)

        with self.assertRaisesRegex(RuntimeError, "follows a source-tree symlink"):
            receipt.validate_cleanup_root(self.source, link, "test directory")

    def test_release_directories_must_not_overlap(self) -> None:
        output = self.source / "artifacts"
        build = output / "build"

        with self.assertRaisesRegex(RuntimeError, "must not overlap"):
            receipt.validate_release_directories(self.source, build, output)

    def test_safe_untracked_release_directories_are_accepted(self) -> None:
        build = self.source / "build" / "release"
        output = self.source / "artifacts" / "release"

        self.assertEqual(
            receipt.validate_release_directories(self.source, build, output),
            (build, output),
        )

    def test_prepare_creates_owned_release_roots(self) -> None:
        build = self.root / "build"
        output = self.root / "output"

        receipt.prepare_release_directories(self.source, build, output)

        for root in (build, output):
            marker = root / receipt.RELEASE_ROOT_MARKER
            self.assertEqual(
                marker.read_text(encoding="utf-8"),
                receipt._release_root_marker_contents(root),
            )

    def test_owned_markers_can_be_the_only_allowed_untracked_files(self) -> None:
        build = self.source / "release-build"
        output = self.source / "release-output"
        receipt.prepare_release_directories(self.source, build, output)

        receipt.require_clean(
            self.source,
            include_untracked=True,
            allowed_untracked_roots=(
                build / receipt.RELEASE_ROOT_MARKER,
                output / receipt.RELEASE_ROOT_MARKER,
            ),
        )

    def test_prepare_refuses_unowned_existing_root_without_deleting_it(self) -> None:
        build = self.root / "build"
        output = self.root / "output"
        build.mkdir()
        output.mkdir()
        sentinel = output / "human-file.txt"
        sentinel.write_text("keep me\n", encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "unowned build directory"):
            receipt.prepare_release_directories(self.source, build, output)

        self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep me\n")

    def test_prepare_checks_both_roots_before_deleting_either(self) -> None:
        build = self.root / "build"
        output = self.root / "output"
        receipt.prepare_release_directories(self.source, build, output)
        build_sentinel = build / "old-build.txt"
        build_sentinel.write_text("keep until both roots pass\n", encoding="utf-8")
        (output / receipt.RELEASE_ROOT_MARKER).unlink()

        with self.assertRaisesRegex(RuntimeError, "unowned output directory"):
            receipt.prepare_release_directories(self.source, build, output)

        self.assertTrue(build_sentinel.is_file())

    def test_prepare_resets_owned_roots_and_preserves_markers(self) -> None:
        build = self.root / "build"
        output = self.root / "output"
        receipt.prepare_release_directories(self.source, build, output)
        (build / "old-file").write_text("old\n", encoding="utf-8")
        old_dir = output / "old-dir"
        old_dir.mkdir()
        (old_dir / "old-file").write_text("old\n", encoding="utf-8")

        receipt.prepare_release_directories(self.source, build, output)

        for root in (build, output):
            self.assertEqual(
                {path.name for path in root.iterdir()},
                {receipt.RELEASE_ROOT_MARKER},
            )

    def test_prepare_rejects_marker_copied_from_another_root(self) -> None:
        build = self.root / "build"
        output = self.root / "output"
        receipt.prepare_release_directories(self.source, build, output)
        copied = self.root / "copied-build"
        copied.mkdir()
        (copied / receipt.RELEASE_ROOT_MARKER).write_text(
            (build / receipt.RELEASE_ROOT_MARKER).read_text(encoding="utf-8"),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "invalid ownership marker"):
            receipt.prepare_release_directories(self.source, copied, output)


if __name__ == "__main__":
    unittest.main()
