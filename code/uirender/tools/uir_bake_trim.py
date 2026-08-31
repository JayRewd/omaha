#!/usr/bin/env python3
"""
Added in OPM: trim transparent margins from baked weapon PNGs.

Runs after ui_bake_model export (including MSAA resolve). Reads *x2.png MSAA
bakes from VFS, writes snug trimmed HUD PNGs (x2 suffix stripped) plus an
all-white filled variant with a crisp 1 px alpha expand (MSAA preserved, no blur).

Usage:
  python3 uir_bake_trim.py [--out DIR] [--pad N] [--strip-suffix S] path/to/*.png
  python3 uir_bake_trim.py --from-vfs ui/modern/textures/weapons --glob '*x2.png'
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageFilter

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[2]
sys.path.insert(0, str(TOOLS))

from uir_bake_inspect import alpha_bbox  # noqa: E402

DEFAULT_OUT = REPO / "assets/main/ui/modern/textures/modernhud"
HOME_MAIN = Path.home() / ".local/share/openmohaa/main"
DEFAULT_STRIP_SUFFIX = "x2"
DEFAULT_FILLED_SUFFIX = "_filled"


def trim_image(img: Image.Image, threshold: int, pad: int) -> tuple[Image.Image, dict]:
    bbox = alpha_bbox(img, threshold)
    if bbox is None:
        raise ValueError("no opaque pixels")

    width, height = img.size
    x0 = max(0, bbox["min_x"] - pad)
    y0 = max(0, bbox["min_y"] - pad)
    x1 = min(width, bbox["max_x"] + 1 + pad)
    y1 = min(height, bbox["max_y"] + 1 + pad)
    cropped = img.crop((x0, y0, x1, y1))
    return cropped, {
        "src_w": width,
        "src_h": height,
        "out_w": x1 - x0,
        "out_h": y1 - y0,
        "crop": [x0, y0, x1, y1],
        "bbox": bbox,
    }


def base_stem(src: Path, strip_suffix: str | None) -> str:
    stem = src.stem
    if strip_suffix and stem.endswith(strip_suffix):
        stem = stem[: -len(strip_suffix)]
    return stem


def output_name(src: Path, strip_suffix: str | None) -> str:
    return f"{base_stem(src, strip_suffix)}.png"


def filled_output_name(src: Path, strip_suffix: str | None, filled_suffix: str) -> str:
    return f"{base_stem(src, strip_suffix)}{filled_suffix}.png"


def dilate_alpha(alpha: Image.Image, radius: int) -> Image.Image:
    """Grayscale dilation — preserves MSAA fringe instead of binarizing."""
    out = alpha
    for _ in range(radius):
        out = out.filter(ImageFilter.MaxFilter(3))
    return out


def make_filled_white(
    img: Image.Image,
    *,
    threshold: int,
    stroke_width: int,
) -> Image.Image:
    """
    Solid white fill using the bake MSAA alpha, with optional crisp 1px expand.

    No binarization (jagged) and no blur/supersample (mushy).
    """
    rgba = img.convert("RGBA")
    alpha = rgba.split()[3]
    soft_alpha = alpha.point(lambda a: 0 if a <= threshold else a, mode="L")

    if stroke_width > 0:
        expanded = dilate_alpha(soft_alpha, stroke_width)
        final_alpha = ImageChops.lighter(soft_alpha, expanded)
    else:
        final_alpha = soft_alpha

    white = Image.new("RGBA", rgba.size, (255, 255, 255, 0))
    white.putalpha(final_alpha)
    return white


def collect_paths(
    paths: list[str],
    *,
    from_vfs: str | None,
    glob_pattern: str | None,
    strip_suffix: str | None,
) -> list[Path]:
    found: list[Path] = []
    if from_vfs:
        base = HOME_MAIN / from_vfs.replace("/", "/")
        pattern = glob_pattern or "*x2.png"
        found.extend(sorted(base.glob(pattern)))
    for raw in paths:
        path = Path(raw)
        if path.is_dir():
            found.extend(sorted(path.glob("*.png")))
        elif path.is_file():
            found.append(path)

    seen: set[Path] = set()
    unique: list[Path] = []
    for path in found:
        resolved = path.resolve()
        if resolved in seen:
            continue
        if strip_suffix and not path.stem.endswith(strip_suffix):
            continue
        seen.add(resolved)
        unique.append(path)
    return unique


def trim_file(
    src: Path,
    out_dir: Path,
    *,
    threshold: int,
    pad: int,
    strip_suffix: str | None,
    filled_suffix: str,
    stroke_width: int,
    write_filled: bool,
    dry_run: bool,
) -> dict:
    result: dict = {"src": str(src), "name": src.name, "ok": False}
    try:
        img = Image.open(src).convert("RGBA")
        cropped, meta = trim_image(img, threshold, pad)
        filled_base, _ = trim_image(img, threshold, max(pad, stroke_width))
        filled = make_filled_white(
            filled_base,
            threshold=threshold,
            stroke_width=stroke_width,
        )

        out_name = output_name(src, strip_suffix)
        filled_name = filled_output_name(src, strip_suffix, filled_suffix)
        dst = out_dir / out_name
        filled_dst = out_dir / filled_name

        result.update(meta)
        result["out"] = str(dst)
        result["out_name"] = out_name
        result["filled_out"] = str(filled_dst)
        result["filled_out_name"] = filled_name
        result["filled_w"] = filled.size[0]
        result["filled_h"] = filled.size[1]

        if not dry_run:
            out_dir.mkdir(parents=True, exist_ok=True)
            cropped.save(dst, optimize=True)
            if write_filled:
                filled.save(filled_dst, optimize=True)
        result["ok"] = True
    except Exception as exc:  # noqa: BLE001
        result["error"] = str(exc)
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="*", help="PNG files or directories")
    ap.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"output directory (default: {DEFAULT_OUT.relative_to(REPO)})",
    )
    ap.add_argument(
        "--from-vfs",
        type=str,
        default=None,
        help="read PNGs from ~/.local/share/openmohaa/main/<vfs path>",
    )
    ap.add_argument(
        "--glob",
        dest="glob_pattern",
        type=str,
        default="*x2.png",
        help="glob under --from-vfs (default: '*x2.png')",
    )
    ap.add_argument(
        "--alpha-threshold",
        type=int,
        default=8,
        help="alpha >= threshold counts as content (match inspect tool)",
    )
    ap.add_argument(
        "--pad",
        type=int,
        default=0,
        help="transparent pixels to keep around snug bbox for trimmed PNG",
    )
    ap.add_argument(
        "--strip-suffix",
        type=str,
        default=DEFAULT_STRIP_SUFFIX,
        help="remove suffix from output basename (e.g. mp40x2 -> mp40); empty to keep",
    )
    ap.add_argument(
        "--filled-suffix",
        type=str,
        default=DEFAULT_FILLED_SUFFIX,
        help="suffix before .png for white filled variant (default: _filled)",
    )
    ap.add_argument(
        "--filled-stroke-width",
        type=int,
        default=1,
        help="outside expand radius in pixels for filled variant (grayscale, no blur)",
    )
    ap.add_argument(
        "--no-filled",
        action="store_true",
        help="skip writing *_filled.png variants",
    )
    ap.add_argument("--dry-run", action="store_true", help="report crops without writing")
    ap.add_argument("--json", action="store_true", help="emit JSON summary")
    args = ap.parse_args()

    strip_suffix = args.strip_suffix or None
    paths = collect_paths(
        args.paths,
        from_vfs=args.from_vfs,
        glob_pattern=args.glob_pattern,
        strip_suffix=strip_suffix,
    )
    if not paths:
        print("no input PNGs found", file=sys.stderr)
        return 1

    results = [
        trim_file(
            src,
            args.out,
            threshold=args.alpha_threshold,
            pad=args.pad,
            strip_suffix=strip_suffix,
            filled_suffix=args.filled_suffix,
            stroke_width=max(0, args.filled_stroke_width),
            write_filled=not args.no_filled,
            dry_run=args.dry_run,
        )
        for src in paths
    ]

    failed = [r for r in results if not r.get("ok")]
    written = (len(results) - len(failed)) * (2 if not args.no_filled else 1)

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for r in results:
            if not r.get("ok"):
                print(f"FAIL {r.get('src')}: {r.get('error')}")
                continue
            print(
                f"{'DRY   ' if args.dry_run else 'TRIM  '} {r['name']} -> {r['out_name']} "
                f"{r['src_w']}x{r['src_h']} -> {r['out_w']}x{r['out_h']} "
                f"crop={r['crop']}"
            )
            if not args.no_filled:
                print(
                    f"{'DRY   ' if args.dry_run else 'FILL  '} {r['name']} -> {r['filled_out_name']} "
                    f"{r['filled_w']}x{r['filled_h']}"
                )
        print(f"{'Would write' if args.dry_run else 'Wrote'} {written} PNG(s) to {args.out}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
