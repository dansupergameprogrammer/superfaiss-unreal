#!/usr/bin/env python3
"""Coherence check — repealed-phrase sweep.

A repealed contract, once corrected, tends to survive somewhere else in the
tree — a second doc, an older comment, a CHANGELOG's own claim that the scrub
already happened. This is the structural form of that sweep: a short list of
phrases that must never appear in HEAD again, checked everywhere at once
(code, docs, comments, workflow YAML), so the class of bug does not need a
human to remember to grep for it a third time.

Two families are seeded:
  - Repealed technical contracts (a promise the code no longer keeps). Seed:
    the v3.0 channel-table phrasing that v3.1's `Relabel` repealed.
  - Private-provenance patterns that must never ship in public source: the
    private records-tree path prefix, internal reviewer/persona names, and
    internal decision-identifier shapes.

Run from anywhere; pass --root pointing at the tree to scan (a vendored core
checkout, or a plugin repo root). Text files only, matched by extension;
binary files and the `.git` directory are skipped. This script's own file is
also skipped, since it necessarily quotes every phrase it bans.

Exit codes: 0 = zero phrase hits, over at least one scanned file; 1 = at
least one hit, OR zero files were scanned (a check that can pass by finding
nothing to check is not a check).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# A console using a legacy code page (cp1252 on stock Windows terminals) can't
# encode every character this script might print (source comments quote em
# dashes, arrows, etc.). Force UTF-8 on stdout so a report never crashes
# instead of finishing — a check that dies before printing all its findings
# is worse than one that prints them oddly.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# (label, compiled pattern). Patterns are plain substrings or small regexes;
# case-sensitive — these are exact repealed strings and exact proper nouns,
# not prose that might legitimately vary in case.
_PHRASES: list[tuple[str, re.Pattern]] = [
    ("repealed channel-table contract",
     re.compile(r"fixed for the bank's lifetime")),
    ("repealed channel-table contract (channel partition variant)",
     re.compile(r"\bfixed channel (?:table|partition)\b")),
    ("private records-tree path",
     re.compile(r"\bClaude/[A-Za-z]")),
    ("internal decision identifier",
     re.compile(r"\bD-V32-\d+")),
    ("internal reviewer/persona name: Poirot", re.compile(r"\bPoirot\b")),
    ("internal reviewer/persona name: Curie", re.compile(r"\bCurie\b")),
    ("internal reviewer/persona name: Charpy", re.compile(r"\bCharpy\b")),
    ("internal reviewer/persona name: Brunel", re.compile(r"\bBrunel\b")),
    ("internal reviewer/persona name: Mendeleev", re.compile(r"\bMendeleev\b")),
    ("internal reviewer/persona name: Loki", re.compile(r"\bLoki\b")),
    ("internal reviewer/persona name: Vitruvius", re.compile(r"\bVitruvius\b")),
    ("internal reviewer/persona name: Japp", re.compile(r"\bJapp\b")),
    ("internal reviewer/persona name: Hastings", re.compile(r"\bHastings\b")),
    ("internal reviewer/persona name: Forge", re.compile(r"\bForge\b")),
]

_SCAN_EXTENSIONS = {
    ".h", ".hpp", ".hh", ".c", ".cc", ".cpp", ".cxx",
    ".md", ".yml", ".yaml", ".py", ".json", ".uplugin", ".txt",
}

_SKIP_DIR_NAMES = {".git", "out", "Binaries", "Intermediate", "DerivedDataCache", "Saved"}

# Historical-record files are explicitly exempt: a changelog's (or a vendoring
# note's) whole job is to describe what used to be true and quote the phrasing
# that was retired, in the past tense, as part of the record of the fix.
# Scrubbing an entry to remove the phrase it is reporting as fixed would
# falsify the record, which is the opposite of this check's purpose. Only
# current-truth documentation (README, API docs, code comments) is in scope.
_SKIP_FILE_NAMES = {"changelog.md", "vendored_version.txt"}


def iter_candidate_files(root: Path, self_path: Path) -> list[Path]:
    files: list[Path] = []
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        if p.suffix.lower() not in _SCAN_EXTENSIONS:
            continue
        if p.name.lower() in _SKIP_FILE_NAMES:
            continue
        if any(part in _SKIP_DIR_NAMES for part in p.parts):
            continue
        try:
            if p.resolve() == self_path.resolve():
                continue
        except OSError:
            pass
        files.append(p)
    return files


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, type=Path)
    args = ap.parse_args()

    if not args.root.is_dir():
        print(f"FAIL: repealed-phrase check found no directory at {args.root}")
        return 1

    self_path = Path(__file__)
    files = iter_candidate_files(args.root, self_path)

    if not files:
        print("FAIL: repealed-phrase check scanned zero files — "
              "nothing was checked. This is a failure, not a pass.")
        return 1

    hits: list[str] = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError):
            continue
        for line_no, line in enumerate(text.splitlines(), start=1):
            for label, pattern in _PHRASES:
                if pattern.search(line):
                    rel = path.relative_to(args.root)
                    hits.append(f"{rel}:{line_no}: [{label}] {line.strip()}")

    if hits:
        print(f"FAIL: repealed-phrase check — {len(hits)} hit(s) across "
              f"{len(files)} scanned file(s):")
        for h in hits:
            print(f"  - {h}")
        return 1

    print(f"OK: repealed-phrase check — {len(files)} file(s) scanned, "
          f"{len(_PHRASES)} phrase(s) checked, zero hits.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
