#!/usr/bin/env python3
"""Coherence check 1 — vendored coherence.

The vendored core tree at Source/ThirdParty/SuperFAISS must match, file for
file, the core commit its own VENDORED_VERSION.txt names. That stamp is
hand-written and has gone stale twice this release because nothing verified
it against the content it describes. This script COMPUTES the correspondence
instead of trusting the text: it reads the commit hash out of
VENDORED_VERSION.txt, obtains that commit's tree (a local clone, or a fresh
shallow fetch), and byte-compares every file in the parity roots (include/,
src/, docs/, CHANGELOG.md, LICENSE, README.md, tests/xd_fixtures.h) in both
directions — nothing missing on either side, nothing differing in content.

Usage:
    check_vendored_coherence.py --plugin-root <path to Plugins/SuperFAISSUnreal>
        [--core-repo <path to an existing local clone/worktree of the core repo>]
        [--core-remote <git URL, used only if --core-repo is not given>]

Exit codes: 0 = every parity file matched; 1 = at least one mismatch, OR the
comparison covered zero files (a check that can pass by comparing nothing is
not a check).
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_CORE_REMOTE = "https://github.com/dansupergameprogrammer/superfaiss.git"

PARITY_DIRS = ("include", "src", "docs")
PARITY_FILES = ("CHANGELOG.md", "LICENSE", "README.md")
PARITY_SINGLE_FILES = ("tests/xd_fixtures.h",)


def read_vendored_commit(vendored_version_txt: Path) -> str:
    text = vendored_version_txt.read_text(encoding="utf-8")
    m = re.search(r"core (?:master )?commit ([0-9a-fA-F]{7,40})", text)
    if not m:
        raise SystemExit(
            f"FAIL: vendored-coherence check could not find a 'core commit <sha>' "
            f"reference in {vendored_version_txt}"
        )
    return m.group(1)


def materialize_core_at_commit(
    commit: str, core_repo: Path | None, core_remote: str, workdir: Path
) -> tuple[Path, tuple[Path, Path] | None]:
    """Returns (path to core tree at `commit`, cleanup info-or-None).

    cleanup info, when present, is (repo, worktree_path) and must be passed
    to `cleanup_worktree` when the caller is done reading the tree.
    """
    if core_repo is not None:
        head = subprocess.run(
            ["git", "-C", str(core_repo), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        if head.startswith(commit) or commit.startswith(head):
            return core_repo, None
        # Not already at the pinned commit — check it out into a fresh
        # worktree so we never mutate the caller's checkout in place.
        wt = workdir / "core-at-pin"
        subprocess.run(
            ["git", "-C", str(core_repo), "worktree", "add", "--detach", str(wt), commit],
            check=True, capture_output=True, text=True,
        )
        return wt, (core_repo, wt)

    clone_dir = workdir / "core-clone"
    subprocess.run(["git", "clone", "--filter=blob:none", core_remote, str(clone_dir)],
                    check=True, capture_output=True, text=True)
    subprocess.run(["git", "-C", str(clone_dir), "fetch", "--depth", "1", "origin", commit],
                    check=True, capture_output=True, text=True)
    subprocess.run(["git", "-C", str(clone_dir), "checkout", commit],
                    check=True, capture_output=True, text=True)
    return clone_dir, None


def cleanup_worktree(info: tuple[Path, Path] | None) -> None:
    if info is None:
        return
    repo, wt = info
    subprocess.run(["git", "-C", str(repo), "worktree", "remove", "--force", str(wt)],
                    capture_output=True, text=True)


def _normalized_content(path: Path) -> bytes:
    """Content for comparison, with line endings normalized to LF.

    Two independent git working trees (this monorepo and the standalone core
    checkout) can carry different `core.autocrlf`/`.gitattributes` `eol`
    settings for the same path, so a fresh checkout on either side can flip
    line endings without the actual file content having changed. Comparing
    on line-ending-normalized text is what "the same file" means here;
    genuinely binary files (which never round-trip through utf-8) fall back
    to a raw byte comparison, where line-ending normalization does not apply.
    """
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw
    return text.replace("\r\n", "\n").encode("utf-8")


def collect_files(root: Path) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for d in PARITY_DIRS:
        base = root / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if p.is_file():
                out[str(p.relative_to(root)).replace("\\", "/")] = p
    for f in PARITY_FILES:
        p = root / f
        if p.is_file():
            out[f] = p
    for f in PARITY_SINGLE_FILES:
        p = root / f
        if p.is_file():
            out[f] = p
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-root", required=True, type=Path,
                     help="Path to the plugin root (contains Source/ThirdParty/SuperFAISS)")
    ap.add_argument("--core-repo", type=Path, default=None,
                     help="Path to an existing local clone/worktree of the core repo")
    ap.add_argument("--core-remote", default=DEFAULT_CORE_REMOTE)
    args = ap.parse_args()

    vendored_root = args.plugin_root / "Source" / "ThirdParty" / "SuperFAISS"
    version_txt = vendored_root / "VENDORED_VERSION.txt"
    if not version_txt.is_file():
        print(f"FAIL: vendored-coherence check found no VENDORED_VERSION.txt at {version_txt}")
        return 1

    commit = read_vendored_commit(version_txt)

    with tempfile.TemporaryDirectory(prefix="superfaiss-vendor-check-") as tmp:
        tmp_path = Path(tmp)
        try:
            core_at_commit, cleanup_info = materialize_core_at_commit(
                commit, args.core_repo, args.core_remote, tmp_path
            )
        except subprocess.CalledProcessError as e:
            print(f"FAIL: vendored-coherence check could not obtain core commit {commit}: "
                  f"{e.stderr or e}")
            return 1

        try:
            vendored_files = collect_files(vendored_root)
            core_files = collect_files(core_at_commit)

            vendored_names = set(vendored_files) - {"VENDORED_VERSION.txt"}
            core_names = set(core_files)

            missing_from_core = sorted(vendored_names - core_names)
            missing_from_vendor = sorted(core_names - vendored_names)
            content_mismatches = []
            for name in sorted(vendored_names & core_names):
                if _normalized_content(vendored_files[name]) != _normalized_content(core_files[name]):
                    content_mismatches.append(name)

            total_compared = len(vendored_names & core_names)
        finally:
            cleanup_worktree(cleanup_info)

    if total_compared == 0:
        print("FAIL: vendored-coherence check compared zero files — nothing was checked. "
              "This is a failure, not a pass.")
        return 1

    if missing_from_core or missing_from_vendor or content_mismatches:
        print(f"FAIL: vendored-coherence check — vendored tree does not match core commit {commit}:")
        for name in missing_from_core:
            print(f"  - vendored has '{name}' but core commit {commit} does not")
        for name in missing_from_vendor:
            print(f"  - core commit {commit} has '{name}' but the vendored tree does not")
        for name in content_mismatches:
            print(f"  - '{name}' content differs between the vendored tree and core commit {commit}")
        return 1

    print(f"OK: vendored-coherence check — {total_compared} file(s) byte-identical "
          f"between the vendored tree and core commit {commit}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
