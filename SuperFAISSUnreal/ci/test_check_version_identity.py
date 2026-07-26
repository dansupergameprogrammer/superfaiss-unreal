#!/usr/bin/env python3
"""Test harness for check_version_identity.py.

Builds throwaway git repositories in a temp directory and runs the real
script against them as a subprocess (the same way the coherence-checks and
release workflows invoke it), so the tests exercise the actual `git tag
--points-at HEAD` behavior rather than a mocked stand-in. A second, smaller
set of tests calls `read_head_tag` directly for precise coverage of its
return shape.

Covers:
  - accepted tag format: plain `vX.Y.Z` and bare `X.Y.Z` both resolve.
    A `SuperFAISSUnreal-v3.3.1`-style prefixed form is deliberately NOT
    tested — this repository does not use that convention (every tag, local
    and remote, is plain `vX.Y.Z`), and asserting a convention the project
    does not use would be worse than no test.
  - normal paths: a matching tag passes; a tag that disagrees with the
    plugin line fails; no tag fails by default and passes under
    --allow-untagged.
  - the conflict case: two distinct version-shaped tags on HEAD fail
    explicitly, in both the default mode and --allow-untagged. An absent tag
    and a contradictory pair are different things, and --allow-untagged
    forgives only the absent-tag case.
  - two tags that normalize to the *same* version (e.g. "v1.0.0" and
    "1.0.0" both on HEAD) are NOT treated as a conflict, because
    `read_head_tag` dedups by normalized value, not by raw tag string. This
    is asserted as correct: the check exists to catch two DIFFERENT
    versions claimed for the same commit, not two spellings of one version.

Not covered: the plugin-behind-core failure path, and the <4-readable-
sources failure path. Both predate this hardening pass and are unrelated to
the read_head_tag defect this harness was written to pin; they are not
exercised here.

Run directly:
    python ci/test_check_version_identity.py
or via unittest/pytest discovery from the ci/ directory.
"""
from __future__ import annotations

import importlib.util
import json
import shutil
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "check_version_identity.py"


def _load_module() -> types.ModuleType:
    spec = importlib.util.spec_from_file_location("check_version_identity_under_test", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


MODULE = _load_module()


def _rmtree_force(path: Path) -> None:
    """shutil.rmtree that clears the read-only bit git sets on some objects (Windows)."""

    def _on_error(func, target, exc_info):  # noqa: ANN001 - shutil onerror signature
        try:
            Path(target).chmod(stat.S_IWRITE)
            func(target)
        except OSError:
            pass

    shutil.rmtree(path, onerror=_on_error)


def _run_git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True, text=True, check=True,
    )


def _init_repo(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    _run_git(root, "init", "-q")
    _run_git(root, "config", "user.email", "test@example.invalid")
    _run_git(root, "config", "user.name", "Test")
    _run_git(root, "config", "core.autocrlf", "false")


def _write_plugin_tree(
    root: Path,
    plugin_version: str,
    plugin_changelog_version: str,
    core_version: str,
    core_changelog_version: str,
) -> None:
    (root / "SuperFAISSUnreal.uplugin").write_text(
        json.dumps({"FriendlyName": "SuperFAISSUnreal", "VersionName": plugin_version}),
        encoding="utf-8",
    )
    (root / "CHANGELOG.md").write_text(
        f"# Changelog\n\n## [{plugin_changelog_version}] - 2026-01-01\n- entry\n",
        encoding="utf-8",
    )
    vendored = root / "Source" / "ThirdParty" / "SuperFAISS"
    (vendored / "include" / "superfaiss").mkdir(parents=True, exist_ok=True)
    major, minor, patch = core_version.split(".")
    (vendored / "include" / "superfaiss" / "version.h").write_text(
        "#pragma once\n"
        f"#define SUPERFAISS_VERSION_MAJOR {major}\n"
        f"#define SUPERFAISS_VERSION_MINOR {minor}\n"
        f"#define SUPERFAISS_VERSION_PATCH {patch}\n",
        encoding="utf-8",
    )
    (vendored / "CHANGELOG.md").write_text(
        f"# Changelog\n\n## [{core_changelog_version}] - 2026-01-01\n- entry\n",
        encoding="utf-8",
    )


def _commit_all(root: Path) -> None:
    _run_git(root, "add", "-A")
    _run_git(root, "commit", "-q", "-m", "test commit")


def _tag(root: Path, name: str) -> None:
    _run_git(root, "tag", name)


def _run_check(root: Path, allow_untagged: bool = False) -> subprocess.CompletedProcess:
    args = [sys.executable, str(SCRIPT), "--plugin-root", str(root)]
    if allow_untagged:
        args.append("--allow-untagged")
    return subprocess.run(args, capture_output=True, text=True)


class _RepoTestCase(unittest.TestCase):
    """Base class: gives each test its own throwaway repo under a scratch temp dir."""

    def setUp(self) -> None:
        self._scratch = Path(tempfile.mkdtemp(prefix="check_version_identity_test_"))
        self.addCleanup(lambda: _rmtree_force(self._scratch))

    def make_repo(self, version: str = "1.2.3") -> Path:
        """A repo whose four non-tag sources all agree on `version`, committed once."""
        root = self._scratch / "plugin"
        _init_repo(root)
        _write_plugin_tree(root, version, version, version, version)
        _commit_all(root)
        return root


class AcceptedTagFormatTests(_RepoTestCase):
    """Plain vX.Y.Z and bare X.Y.Z both resolve — the two forms this repo actually uses."""

    def test_v_prefixed_tag_resolves(self):
        root = self.make_repo("1.2.3")
        _tag(root, "v1.2.3")
        result = _run_check(root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("OK", result.stdout)

    def test_bare_tag_resolves(self):
        root = self.make_repo("1.2.3")
        _tag(root, "1.2.3")
        result = _run_check(root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("OK", result.stdout)


class NormalPathTests(_RepoTestCase):
    """Matching tag passes; disagreeing tag fails; absent tag fails only without the flag."""

    def test_matching_tag_passes(self):
        root = self.make_repo("2.0.0")
        _tag(root, "v2.0.0")
        result = _run_check(root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_disagreeing_tag_fails_default_mode(self):
        root = self.make_repo("2.0.0")
        _tag(root, "v9.9.9")
        result = _run_check(root)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("disagree", result.stdout)

    def test_disagreeing_tag_fails_allow_untagged_mode(self):
        # --allow-untagged forgives an ABSENT tag, never a tag that exists and disagrees.
        root = self.make_repo("2.0.0")
        _tag(root, "v9.9.9")
        result = _run_check(root, allow_untagged=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("disagree", result.stdout)

    def test_no_tag_fails_by_default(self):
        root = self.make_repo("2.0.0")
        result = _run_check(root)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no version-shaped git tag", result.stdout)

    def test_no_tag_passes_with_allow_untagged(self):
        root = self.make_repo("2.0.0")
        result = _run_check(root, allow_untagged=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class ConflictCaseTests(_RepoTestCase):
    """Two DISTINCT version tags on HEAD fail explicitly, in both modes."""

    def test_two_distinct_tags_fail_default_mode(self):
        root = self.make_repo("1.0.0")
        _tag(root, "v1.0.0")
        _tag(root, "v1.0.1")
        result = _run_check(root)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("multiple distinct version-shaped git tags", result.stdout)

    def test_two_distinct_tags_fail_allow_untagged_mode(self):
        # The sharpest case: --allow-untagged forgives a MISSING tag, and this repo
        # has the opposite problem (too many, not zero) — it must still fail.
        root = self.make_repo("1.0.0")
        _tag(root, "v1.0.0")
        _tag(root, "v1.0.1")
        result = _run_check(root, allow_untagged=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("multiple distinct version-shaped git tags", result.stdout)

    def test_two_tags_naming_the_same_version_are_not_a_conflict(self):
        # "v1.0.0" and "1.0.0" both point at HEAD but normalize to the identical
        # version string. This is a duplicate spelling of one fact, not two
        # contradictory facts, so it must NOT trip the multiple-distinct-tags
        # failure — asserted here as the correct behavior, not merely the
        # observed one: the check exists to catch two DIFFERENT versions
        # claimed for the same commit.
        root = self.make_repo("1.0.0")
        _tag(root, "v1.0.0")
        _tag(root, "1.0.0")
        result = _run_check(root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("multiple distinct version-shaped git tags", result.stdout)


class ReadHeadTagUnitTests(_RepoTestCase):
    """Direct coverage of read_head_tag's (tag, all_tags) return shape."""

    def test_no_tags_returns_none_and_empty_list(self):
        root = self.make_repo("1.0.0")
        tag, all_tags = MODULE.read_head_tag(root)
        self.assertIsNone(tag)
        self.assertEqual(all_tags, [])

    def test_single_tag_returns_it_in_both_slots(self):
        root = self.make_repo("1.0.0")
        _tag(root, "v1.0.0")
        tag, all_tags = MODULE.read_head_tag(root)
        self.assertEqual(tag, "1.0.0")
        self.assertEqual(all_tags, ["1.0.0"])

    def test_two_distinct_tags_both_appear_in_all_tags(self):
        root = self.make_repo("1.0.0")
        _tag(root, "v1.0.0")
        _tag(root, "v1.0.1")
        tag, all_tags = MODULE.read_head_tag(root)
        # tag is deterministically the first of the sorted distinct set — callers must
        # check len(all_tags) > 1 themselves rather than trust `tag` alone; this is
        # exactly the defect this hardening pass closed.
        self.assertEqual(tag, "1.0.0")
        self.assertEqual(all_tags, ["1.0.0", "1.0.1"])

    def test_two_tags_same_normalized_version_dedup_to_one(self):
        root = self.make_repo("1.0.0")
        _tag(root, "v1.0.0")
        _tag(root, "1.0.0")
        tag, all_tags = MODULE.read_head_tag(root)
        self.assertEqual(tag, "1.0.0")
        self.assertEqual(all_tags, ["1.0.0"])


if __name__ == "__main__":
    unittest.main()
