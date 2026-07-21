#!/usr/bin/env python3
"""Coherence check 5 — orphan sweep.

No file tracked in the public plugin repo's synced subtree may be absent
from the source-of-truth tree it syncs from. The publish step is a folder
copy that adds and overwrites but never deletes: a file removed at the
source keeps shipping in every release after, unreferenced, until something
notices.

This check needs BOTH trees, so it only makes sense where both are reachable
on the same filesystem — the self-hosted publish runner, not a hosted
GitHub-Actions runner (which only ever checks out the public repo and has no
access to the private source-of-truth worktree). Wire it into the publish
workflow on that runner, with SUPERFAISS_UNREAL_SOURCE_ROOT set.

Usage:
    check_orphan_sweep.py --source-root <path to Plugins/SuperFAISSUnreal>
                           --public-root <path to the synced folder in the public repo>

Both roots are compared via `git ls-files` (so build output and other
gitignored artifacts on either side are never treated as orphans).

Exit codes: 0 = every public-tracked file exists at the source; 1 = at least
one orphan, OR either side reported zero tracked files (a check that can
pass by comparing nothing is not a check).
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def tracked_files(root: Path) -> set[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files"],
        capture_output=True, text=True, check=True,
    ).stdout
    return {line.strip().replace("\\", "/") for line in out.splitlines() if line.strip()}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True, type=Path)
    ap.add_argument("--public-root", required=True, type=Path)
    args = ap.parse_args()

    if not args.source_root.is_dir():
        print(f"FAIL: orphan-sweep check found no source root at {args.source_root}")
        return 1
    if not args.public_root.is_dir():
        print(f"FAIL: orphan-sweep check found no public root at {args.public_root}")
        return 1

    try:
        source_files = tracked_files(args.source_root)
        public_files = tracked_files(args.public_root)
    except subprocess.CalledProcessError as e:
        print(f"FAIL: orphan-sweep check could not list tracked files: {e.stderr or e}")
        return 1

    if not source_files or not public_files:
        print("FAIL: orphan-sweep check found zero tracked files on one side — "
              "this is a failure, not a pass.")
        print(f"  - source-root tracked files: {len(source_files)}")
        print(f"  - public-root tracked files: {len(public_files)}")
        return 1

    orphans = sorted(public_files - source_files)

    if orphans:
        print(f"FAIL: orphan-sweep check — {len(orphans)} file(s) tracked in the public "
              f"repo are absent from the source-of-truth tree at {args.source_root}:")
        for o in orphans:
            print(f"  - {o}")
        return 1

    print(f"OK: orphan-sweep check — all {len(public_files)} public-tracked file(s) "
          f"exist in the source-of-truth tree ({len(source_files)} tracked there).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
