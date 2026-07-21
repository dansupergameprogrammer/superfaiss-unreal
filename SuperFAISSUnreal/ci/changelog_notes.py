#!/usr/bin/env python3
"""Print the top dated entry's body from a Keep-a-Changelog file.

Used by release.yml to fill in the GitHub Release notes from
SuperFAISSUnreal/CHANGELOG.md's top `## [X.Y.Z] - date` section, so release
notes are never hand-retyped out of sync with the changelog.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("changelog", type=Path)
    args = ap.parse_args()

    text = args.changelog.read_text(encoding="utf-8")
    m = re.search(r"^## \[.*?\].*?\n(.*?)(?=\n## \[|\Z)", text, re.MULTILINE | re.DOTALL)
    if not m:
        print(f"changelog_notes: no top '## [x.y.z]' entry found in {args.changelog}", file=sys.stderr)
        return 1
    print(m.group(1).strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
