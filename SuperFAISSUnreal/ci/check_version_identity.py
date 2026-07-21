#!/usr/bin/env python3
"""Coherence check 2 — version identity.

Five sources of truth must agree on one version number:
  1. the vendored core's include/superfaiss/version.h macros
  2. the vendored core's CHANGELOG.md top entry
  3. the plugin's .uplugin VersionName
  4. the plugin's own CHANGELOG.md top entry
  5. the git tag on HEAD (when one exists)

By default a tag on HEAD is required to agree; pass --allow-untagged for a
pre-release run (a PR or a commit before the release tag has been cut) where
the first four sources are still checked but an absent tag is not itself a
failure. A tag that DOES exist and disagrees is always a failure either way.

Usage:
    check_version_identity.py --plugin-root <path> [--allow-untagged]

Exit codes: 0 = every present source agrees; 1 = disagreement, OR fewer than
four sources were readable (a check that can pass without reading its
sources is not a check).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

CHANGELOG_TOP_RE = re.compile(r"^##\s*\[(\d+\.\d+\.\d+)\]", re.MULTILINE)


def read_version_h(path: Path) -> str | None:
    if not path.is_file():
        return None
    text = path.read_text(encoding="utf-8")
    major = re.search(r"SUPERFAISS_VERSION_MAJOR\s+(\d+)", text)
    minor = re.search(r"SUPERFAISS_VERSION_MINOR\s+(\d+)", text)
    patch = re.search(r"SUPERFAISS_VERSION_PATCH\s+(\d+)", text)
    if not (major and minor and patch):
        return None
    return f"{major.group(1)}.{minor.group(1)}.{patch.group(1)}"


def read_changelog_top(path: Path) -> str | None:
    if not path.is_file():
        return None
    m = CHANGELOG_TOP_RE.search(path.read_text(encoding="utf-8"))
    return m.group(1) if m else None


def read_uplugin_version(path: Path) -> str | None:
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    return data.get("VersionName")


def read_head_tag(repo_root: Path) -> str | None:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_root), "tag", "--points-at", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except subprocess.CalledProcessError:
        return None
    if not out:
        return None
    for line in out.splitlines():
        line = line.strip()
        if re.match(r"^v?\d+\.\d+\.\d+$", line):
            return line.lstrip("v")
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-root", required=True, type=Path)
    ap.add_argument("--allow-untagged", action="store_true")
    args = ap.parse_args()

    vendored = args.plugin_root / "Source" / "ThirdParty" / "SuperFAISS"

    sources: dict[str, str | None] = {
        "core version.h": read_version_h(vendored / "include" / "superfaiss" / "version.h"),
        "core CHANGELOG.md top entry": read_changelog_top(vendored / "CHANGELOG.md"),
        "plugin .uplugin VersionName": read_uplugin_version(args.plugin_root / "SuperFAISSUnreal.uplugin"),
        "plugin CHANGELOG.md top entry": read_changelog_top(args.plugin_root / "CHANGELOG.md"),
    }

    unreadable = [name for name, v in sources.items() if v is None]
    readable = {name: v for name, v in sources.items() if v is not None}

    if len(readable) < 4:
        print("FAIL: version-identity check could not read all four required sources — "
              "this is a failure, not a pass:")
        for name in unreadable:
            print(f"  - unreadable: {name}")
        return 1

    tag = read_head_tag(args.plugin_root)
    if tag is not None:
        sources["git tag on HEAD"] = tag
    elif not args.allow_untagged:
        print("FAIL: version-identity check found no version-shaped git tag on HEAD "
              "(pass --allow-untagged for a pre-release run).")
        return 1

    values = {v for v in sources.values() if v is not None}
    if len(values) > 1:
        print(f"FAIL: version-identity check — sources disagree:")
        for name, v in sources.items():
            print(f"  - {name}: {v}")
        return 1

    agreed = next(iter(values))
    print(f"OK: version-identity check — all {len(sources)} source(s) agree on {agreed}.")
    for name, v in sources.items():
        print(f"  - {name}: {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
