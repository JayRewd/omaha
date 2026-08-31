#!/usr/bin/env python3
"""
Added in OPM: staged world-model weapon bake calibration loop.

Runs probe/final bakes via openmohaa, inspects PNGs, updates uir_weapon_bake_list.c.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BAKE_LIST = REPO / "code/uirender/uir_weapon_bake_list.c"
CL_UIRENDER = REPO / "code/client/cl_uirender.cpp"
INSPECT = REPO / "code/uirender/tools/uir_bake_inspect.py"
# Override with OPENMOHAA_DEST; never hardcode a personal absolute path.
GAMES = Path(os.environ.get("OPENMOHAA_DEST", str(Path.home() / "Games/openmohaa")))
BINARY = GAMES / "openmohaa"
HOME_MAIN = Path.home() / ".local/share/openmohaa/main"
PROBE_VFS = "ui/modern/textures/weapons/_probe"
FINAL_VFS = "ui/modern/textures/weapons"
PROBE_DIR = HOME_MAIN / PROBE_VFS.replace("/", "/")
FINAL_DIR = HOME_MAIN / FINAL_VFS.replace("/", "/")


@dataclass
class BakeEntry:
    path: str
    kind: str
    angles: list[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])
    offset: list[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])
    framing_scale: float = 0.0
    has_angles: int = 0
    has_offset: int = 0
    exclude_pool: int = 0

    @property
    def png_name(self) -> str:
        base = self.path.rsplit("/", 1)[-1]
        if base.lower().endswith(".tik"):
            base = base[:-4]
        return base.lower() + ".png"

    @property
    def vfs_out(self) -> str:
        return f"{PROBE_VFS}/{self.png_name}"


def expand_bake_macros(text: str) -> str:
    return (
        text.replace("{A_GUN}", "{0.0f, 90.0f, 90.0f}")
        .replace("{A_PIST}", "{0.0f, 90.0f, -90.0f}")
        .replace("{A_NADE}", "{0.0f, 90.0f, 0.0f}")
        .replace("{A_MISC}", "{0.0f, 90.0f, 0.0f}")
        .replace("{A0}", "{0.0f, 0.0f, 0.0f}")
        .replace("{O0}", "{0.0f, 0.0f, 0.0f}")
    )


def parse_bake_list(text: str) -> list[BakeEntry]:
    entries: list[BakeEntry] = []
    text = expand_bake_macros(text)
    macro_re = re.compile(
        r'\{"(?P<path>models/[^"]+)",\s*\{(?P<a>[^}]+)\},\s*\{(?P<o>[^}]+)\},\s*'
        r'(?P<fs>[-0-9.]+f),\s*(?P<ha>\d+),\s*(?P<ho>\d+)(?:,\s*(?P<pool>\d+))?\s*,\s*'
        r'UIR_BAKE_(?P<kind>GUN|GRENADE|OTHER)\},?'
    )
    float_re = re.compile(r'[-]?\d+\.\d+f|[-]?\d+')
    for m in macro_re.finditer(text):
        path = m.group("path")
        kind = m.group("kind")
        if kind == "GUN":
            kind = "GUN"
        elif kind == "GRENADE":
            kind = "NADE"
        else:
            kind = "MISC"
        a = [float(x.rstrip("f")) for x in float_re.findall(m.group("a"))]
        o = [float(x.rstrip("f")) for x in float_re.findall(m.group("o"))]
        fs = float(m.group("fs").rstrip("f"))
        ha = int(m.group("ha"))
        ho = int(m.group("ho"))
        pool = int(m.group("pool")) if m.group("pool") else 0
        if len(a) < 3 or len(o) < 3:
            continue
        entries.append(
            BakeEntry(
                path=path,
                kind=kind,
                angles=a[:3],
                offset=o[:3],
                framing_scale=fs,
                has_angles=ha,
                has_offset=ho,
                exclude_pool=pool,
            )
        )
    return entries


def fmt_float(v: float) -> str:
    if abs(v) < 1e-6:
        return "0.0f"
    return f"{v:.2f}f"


def format_entry(e: BakeEntry) -> str:
    a = ", ".join(fmt_float(v) for v in e.angles)
    o = ", ".join(fmt_float(v) for v in e.offset)
    fs = fmt_float(e.framing_scale) if e.framing_scale > 0 else "0.0f"
    kind_const = {"GUN": "UIR_BAKE_GUN", "NADE": "UIR_BAKE_GRENADE", "MISC": "UIR_BAKE_OTHER"}[e.kind]
    if e.kind == "GUN":
        return (
            f'\t{{"{e.path}", {{{a}}}, {{{o}}}, {fs}, {e.has_angles}, {e.has_offset}, '
            f"{e.exclude_pool}, {kind_const}}},"
        )
    return f'\t{{"{e.path}", {{{a}}}, {{{o}}}, {fs}, {e.has_angles}, {e.has_offset}, {kind_const}}},'


def write_bake_list(entries: list[BakeEntry]) -> None:
    if not entries:
        print("write_bake_list: refusing to write empty list", file=sys.stderr)
        return
    text = BAKE_LIST.read_text()
    start = text.index("static const uir_weapon_bake_entry_t")
    end = text.index("int UIR_WeaponBakeEntryCount")
    header = text[:start]
    footer = text[end:]
    body = "static const uir_weapon_bake_entry_t g_weaponBakeList[] = {\n"
    body += "\n".join(format_entry(e) for e in entries)
    body += "\n};\n\n"
    BAKE_LIST.write_text(header + body + footer)


def update_cl_defaults(gun_scale: float, nade_scale: float) -> None:
    text = CL_UIRENDER.read_text()
    text = re.sub(
        r"#define UIR_BAKE_GUN_SCALE\s+[-0-9.]+f",
        f"#define UIR_BAKE_GUN_SCALE      {gun_scale:.2f}f",
        text,
        count=1,
    )
    text = re.sub(
        r"#define UIR_BAKE_GRENADE_SCALE\s+[-0-9.]+f",
        f"#define UIR_BAKE_GRENADE_SCALE  {nade_scale:.2f}f",
        text,
        count=1,
    )
    CL_UIRENDER.write_text(text)


def run_openmohaa(extra: str) -> None:
    # Play default is opengl1; bake must opt in via startup +set (see CL_InitRef).
    cmd = [
        str(BINARY),
        "+set",
        "cl_renderer",
        "opengl2",
        "+set",
        "cl_playintro",
        "0",
        "+set",
        "developer",
        "1",
        extra,
        "+quit",
    ]
    subprocess.run(cmd, cwd=GAMES, capture_output=True, text=True, timeout=120, check=False)


def bake_single(entry: BakeEntry, stage: str, scale: float | None = None) -> Path:
    PROBE_DIR.mkdir(parents=True, exist_ok=True)
    pitch = yaw = roll = 0.0
    if stage in ("rotate", "final", "scale") and entry.has_angles:
        pitch, yaw, roll = entry.angles
    fs = scale if scale is not None else (entry.framing_scale if entry.framing_scale > 0 else 0.5)
    ox, oy, oz = entry.offset
    cmd = (
        f"+ui_bake_model {entry.path} {entry.vfs_out} "
        f"{yaw:.2f} {pitch:.2f} {roll:.2f} {fs:.3f} 30 "
        f"{ox:.2f} {oy:.2f} {oz:.2f} 500 500"
    )
    run_openmohaa(cmd)
    return PROBE_DIR / entry.png_name


def probe_batch(stage: str) -> None:
    run_openmohaa(f"+ui_bake_mp_weapons_probe {stage} {PROBE_VFS}")


def final_batch(gun_scale: float, nade_scale: float) -> None:
    run_openmohaa(f"+ui_bake_mp_weapons {FINAL_VFS} {gun_scale:.3f} {nade_scale:.3f}")


def inspect_paths(*paths: Path, **kwargs) -> list[dict]:
    cmd = [sys.executable, str(INSPECT), "--json", str(paths[0]) if len(paths) == 1 else str(paths[0].parent)]
    for key, val in kwargs.items():
        if val is None:
            continue
        flag = key.replace("_", "-")
        cmd.extend([f"--{flag}", str(val)])
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if not proc.stdout.strip():
        return []
    return json.loads(proc.stdout)


def inspect_one_png(png: Path, **kwargs) -> dict | None:
    rows = inspect_paths(png, **kwargs)
    return rows[0] if rows else None


def center_error(r: dict | None) -> float:
    if not r or r.get("empty") or "error" in r:
        return 999.0
    return abs(r.get("cx", 0.5) - 0.5) + abs(r.get("cy", 0.5) - 0.5)


def is_pistol(path: str) -> bool:
    p = path.lower()
    return any(x in p for x in ("colt45", "p38", "revolver", "beretta", "webley", "nagant_revolver"))


def rotation_template(entry: BakeEntry) -> list[float]:
    if entry.kind == "NADE":
        return [0.0, 90.0, 0.0]
    if is_pistol(entry.path):
        return [0.0, 90.0, -90.0]
    if entry.kind == "MISC":
        return [0.0, 90.0, 0.0]
    return [0.0, 90.0, 90.0]


def tune_center(entry: BakeEntry, stage: str = "rotate", max_iters: int = 20) -> None:
    for i in range(max_iters):
        png = bake_single(entry, stage)
        r = inspect_one_png(png, margin=0, center_tol=0.03)
        err = center_error(r)
        if err <= 0.03:
            entry.has_offset = 1
            return
        if not r or r.get("empty"):
            return
        gain = max(8.0, 40.0 - i * 1.5)
        entry.offset[1] += (0.5 - r["cx"]) * gain
        entry.offset[2] += (0.5 - r["cy"]) * gain
        entry.has_offset = 1


def tune_scale(entry: BakeEntry, stage: str = "rotate", max_iters: int = 18) -> None:
    scale = entry.framing_scale if entry.framing_scale > 0 else 0.45
    for _ in range(max_iters):
        entry.framing_scale = scale
        png = bake_single(entry, stage, scale=scale)
        r = inspect_one_png(png, margin=8, center_tol=0.05, min_fill=0.18, max_fill=0.72)
        if not r or r.get("empty"):
            scale *= 1.08
            continue
        if r.get("clips"):
            scale *= 1.05
            continue
        if r.get("badFill"):
            if r.get("fillRatio", 0) > 0.72:
                scale *= 1.04
            else:
                scale *= 0.96
            continue
        if r.get("ok"):
            return
        scale *= 1.02
    entry.framing_scale = scale


def phase_center(entries: list[BakeEntry]) -> None:
    probe_batch("center")
    for e in entries:
        png = PROBE_DIR / e.png_name
        if not png.exists():
            continue
        r = inspect_one_png(png, margin=0, center_tol=0.03)
        if center_error(r) > 0.03:
            tune_center(e, stage="center", max_iters=12)


def phase_scale(entries: list[BakeEntry]) -> None:
    for e in entries:
        if not (PROBE_DIR / e.png_name).exists():
            bake_single(e, "center")
        tune_scale(e, stage="center")


def phase_rotate(entries: list[BakeEntry]) -> None:
    for e in entries:
        if not e.has_angles:
            e.angles = rotation_template(e)
            e.has_angles = 1
    probe_batch("rotate")
    for e in entries:
        if not (PROBE_DIR / e.png_name).exists():
            continue
        tune_center(e, stage="rotate", max_iters=14)
        tune_scale(e, stage="rotate", max_iters=10)


def phase_final(gun_scale: float, nade_scale: float) -> tuple[float, float]:
    for _ in range(25):
        final_batch(gun_scale, nade_scale)
        rows = inspect_paths(FINAL_DIR, margin=2, center_tol=0.05)
        if not rows:
            break
        clipped = [r for r in rows if r.get("clips") and not r.get("empty")]
        failed = [r for r in rows if not r.get("ok") and not r.get("empty")]
        if not clipped and len(failed) <= max(2, len(rows) // 8):
            return gun_scale, nade_scale
        # framingScale maps to camera distance: increase to shrink when clipping
        gun_scale *= 1.05
        nade_scale *= 1.05
    return gun_scale, nade_scale


def phase_finetune(entries: list[BakeEntry], gun_scale: float, nade_scale: float) -> None:
    rows = inspect_paths(FINAL_DIR, margin=2, center_tol=0.05)
    bad = {Path(r["path"]).name: r for r in rows if not r.get("ok") and not r.get("empty")}
    for e in entries:
        if e.png_name not in bad:
            continue
        tune_center(e, stage="rotate", max_iters=8)
        tune_scale(e, stage="rotate", max_iters=8)
    write_bake_list(entries)
    rebuild_and_deploy()
    phase_final(gun_scale, nade_scale)


def rebuild_and_deploy() -> None:
    build = REPO / "build-release"
    subprocess.run(
        ["cmake", "--build", str(build), "--target", "openmohaa", "-j", str(8)],
        check=False,
        capture_output=True,
    )
    dest = GAMES
    rel = build / "Release"
    for name in ("openmohaa", "cgame.so", "game.so", "renderer_opengl1.so", "renderer_opengl2.so"):
        src = rel / name
        if src.exists():
            subprocess.run(["cp", "-f", str(src), str(dest / name)], check=False)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("phase", choices=("all", "center", "scale", "rotate", "final", "finetune"))
    ap.add_argument("--gun-scale", type=float, default=0.40)
    ap.add_argument("--nade-scale", type=float, default=0.92)
    ap.add_argument("--skip-rebuild", action="store_true")
    args = ap.parse_args()

    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        return 1

    entries = parse_bake_list(BAKE_LIST.read_text())
    gs, ns = args.gun_scale, args.nade_scale

    if args.phase in ("all", "center"):
        print("=== phase center ===")
        phase_center(entries)
        write_bake_list(entries)
        rebuild_and_deploy()
    if args.phase in ("all", "scale"):
        print("=== phase scale ===")
        phase_scale(entries)
        write_bake_list(entries)
    if args.phase in ("all", "rotate"):
        print("=== phase rotate ===")
        phase_rotate(entries)
        write_bake_list(entries)
        if not args.skip_rebuild:
            rebuild_and_deploy()
    if args.phase in ("all", "final", "finetune"):
        print("=== phase final ===")
        if not args.skip_rebuild:
            rebuild_and_deploy()
        gs, ns = phase_final(gs, ns)
        update_cl_defaults(gs, ns)
        print(f"final scales gun={gs:.3f} nade={ns:.3f}")
    if args.phase in ("all", "finetune"):
        print("=== phase finetune ===")
        phase_finetune(entries, gs, ns)
    write_bake_list(entries)
    if args.phase == "center":
        probe_batch("center")
        rows = inspect_paths(PROBE_DIR, margin=8, center_tol=0.03)
    elif args.phase in ("scale", "rotate"):
        rows = inspect_paths(PROBE_DIR, margin=8, center_tol=0.05)
    else:
        rows = inspect_paths(FINAL_DIR, margin=2, center_tol=0.05)
    failed = [r for r in rows if not r.get("ok") and not r.get("empty")]
    print(f"inspect: {len(rows)} pngs, {len(failed)} failing")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
