#!/usr/bin/env python3
"""
Added in OPM: inspect baked weapon/grenade PNGs for center, margins, and clipping.

Usage:
  python3 uir_bake_inspect.py [--margin N] [--center-tol F] [--min-fill F] [--max-fill F]
    [--json] [--worst N] path/to/*.png

Exit 0 if all pass; 1 if any fail (clip, off-center, or fill out of range).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image


def alpha_bbox(img: Image.Image, threshold: int = 8):
    rgba = img.convert("RGBA")
    width, height = rgba.size
    pixels = rgba.load()
    min_x, min_y = width, height
    max_x, max_y = -1, -1
    opaque = 0
    for y in range(height):
        for x in range(width):
            if pixels[x, y][3] > threshold:
                opaque += 1
                if x < min_x:
                    min_x = x
                if y < min_y:
                    min_y = y
                if x > max_x:
                    max_x = x
                if y > max_y:
                    max_y = y
    if max_x < 0:
        return None
    return {
        "min_x": min_x,
        "min_y": min_y,
        "max_x": max_x,
        "max_y": max_y,
        "bw": max_x - min_x + 1,
        "bh": max_y - min_y + 1,
        "opaque": opaque,
    }


def inspect_one(
    path: Path,
    margin_min: int,
    center_tol: float,
    min_fill: float | None,
    max_fill: float | None,
) -> dict:
    img = Image.open(path)
    width, height = img.size
    bbox = alpha_bbox(img)
    result = {
        "path": str(path),
        "name": path.name,
        "w": width,
        "h": height,
        "ok": False,
        "empty": bbox is None,
    }
    if bbox is None:
        result["error"] = "no opaque pixels"
        return result

    ml = bbox["min_x"]
    mt = bbox["min_y"]
    mr = width - 1 - bbox["max_x"]
    mb = height - 1 - bbox["max_y"]
    cx = (bbox["min_x"] + bbox["max_x"]) * 0.5 / width
    cy = (bbox["min_y"] + bbox["max_y"]) * 0.5 / height
    fill = (bbox["bw"] * bbox["bh"]) / float(width * height)
    clips = ml < margin_min or mr < margin_min or mt < margin_min or mb < margin_min
    off = abs(cx - 0.5) > center_tol or abs(cy - 0.5) > center_tol
    bad_fill = False
    if min_fill is not None and fill < min_fill:
        bad_fill = True
    if max_fill is not None and fill > max_fill:
        bad_fill = True

    result.update(
        {
            "marginL": ml,
            "marginR": mr,
            "marginT": mt,
            "marginB": mb,
            "cx": cx,
            "cy": cy,
            "fillRatio": fill,
            "bboxArea": bbox["bw"] * bbox["bh"],
            "clips": clips,
            "offCenter": off,
            "badFill": bad_fill,
            "ok": not clips and not off and not bad_fill,
        }
    )
    return result


def failure_rank(r: dict) -> tuple:
    if "error" in r:
        return (0, 0, 0, 0)
    score = 0
    if r.get("clips"):
        score += 1000
    if r.get("offCenter"):
        score += 100
    score += abs(r.get("cx", 0.5) - 0.5) * 50
    score += abs(r.get("cy", 0.5) - 0.5) * 50
    if r.get("badFill"):
        score += 10
    min_margin = min(r.get("marginL", 0), r.get("marginR", 0), r.get("marginT", 0), r.get("marginB", 0))
    return (score, -min_margin, -r.get("fillRatio", 0), r.get("name", ""))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", help="PNG files or directories")
    ap.add_argument("--margin", type=int, default=2, help="minimum edge margin in px")
    ap.add_argument("--center-tol", type=float, default=0.08, help="max |cx-0.5| / |cy-0.5|")
    ap.add_argument("--min-fill", type=float, default=None, help="minimum fill ratio gate")
    ap.add_argument("--max-fill", type=float, default=None, help="maximum fill ratio gate")
    ap.add_argument("--json", action="store_true", help="emit JSON array to stdout")
    ap.add_argument("--worst", type=int, default=0, help="print only N worst failures")
    args = ap.parse_args()

    paths: list[Path] = []
    for p in args.paths:
        path = Path(p)
        if path.is_dir():
            paths.extend(sorted(path.glob("*.png")))
        else:
            paths.append(path)

    results = []
    for path in paths:
        try:
            results.append(
                inspect_one(path, args.margin, args.center_tol, args.min_fill, args.max_fill)
            )
        except Exception as exc:  # noqa: BLE001
            results.append({"path": str(path), "name": path.name, "ok": False, "error": str(exc)})

    failed = [r for r in results if not r.get("ok")]
    failed.sort(key=failure_rank, reverse=True)

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for r in sorted(results, key=lambda x: x.get("bboxArea", 0), reverse=True):
            if "error" in r:
                print(f"FAIL {r['path']}: {r['error']}")
                continue
            status = "OK  " if r["ok"] else "FAIL"
            print(
                f"{status} {Path(r['path']).name}: "
                f"{r['w']}x{r['h']} fill={r['fillRatio']:.3f} "
                f"cx={r['cx']:.3f} cy={r['cy']:.3f} "
                f"mLRTB={r['marginL']},{r['marginR']},{r['marginT']},{r['marginB']} "
                f"area={r['bboxArea']}"
                + (" CLIP" if r["clips"] else "")
                + (" OFFCENTER" if r["offCenter"] else "")
                + (" BADFILL" if r["badFill"] else "")
            )
        if failed and args.worst > 0:
            print(f"--- worst {args.worst} ---")
            for r in failed[: args.worst]:
                print(
                    f"WORST {r.get('name', r['path'])} cx={r.get('cx')} cy={r.get('cy')} "
                    f"fill={r.get('fillRatio')} clips={r.get('clips')}"
                )
        valid = [r for r in results if "bboxArea" in r]
        if valid:
            largest = max(valid, key=lambda r: r["bboxArea"])
            print(
                f"LARGEST {Path(largest['path']).name} area={largest['bboxArea']} "
                f"fill={largest.get('fillRatio', 0):.3f}"
            )

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
