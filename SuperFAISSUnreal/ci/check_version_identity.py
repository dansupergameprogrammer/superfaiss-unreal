#!/usr/bin/env python3
"""Coherence check 2 — version identity.

Two version lines must each be internally consistent, and the plugin must not
lag the core it vendors:

  Plugin line (must all agree with each other):
    - the plugin's .uplugin VersionName
    - the plugin's own CHANGELOG.md top entry
    - the git tag on HEAD (when one exists)
  Core line (must all agree with each other):
    - the vendored core's include/superfaiss/version.h macros
    - the vendored core's CHANGELOG.md top entry

  And: plugin version >= core version. The plugin tracks the vendored core but
  is NOT identical to it — a plugin-only release (e.g. an editor/inspector fix
  over an unchanged library) advances the plugin ahead of the core it vendors.
  The one thing forbidden is the plugin falling BEHIND the core, which would
  mean shipping a newer core under an older plugin version number.

By default a tag on HEAD is required to agree with the plugin line; pass
--allow-untagged for a pre-release run (a PR, or a commit before the release
tag is cut) where the other sources are still checked but an absent tag is not
itself a failure. A tag that DOES exist and disagrees with the plugin line is
always a failure.

Usage:
    check_version_identity.py --plugin-root <path> [--allow-untagged]

Exit codes: 0 = each line is internally consistent and plugin >= core;
1 = an internal disagreement, plugin behind core, OR fewer than four of the
non-tag sources were readable (a check that can pass without reading its
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


def parse_version(v: str) -> tuple[int, ...]:
    return tuple(int(x) for x in v.split("."))


def one_value(group: dict[str, str | None], label: str) -> str | None:
    """Return the single agreed value of a group, or None (and print) on disagreement."""
    values = {v for v in group.values() if v is not None}
    if len(values) > 1:
        print(f"FAIL: version-identity check — {label} sources disagree:")
        for name, v in group.items():
            if v is not None:
                print(f"  - {name}: {v}")
        return None
    return next(iter(values)) if values else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-root", required=True, type=Path)
    ap.add_argument("--allow-untagged", action="store_true")
    args = ap.parse_args()

    vendored = args.plugin_root / "Source" / "ThirdParty" / "SuperFAISS"

    plugin_sources: dict[str, str | None] = {
        "plugin .uplugin VersionName": read_uplugin_version(args.plugin_root / "SuperFAISSUnreal.uplugin"),
        "plugin CHANGELOG.md top entry": read_changelog_top(args.plugin_root / "CHANGELOG.md"),
    }
    core_sources: dict[str, str | None] = {
        "core version.h": read_version_h(vendored / "include" / "superfaiss" / "version.h"),
        "core CHANGELOG.md top entry": read_changelog_top(vendored / "CHANGELOG.md"),
    }

    named = {**plugin_sources, **core_sources}
    unreadable = [name for name, v in named.items() if v is None]
    if sum(1 for v in named.values() if v is not None) < 4:
        print("FAIL: version-identity check could not read all four required sources — "
              "this is a failure, not a pass:")
        for name in unreadable:
            print(f"  - unreadable: {name}")
        return 1

    # The git tag, when present, joins the plugin line.
    tag = read_head_tag(args.plugin_root)
    if tag is not None:
        plugin_sources["git tag on HEAD"] = tag
    elif not args.allow_untagged:
        print("FAIL: version-identity check found no version-shaped git tag on HEAD "
              "(pass --allow-untagged for a pre-release run).")
        return 1

    plugin_ver = one_value(plugin_sources, "plugin")
    core_ver = one_value(core_sources, "core")
    if plugin_ver is None or core_ver is None:
        return 1

    if parse_version(plugin_ver) < parse_version(core_ver):
        print(f"FAIL: version-identity check — plugin {plugin_ver} is BEHIND the vendored "
              f"core {core_ver}. The plugin may track ahead of the core it vendors, never lag it.")
        return 1

    rel = "==" if plugin_ver == core_ver else ">"
    print(f"OK: version-identity check — plugin {plugin_ver} {rel} core {core_ver}; "
          "each version line is internally consistent.")
    for name, v in {**plugin_sources, **core_sources}.items():
        if v is not None:
            print(f"  - {name}: {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
