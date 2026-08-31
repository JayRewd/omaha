#!/usr/bin/env python3
"""
Added in OPM: FOV calibration for weapon icon bakes.

Reads artifacts/bake_mp_weapons.sh (read-only), searches shared FOV for guns
(mp44 anchor) and misc 100x100 (grenades + binoculars at same FOV).

Pass criteria:
  - All edges >= --margin px (default 1) without clipping.
  - Snug vertical fit on guns: top/bottom margins in [margin, margin+1] and
    within 1 px of each other (target ~1 px top and bottom on the anchor).

Writes artifacts/bake_fov_calibration.json — does not modify the reference script.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT_REF = REPO / "artifacts/bake_mp_weapons.sh"
OUT_JSON = REPO / "artifacts/bake_fov_calibration.json"
INSPECT = REPO / "code/uirender/tools/uir_bake_inspect.py"
# Override with OPENMOHAA_DEST; never hardcode a personal absolute path.
GAMES = Path(os.environ.get("OPENMOHAA_DEST", str(Path.home() / "Games/openmohaa")))
BINARY = GAMES / "openmohaa"
HOME_MAIN = Path.home() / ".local/share/openmohaa/main"
PROBE_VFS = "ui/modern/textures/weapons/_fov_probe"
PROBE_DIR = HOME_MAIN / PROBE_VFS.replace("/", "/")

MARGIN = 1
SNUG_TB_SPREAD = 1
CENTER_TOL = 0.08
GUN_FOV_LO = 20.0
GUN_FOV_HI = 55.0
MISC_FOV_LO = 10.0
MISC_FOV_HI = 40.0
BINOCULAR_FILL_LO = 0.7
BINOCULAR_FILL_HI = 1.3
MP44_ANCHOR = "mp44"
OFF_Z_SEARCH_RADIUS = 12.0
OFF_Z_COARSE_STEP = 0.5
OFF_Z_FINE_STEP = 0.25


@dataclass
class BakeBlock:
    model: str
    out_name: str
    yaw: float
    pitch: float
    roll: float
    scale: float
    fov: float
    off_x: float
    off_y: float
    off_z: float
    width: int
    height: int

    @property
    def png_name(self) -> str:
        return Path(self.out_name).name

    @property
    def stem(self) -> str:
        return Path(self.png_name).stem.lower()

    @property
    def kind(self) -> str:
        if "binoculars" in self.model.lower():
            return "binocular"
        if self.width == 500 and self.height == 100:
            return "gun"
        if self.width == 100 and self.height == 100:
            return "grenade"
        return "other"

    @property
    def vfs_out(self) -> str:
        return f"{PROBE_VFS}/{self.png_name}"


def parse_reference_script(path: Path) -> list[BakeBlock]:
    text = path.read_text()
    blocks: list[BakeBlock] = []
    pattern = re.compile(
        r"\+ui_bake_model\s+(\S+)\s+\$O/(\S+)\s*\\?\s*"
        r"([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)\s*\\?\s*"
        r"([\d.]+)\s+([\d.]+)\s*\\?\s*"
        r"([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)\s*\\?\s*"
        r"(\d+)\s+(\d+)",
        re.MULTILINE,
    )
    for m in pattern.finditer(text):
        blocks.append(
            BakeBlock(
                model=m.group(1),
                out_name=m.group(2),
                yaw=float(m.group(3)),
                pitch=float(m.group(4)),
                roll=float(m.group(5)),
                scale=float(m.group(6)),
                fov=float(m.group(7)),
                off_x=float(m.group(8)),
                off_y=float(m.group(9)),
                off_z=float(m.group(10)),
                width=int(m.group(11)),
                height=int(m.group(12)),
            )
        )
    return blocks


def run_openmohaa(extra_args: list[str]) -> subprocess.CompletedProcess[str]:
    # Play default is opengl1; bake must opt in via startup +set (see CL_InitRef).
    cmd = [
        str(BINARY),
        "+set",
        "cl_renderer",
        "opengl2",
        "+set",
        "cl_playintro",
        "0",
        *extra_args,
        "+quit",
    ]
    return subprocess.run(
        cmd, cwd=GAMES, capture_output=True, text=True, timeout=180, check=False
    )


def bake_cmd_args(
    block: BakeBlock,
    fov: float,
    *,
    off_x: float | None = None,
    off_y: float | None = None,
    off_z: float | None = None,
) -> list[str]:
    ox = block.off_x if off_x is None else off_x
    oy = block.off_y if off_y is None else off_y
    oz = block.off_z if off_z is None else off_z
    return [
        "+ui_bake_model",
        block.model,
        block.vfs_out,
        f"{block.yaw:.4g}",
        f"{block.pitch:.4g}",
        f"{block.roll:.4g}",
        f"{block.scale:.4g}",
        f"{fov:.4g}",
        f"{ox:.4g}",
        f"{oy:.4g}",
        f"{oz:.4g}",
        str(block.width),
        str(block.height),
    ]


def bake_one(
    block: BakeBlock,
    fov: float,
    *,
    off_x: float | None = None,
    off_y: float | None = None,
    off_z: float | None = None,
) -> Path:
    PROBE_DIR.mkdir(parents=True, exist_ok=True)
    out = PROBE_DIR / block.png_name
    proc = run_openmohaa(bake_cmd_args(block, fov, off_x=off_x, off_y=off_y, off_z=off_z))
    if proc.returncode != 0 or not out.is_file() or out.stat().st_size == 0:
        err = (proc.stderr or proc.stdout or "").strip()
        raise RuntimeError(
            f"bake failed for {block.stem} fov={fov}: rc={proc.returncode} {err[-400:]}"
        )
    return out


def bake_batch(
    blocks: list[BakeBlock],
    fov: float,
    *,
    off_z_by_stem: dict[str, float] | None = None,
) -> None:
    if not blocks:
        return
    PROBE_DIR.mkdir(parents=True, exist_ok=True)
    args: list[str] = []
    for block in blocks:
        oz = None
        if off_z_by_stem and block.stem in off_z_by_stem:
            oz = off_z_by_stem[block.stem]
        args.extend(bake_cmd_args(block, fov, off_z=oz))
    proc = run_openmohaa(args)
    missing = [
        b.stem
        for b in blocks
        if not (PROBE_DIR / b.png_name).is_file()
        or (PROBE_DIR / b.png_name).stat().st_size == 0
    ]
    if proc.returncode != 0 or missing:
        err = (proc.stderr or proc.stdout or "").strip()
        raise RuntimeError(
            f"batch bake failed fov={fov} missing={missing} rc={proc.returncode} {err[-400:]}"
        )


def inspect_png(path: Path, margin: int) -> dict:
    proc = subprocess.run(
        [
            sys.executable,
            str(INSPECT),
            "--json",
            "--margin",
            str(margin),
            "--center-tol",
            str(CENTER_TOL),
            str(path),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if not proc.stdout.strip():
        return {"path": str(path), "name": path.name, "ok": False, "error": "inspect failed"}
    rows = json.loads(proc.stdout)
    return rows[0] if rows else {"path": str(path), "ok": False, "error": "empty inspect"}


def inspect_at(path: Path, margin: int) -> dict:
    if not path.is_file() or path.stat().st_size == 0:
        return {
            "path": str(path),
            "name": path.name,
            "ok": False,
            "empty": True,
            "error": "missing or empty bake PNG",
            "clips": True,
            "ok_clip": False,
            "snug_vertical": False,
        }
    r = inspect_png(path, margin)
    if r.get("empty") or "error" in r:
        r.setdefault("clips", True)
        r["ok_clip"] = False
        r["snug_vertical"] = False
        return r
    r["ok_clip"] = not r.get("clips", True)
    r["snug_vertical"] = passes_snug_vertical(r, margin)
    return r


def passes_snug_vertical(r: dict, margin: int) -> bool:
    mt = r.get("marginT")
    mb = r.get("marginB")
    if mt is None or mb is None:
        return False
    hi = margin + SNUG_TB_SPREAD
    return (
        mt >= margin
        and mb >= margin
        and mt <= hi
        and mb <= hi
        and abs(mt - mb) <= SNUG_TB_SPREAD
    )


def passes_clip_gate(r: dict) -> bool:
    return bool(r.get("ok_clip"))


def passes_snug_gate(r: dict, margin: int) -> bool:
    return passes_clip_gate(r) and passes_snug_vertical(r, margin)


def verify_summary(
    block: BakeBlock,
    fov: float,
    r: dict,
    *,
    off_z: float | None = None,
    off_z_delta: float | None = None,
) -> dict:
    out = {
        "name": block.stem,
        "model": block.model,
        "fov": fov,
        "clips": r.get("clips", True),
        "ok_clip": not r.get("clips", True),
        "snug_vertical": r.get("snug_vertical", False),
        "margins": {
            "L": r.get("marginL"),
            "R": r.get("marginR"),
            "T": r.get("marginT"),
            "B": r.get("marginB"),
        },
        "fillRatio": r.get("fillRatio"),
        "cx": r.get("cx"),
        "cy": r.get("cy"),
    }
    if off_z is not None:
        out["off_z"] = round(off_z, 4)
    if off_z_delta is not None:
        out["off_z_delta"] = round(off_z_delta, 4)
    return out


def search_off_z_snug(
    block: BakeBlock, fov: float, margin: int
) -> tuple[float | None, dict | None]:
    """Find offZ (near script value) giving ~margin px top and bottom."""

    def score(r: dict) -> tuple:
        mt = r["marginT"]
        mb = r["marginB"]
        return (max(mt, mb), abs(mt - mb), abs(r.get("marginL", 999) - r.get("marginR", 999)))

    best: tuple[tuple, float, dict] | None = None
    lo = block.off_z - OFF_Z_SEARCH_RADIUS
    hi = block.off_z + OFF_Z_SEARCH_RADIUS

    def scan(step: float, window_lo: float, window_hi: float) -> None:
        nonlocal best
        oz = window_lo
        while oz <= window_hi + 1e-9:
            path = bake_one(block, fov, off_z=round(oz, 4))
            r = inspect_at(path, margin)
            if passes_snug_gate(r, margin):
                s = score(r)
                if best is None or s < best[0]:
                    best = (s, round(oz, 4), r)
            oz += step

    scan(OFF_Z_COARSE_STEP, lo, hi)
    if best is not None:
        _, center, _ = best
        scan(
            OFF_Z_FINE_STEP,
            center - OFF_Z_COARSE_STEP,
            center + OFF_Z_COARSE_STEP,
        )
        return best[1], best[2]

    return None, None


def search_min_fov_snug(
    block: BakeBlock, lo: float, hi: float, margin: int, refine: float = 0.5
) -> tuple[float, float, dict]:
    """Lowest FOV where block can achieve snug vertical fit via offZ."""

    def probe(fov: float) -> tuple[float | None, dict | None]:
        return search_off_z_snug(block, fov, margin)

    hi_oz, hi_r = probe(hi)
    while hi_oz is None and hi < 80.0:
        hi = round(hi + 2.0, 2)
        hi_oz, hi_r = probe(hi)
    if hi_oz is None or hi_r is None:
        raise RuntimeError(f"{block.stem}: no snug FOV up to 80")

    lo_oz, lo_r = probe(lo)
    if lo_oz is not None and lo_r is not None:
        return lo, lo_oz, lo_r

    low, high = lo, hi
    best_fov = hi
    best_oz = hi_oz
    best_r = hi_r
    while high - low > refine:
        mid = round((low + high) * 0.5, 2)
        mid_oz, mid_r = probe(mid)
        if mid_oz is not None and mid_r is not None:
            best_fov = mid
            best_oz = mid_oz
            best_r = mid_r
            high = mid
        else:
            low = mid

    return best_fov, best_oz, best_r


def search_min_fov_pass(
    block: BakeBlock, lo: float, hi: float, margin: int, refine: float = 0.5
) -> tuple[float, dict]:
    """Lowest FOV in [lo, hi] where block passes clip gate (binary search)."""

    def probe(fov: float) -> dict:
        path = bake_one(block, fov)
        return inspect_at(path, margin)

    hi_r = probe(hi)
    while not passes_clip_gate(hi_r) and hi < 80.0:
        hi = round(hi + 2.0, 2)
        hi_r = probe(hi)
    if not passes_clip_gate(hi_r):
        raise RuntimeError(
            f"{block.stem}: no passing FOV up to 80 (last inspect: {hi_r.get('error', hi_r)})"
        )

    lo_r = probe(lo)
    if passes_clip_gate(lo_r):
        return lo, lo_r

    low, high = lo, hi
    best_fov = hi
    best_r = hi_r
    while high - low > refine:
        mid = round((low + high) * 0.5, 2)
        mid_r = probe(mid)
        if passes_clip_gate(mid_r):
            best_fov = mid
            best_r = mid_r
            high = mid
        else:
            low = mid

    return best_fov, best_r


def verify_guns_at_fov(
    guns: list[BakeBlock],
    fov: float,
    margin: int,
    *,
    off_z_by_stem: dict[str, float] | None = None,
) -> tuple[list[dict], list[str], list[str]]:
    bake_batch(guns, fov, off_z_by_stem=off_z_by_stem)
    verify: list[dict] = []
    clip_outliers: list[str] = []
    snug_outliers: list[str] = []
    for gun in guns:
        oz = off_z_by_stem.get(gun.stem) if off_z_by_stem else None
        r = inspect_at(PROBE_DIR / gun.png_name, margin)
        delta = None if oz is None else oz - gun.off_z
        verify.append(
            verify_summary(gun, fov, r, off_z=oz, off_z_delta=delta)
        )
        if not r.get("ok_clip"):
            clip_outliers.append(gun.stem)
        elif gun.kind == "gun" and not r.get("snug_vertical"):
            snug_outliers.append(gun.stem)
    return verify, clip_outliers, snug_outliers


def calibrate_guns(guns: list[BakeBlock], margin: int) -> dict:
    anchor = next((g for g in guns if g.stem == MP44_ANCHOR), None)
    if not anchor:
        raise SystemExit(f"anchor weapon {MP44_ANCHOR!r} not found in reference script")

    start_fov = anchor.fov
    lo = max(GUN_FOV_LO, start_fov - 5.0)
    gun_fov, anchor_off_z, anchor_r = search_min_fov_snug(anchor, lo, GUN_FOV_HI, margin)

    verify, clip_outliers, _ = verify_guns_at_fov(guns, gun_fov, margin)
    bump = 0
    while clip_outliers and gun_fov < 80.0 and bump < 15:
        gun_fov = round(gun_fov + 1.0, 2)
        verify, clip_outliers, _ = verify_guns_at_fov(guns, gun_fov, margin)
        bump += 1

    anchor_off_z, anchor_r = search_off_z_snug(anchor, gun_fov, margin)
    if anchor_off_z is None:
        raise RuntimeError(
            f"{MP44_ANCHOR}: no snug fit at gun_fov={gun_fov}; "
            "try raising GUN_FOV_HI or OFF_Z_SEARCH_RADIUS"
        )

    off_z_map: dict[str, float] = {MP44_ANCHOR: anchor_off_z}
    for gun in guns:
        if gun.stem == MP44_ANCHOR:
            continue
        oz, _ = search_off_z_snug(gun, gun_fov, margin)
        if oz is not None:
            off_z_map[gun.stem] = oz
        else:
            off_z_map[gun.stem] = gun.off_z

    verify, _, snug_outliers = verify_guns_at_fov(
        guns, gun_fov, margin, off_z_by_stem=off_z_map
    )

    anchor_delta = anchor_off_z - anchor.off_z
    off_z_paste: dict[str, float] = {}
    off_z_delta_paste: dict[str, float] = {}
    for stem, oz in off_z_map.items():
        off_z_paste[stem] = oz
        ref = next(g.off_z for g in guns if g.stem == stem)
        off_z_delta_paste[stem] = round(oz - ref, 4)

    return {
        "shared_fov": gun_fov,
        "margin_px": margin,
        "anchor": MP44_ANCHOR,
        "anchor_search": verify_summary(
            anchor,
            gun_fov,
            anchor_r,
            off_z=anchor_off_z,
            off_z_delta=anchor_delta,
        ),
        "verify": verify,
        "outliers_after_bump": clip_outliers,
        "snug_vertical_failures": snug_outliers,
        "bump_steps": bump,
        "off_z_paste": off_z_paste,
        "off_z_delta_paste": off_z_delta_paste,
    }


def calibrate_misc(
    grenades: list[BakeBlock], binocular: BakeBlock | None, margin: int
) -> dict:
    misc_items = list(grenades)
    if binocular:
        misc_items.append(binocular)

    start = min(b.fov for b in misc_items) if misc_items else 15.0
    lo = max(MISC_FOV_LO, start - 3.0)

    worst = grenades[0] if grenades else misc_items[0]
    for grenade in grenades:
        bake_one(grenade, lo)
        if not inspect_at(PROBE_DIR / grenade.png_name, margin).get("ok_clip"):
            worst = grenade
            break

    misc_fov, _ = search_min_fov_pass(worst, lo, MISC_FOV_HI, margin)

    all_misc = list(grenades)
    if binocular:
        all_misc.append(binocular)
    bake_batch(all_misc, misc_fov)

    nade_verify: list[dict] = []
    nade_fills: list[float] = []
    for grenade in grenades:
        r = inspect_at(PROBE_DIR / grenade.png_name, margin)
        nade_verify.append(verify_summary(grenade, misc_fov, r))
        if r.get("fillRatio") is not None:
            nade_fills.append(float(r["fillRatio"]))

    size_warnings: list[str] = []
    bino_result: dict | None = None
    fill_vs_nades: float | None = None

    if binocular and nade_fills:
        r = inspect_at(PROBE_DIR / binocular.png_name, margin)
        bino_result = verify_summary(binocular, misc_fov, r)
        mean_fill = sum(nade_fills) / len(nade_fills)
        bfill = r.get("fillRatio")
        if bfill is not None and mean_fill > 0:
            fill_vs_nades = float(bfill) / mean_fill
            if fill_vs_nades < BINOCULAR_FILL_LO or fill_vs_nades > BINOCULAR_FILL_HI:
                size_warnings.append(
                    f"binoculars fillRatio {bfill:.3f} is {fill_vs_nades:.2f}x grenade mean "
                    f"(band {BINOCULAR_FILL_LO}-{BINOCULAR_FILL_HI}); tune offsets in script, not FOV"
                )

    while binocular:
        r = inspect_at(PROBE_DIR / binocular.png_name, margin)
        if r.get("ok_clip"):
            break
        if misc_fov >= 80.0:
            size_warnings.append("binoculars still clip at misc_fov cap 80")
            break
        misc_fov = round(misc_fov + 1.0, 2)
        bake_batch(all_misc, misc_fov)
        nade_verify = []
        nade_fills = []
        for grenade in grenades:
            rr = inspect_at(PROBE_DIR / grenade.png_name, margin)
            nade_verify.append(verify_summary(grenade, misc_fov, rr))
            if rr.get("fillRatio") is not None:
                nade_fills.append(float(rr["fillRatio"]))
        r = inspect_at(PROBE_DIR / binocular.png_name, margin)
        bino_result = verify_summary(binocular, misc_fov, r)
        if nade_fills and r.get("fillRatio") is not None:
            mean_fill = sum(nade_fills) / len(nade_fills)
            fill_vs_nades = float(r["fillRatio"]) / mean_fill if mean_fill > 0 else None

    return {
        "shared_fov": misc_fov,
        "margin_px": margin,
        "grenades": {"verify": nade_verify},
        "binoculars": {
            "verify": bino_result,
            "fill_vs_nades": fill_vs_nades,
        },
        "size_warnings": size_warnings,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--script",
        type=Path,
        default=SCRIPT_REF,
        help="reference bake script (read-only)",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=OUT_JSON,
        help="JSON output path",
    )
    ap.add_argument(
        "--margin",
        type=int,
        default=MARGIN,
        help="minimum edge margin in px (default 1 = snug clip gate)",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="parse only, no bakes",
    )
    args = ap.parse_args()

    if not args.script.is_file():
        print(f"missing reference script: {args.script}", file=sys.stderr)
        return 1
    if not BINARY.is_file():
        print(f"missing openmohaa binary: {BINARY}", file=sys.stderr)
        return 1

    blocks = parse_reference_script(args.script)
    guns = [b for b in blocks if b.kind == "gun"]
    grenades = [b for b in blocks if b.kind == "grenade"]
    binoculars = [b for b in blocks if b.kind == "binocular"]
    binocular = binoculars[0] if binoculars else None

    print(
        f"parsed {len(blocks)} blocks: {len(guns)} guns, {len(grenades)} grenades, "
        f"{len(binoculars)} binoculars (margin={args.margin}px snug TB)"
    )

    if args.dry_run:
        for block in blocks:
            print(f"  {block.kind:10} {block.stem} fov={block.fov} {block.width}x{block.height}")
        return 0

    guns_result = calibrate_guns(guns, args.margin)
    misc_result = calibrate_misc(grenades, binocular, args.margin)

    payload = {
        "reference_script": str(args.script.relative_to(REPO)),
        "margin_px": args.margin,
        "guns": guns_result,
        "misc_100x100": misc_result,
        "notes": (
            f"scale left at 1.0; paste guns.shared_fov on all 500x100 lines and "
            f"misc_100x100.shared_fov on all 100x100 lines (grenades + binoculars). "
            f"Snug vertical fit targets ~{args.margin}px top/bottom; paste guns.off_z_paste "
            f"offZ values where listed. Reference script was not modified."
        ),
        "paste_summary": {
            "gun_fov": guns_result["shared_fov"],
            "misc_fov": misc_result["shared_fov"],
            "gun_off_z": guns_result.get("off_z_paste", {}),
        },
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2) + "\n")

    anchor = guns_result["anchor_search"]
    print()
    print("=== FOV calibration complete ===")
    print(f"Gun FOV (13 lines):  {guns_result['shared_fov']}")
    print(f"Misc FOV (3 lines):  {misc_result['shared_fov']}")
    print(
        f"Anchor {MP44_ANCHOR}: T={anchor['margins']['T']} B={anchor['margins']['B']} "
        f"offZ={anchor.get('off_z')} (delta {anchor.get('off_z_delta', 0):+.4g})"
    )
    if guns_result.get("outliers_after_bump"):
        print(f"Gun clip outliers: {guns_result['outliers_after_bump']}")
    if guns_result.get("snug_vertical_failures"):
        print(f"Gun snug-TB outliers: {guns_result['snug_vertical_failures']}")
    for warning in misc_result.get("size_warnings", []):
        print(f"WARNING: {warning}")
    print(f"Wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
