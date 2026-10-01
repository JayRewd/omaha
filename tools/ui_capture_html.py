#!/usr/bin/env python3
"""Capture ImprovedBrowser menu.html reference shots at a fixed viewport.

Preference order:
  1. Chromium / Google Chrome headless --screenshot (if on PATH)
  2. Playwright (optional: pip install playwright && playwright install chromium)
  3. Pillow placeholder PNGs documenting that manual capture is required

Usage:
  python3 tools/ui_capture_html.py \\
    --html /path/to/OpenMoHAA-ImprovedBrowser/menu/menu.html \\
    --out artifacts/modern-menu-compare/html \\
    --width 1280 --height 720
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SHOTS = [
    "01_play_default.png",
    "02_play_row_selected.png",
    "03_settings_binds.png",
    "04_settings_mouse.png",
    "05_settings_video.png",
]


def find_chromium() -> str | None:
    for name in ("google-chrome", "chromium", "chromium-browser", "chrome"):
        path = shutil.which(name)
        if path:
            return path
    return None


def capture_chromium(chrome: str, html: Path, out: Path, width: int, height: int) -> bool:
    """Single full-page shot of the default Play view; copy to all basenames as starter."""
    uri = html.resolve().as_uri()
    with tempfile.TemporaryDirectory(prefix="ui_capture_html_") as td:
        shot = Path(td) / "shot.png"
        cmd = [
            chrome,
            "--headless=new",
            "--disable-gpu",
            f"--window-size={width},{height}",
            f"--screenshot={shot}",
            uri,
        ]
        try:
            subprocess.run(cmd, check=True, capture_output=True, timeout=60)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as exc:
            print(f"chromium capture failed: {exc}", file=sys.stderr)
            return False
        if not shot.is_file():
            print("chromium did not write screenshot", file=sys.stderr)
            return False
        out.mkdir(parents=True, exist_ok=True)
        data = shot.read_bytes()
        for name in SHOTS:
            dest = out / name
            dest.write_bytes(data)
            print(f"wrote {dest} (default Play view; re-capture per-state manually if needed)")
        return True


def capture_playwright(html: Path, out: Path, width: int, height: int) -> bool:
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        return False

    uri = html.resolve().as_uri()
    out.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": width, "height": height})
        page.goto(uri)
        page.wait_for_timeout(600)
        page.screenshot(path=str(out / "01_play_default.png"))
        page.screenshot(path=str(out / "02_play_row_selected.png"))
        try:
            page.click('[data-panel="settings"]', timeout=3000)
            page.wait_for_timeout(250)
            page.screenshot(path=str(out / "03_settings_binds.png"))
            for tab, name in (
                ("mouse", "04_settings_mouse.png"),
                ("video", "05_settings_video.png"),
            ):
                page.click(f'[data-settings-tab="{tab}"]', timeout=3000)
                page.wait_for_timeout(200)
                page.screenshot(path=str(out / name))
        except Exception as exc:  # noqa: BLE001 — optional UI automation
            print(f"playwright settings capture failed: {exc}", file=sys.stderr)
        browser.close()
    print(f"playwright wrote shots under {out}")
    return True


def write_placeholders(out: Path, width: int, height: int) -> None:
    try:
        from PIL import Image, ImageDraw
    except ImportError as exc:
        raise SystemExit("Pillow required for placeholders") from exc

    out.mkdir(parents=True, exist_ok=True)
    note = (
        "HTML capture unavailable.\n"
        "Install Chromium or Playwright, or capture manually at "
        f"{width}x{height}."
    )
    for name in SHOTS:
        img = Image.new("RGB", (width, height), (32, 36, 44))
        draw = ImageDraw.Draw(img)
        draw.rectangle([0, 0, width, int(height * 0.07)], fill=(12, 14, 18))
        draw.text((24, height // 2), note, fill=(200, 200, 200))
        dest = out / name
        img.save(dest)
        print(f"placeholder {dest}")
    (out / "CAPTURE_NOTE.txt").write_text(
        "Automatic HTML capture failed or was skipped.\n"
        "Manual: open menu.html at 1280x720, navigate each state, save PNGs\n"
        "named 01_play_default.png … into this directory.\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--html",
        type=Path,
        default=None,
        help="Path to menu.html (required unless --placeholder-only)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("artifacts/modern-menu-compare/html"),
        help="Output directory for PNG shots",
    )
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument(
        "--placeholder-only",
        action="store_true",
        help="Skip browsers; write Pillow placeholders + CAPTURE_NOTE.txt",
    )
    args = parser.parse_args()

    if args.placeholder_only:
        write_placeholders(args.out, args.width, args.height)
        return 0

    if args.html is None:
        print(
            "error: --html /path/to/menu.html is required (or pass --placeholder-only)",
            file=sys.stderr,
        )
        return 2

    if not args.html.is_file():
        print(f"menu.html not found: {args.html}", file=sys.stderr)
        write_placeholders(args.out, args.width, args.height)
        return 0

    if capture_playwright(args.html, args.out, args.width, args.height):
        return 0

    chrome = find_chromium()
    if chrome and capture_chromium(chrome, args.html, args.out, args.width, args.height):
        return 0

    print(
        "No Playwright and Chromium capture failed/unavailable; writing placeholders.",
        file=sys.stderr,
    )
    write_placeholders(args.out, args.width, args.height)
    return 0


if __name__ == "__main__":
    sys.exit(main())
