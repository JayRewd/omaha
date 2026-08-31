#!/usr/bin/env python3
"""Compare HTML vs native modern-menu screenshots with optional masks.

Gate (default): for each matching basename PNG, after applying the mask
(black=ignore, white/non-black=compare), fail if more than 1% of unmasked
pixels have max|ΔRGB| > --threshold (default 30). --tolerance is reserved
for future edge-distance checks and is recorded in the report.

Exit 1 on any failing pair. Writes report.md under --out.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def _require_pillow():
    try:
        from PIL import Image  # noqa: F401
    except ImportError as exc:
        raise SystemExit(
            "ui_compare.py requires Pillow (pip install pillow)"
        ) from exc


def load_rgb(path: Path):
    from PIL import Image

    img = Image.open(path).convert("RGB")
    return img


def ensure_mask(masks_dir: Path, basename: str, size: tuple[int, int]):
    """Load mask or synthesize an L0 chrome-band mask (header + frame)."""
    from PIL import Image

    path = masks_dir / basename
    if path.is_file():
        return Image.open(path).convert("RGB")

    w, h = size
    img = Image.new("RGB", (w, h), (0, 0, 0))
    px = img.load()
    header = max(1, int(h * 0.07))
    frame = max(8, int(min(w, h) * 0.04))
    for y in range(header):
        for x in range(w):
            px[x, y] = (255, 255, 255)
    for y in range(header, min(h, header + frame)):
        for x in range(w):
            px[x, y] = (255, 255, 255)
    for y in range(header, h):
        for x in range(frame):
            px[x, y] = (255, 255, 255)
        for x in range(w - frame, w):
            px[x, y] = (255, 255, 255)
    for y in range(h - frame, h):
        for x in range(w):
            px[x, y] = (255, 255, 255)
    masks_dir.mkdir(parents=True, exist_ok=True)
    img.save(path)
    return img


def compare_pair(
    html_img,
    native_img,
    mask_img,
    threshold: int,
):
    from PIL import Image

    if html_img.size != native_img.size:
        return {
            "ok": False,
            "error": f"size mismatch html={html_img.size} native={native_img.size}",
            "fail_pct": 100.0,
            "compared": 0,
            "failed": 0,
        }
    if mask_img.size != html_img.size:
        mask_img = mask_img.resize(html_img.size, Image.NEAREST)

    w, h = html_img.size
    hp = html_img.load()
    np_ = native_img.load()
    mp = mask_img.load()

    compared = 0
    failed = 0
    heat = Image.new("RGB", (w, h), (0, 0, 0))
    heat_px = heat.load()

    for y in range(h):
        for x in range(w):
            mr, mg, mb = mp[x, y]
            if mr == 0 and mg == 0 and mb == 0:
                continue
            compared += 1
            hr, hg, hb = hp[x, y]
            nr, ng, nb = np_[x, y]
            d = max(abs(hr - nr), abs(hg - ng), abs(hb - nb))
            if d > threshold:
                failed += 1
                # heatmap: red intensity by error
                heat_px[x, y] = (min(255, d * 4), 0, 0)
            else:
                heat_px[x, y] = (0, 40, 0)

    fail_pct = (100.0 * failed / compared) if compared else 0.0
    return {
        "ok": compared > 0 and fail_pct <= 1.0,
        "fail_pct": fail_pct,
        "compared": compared,
        "failed": failed,
        "heat": heat,
        "error": None if compared else "no unmasked pixels",
    }


def write_report(out_dir: Path, rows: list[dict], gate_note: str) -> None:
    lines = [
        "# Modern menu screenshot compare",
        "",
        gate_note,
        "",
        "| Shot | Compared | Failed | Fail % | Result |",
        "|------|----------|--------|--------|--------|",
    ]
    for r in rows:
        status = "PASS" if r["ok"] else "FAIL"
        err = f" ({r['error']})" if r.get("error") else ""
        lines.append(
            f"| `{r['name']}` | {r['compared']} | {r['failed']} | "
            f"{r['fail_pct']:.3f}% | **{status}**{err} |"
        )
    lines.append("")
    (out_dir / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_self_test(tmp: Path) -> int:
    from PIL import Image

    html = tmp / "html"
    native = tmp / "native"
    masks = tmp / "masks"
    diff = tmp / "diff"
    for d in (html, native, masks, diff):
        d.mkdir(parents=True, exist_ok=True)

    # Matching pair (should pass)
    a = Image.new("RGB", (64, 64), (10, 20, 30))
    a.save(html / "pass.png")
    a.save(native / "pass.png")
    Image.new("RGB", (64, 64), (255, 255, 255)).save(masks / "pass.png")

    # Differing pair outside threshold (should fail)
    Image.new("RGB", (64, 64), (0, 0, 0)).save(html / "fail.png")
    Image.new("RGB", (64, 64), (255, 0, 0)).save(native / "fail.png")
    Image.new("RGB", (64, 64), (255, 255, 255)).save(masks / "fail.png")

    rc = main(
        [
            "--html",
            str(html),
            "--native",
            str(native),
            "--masks",
            str(masks),
            "--out",
            str(diff),
            "--threshold",
            "30",
        ]
    )
    report = (diff / "report.md").read_text(encoding="utf-8")
    if "pass.png" not in report or "fail.png" not in report:
        print("self-test: report incomplete", file=sys.stderr)
        return 1
    if rc != 1:
        print(f"self-test: expected exit 1 (one fail), got {rc}", file=sys.stderr)
        return 1
    print("self-test: ok")
    return 0


def main(argv: list[str] | None = None) -> int:
    _require_pillow()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--html", type=Path, help="Directory of HTML reference PNGs")
    parser.add_argument("--native", type=Path, help="Directory of native capture PNGs")
    parser.add_argument("--masks", type=Path, help="Directory of mask PNGs (black=ignore)")
    parser.add_argument("--out", type=Path, help="Diff / report output directory")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=2.0,
        help="Documented edge tolerance in px (recorded in report; default 2)",
    )
    parser.add_argument(
        "--threshold",
        type=int,
        default=30,
        help="Max channel delta before a pixel counts as fail (default 30)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run synthetic pass/fail self-test and exit",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        import tempfile

        with tempfile.TemporaryDirectory(prefix="ui_compare_") as td:
            return run_self_test(Path(td))

    if not args.html or not args.native or not args.masks or not args.out:
        parser.error("--html --native --masks --out are required (or use --self-test)")

    html_dir: Path = args.html
    native_dir: Path = args.native
    masks_dir: Path = args.masks
    out_dir: Path = args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    names = sorted({p.name for p in html_dir.glob("*.png")} & {p.name for p in native_dir.glob("*.png")})
    if not names:
        print("No matching basename PNGs between html and native", file=sys.stderr)
        write_report(
            out_dir,
            [],
            f"No pairs. Gate: >1% unmasked pixels with max channel delta > {args.threshold}; "
            f"edge tolerance goal ±{args.tolerance}px.",
        )
        return 1

    rows: list[dict] = []
    any_fail = False
    for name in names:
        html_img = load_rgb(html_dir / name)
        native_img = load_rgb(native_dir / name)
        mask_img = ensure_mask(masks_dir, name, html_img.size)
        result = compare_pair(html_img, native_img, mask_img, args.threshold)
        row = {
            "name": name,
            "ok": result["ok"],
            "compared": result["compared"],
            "failed": result["failed"],
            "fail_pct": result["fail_pct"],
            "error": result.get("error"),
        }
        rows.append(row)
        if result.get("heat") is not None:
            result["heat"].save(out_dir / name)
        if not result["ok"]:
            any_fail = True
            print(
                f"FAIL {name}: {result['fail_pct']:.3f}% "
                f"({result['failed']}/{result['compared']})",
                file=sys.stderr,
            )
        else:
            print(
                f"PASS {name}: {result['fail_pct']:.3f}% "
                f"({result['failed']}/{result['compared']})"
            )

    gate_note = (
        f"Gate: fail if >1% of unmasked pixels have max|ΔRGB| > {args.threshold}. "
        f"Documented chrome edge tolerance goal: ±{args.tolerance}px. "
        f"Mask policy: black=ignore, non-black=compare."
    )
    write_report(out_dir, rows, gate_note)
    print(f"Wrote {out_dir / 'report.md'}")
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
