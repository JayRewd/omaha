#!/usr/bin/env python3
"""Replace UI bool OR operators with word 'or' in modern UI XML.

Rewrites `||` to `or` to match UID_EvalBool word operators.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def migrate_text(text: str) -> tuple[str, int]:
    count = text.count("||")
    return text.replace("||", "or"), count


def iter_xml_files(root: Path):
    yield from sorted(root.rglob("*.xml"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "root",
        nargs="?",
        default="assets/main/ui/modern",
        type=Path,
        help="Directory to walk (default: assets/main/ui/modern)",
    )
    ap.add_argument("-n", "--dry-run", action="store_true", help="Report only")
    args = ap.parse_args()
    root: Path = args.root
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 1

    files_changed = 0
    total = 0
    for path in iter_xml_files(root):
        text = path.read_text(encoding="utf-8")
        new_text, n = migrate_text(text)
        if n == 0:
            continue
        files_changed += 1
        total += n
        print(f"{path}: {n} replacement(s)")
        if not args.dry_run:
            path.write_text(new_text, encoding="utf-8")

    print(f"{'would change' if args.dry_run else 'changed'} {files_changed} file(s), {total} replacement(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
