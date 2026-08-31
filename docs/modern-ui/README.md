# Modern UI engine

Project: Omaha’s menus, HUDs, scoreboard, and pause shell are driven by a
**declarative, retained-mode UI stack**—not the old imperative UIFAKK menu
scripts. Layout and look live in XML under `assets/main/ui/modern/`; the
engine compiles that into widgets, lays them out with a flex-style system, and
paints them through a batched compositor that can still show the 3D menu world
and model previews behind the chrome.

## Why it matters

Retail MOHAA UI is hard to extend without engine surgery. The modern stack makes
client presentation a **data problem**:

- **Ship UI like content** — menus and HUD packs are XML (+ textures) in the VFS.
  Players pick Classic / Modern / Competitive in settings; mods drop new packs
  under `ui/modern/huds/` and they show up after reload—no custom `game.so`.
- **One language for menus and HUD** — the same layout, binds, and templates
  power the disconnected main menu, in-match overlay, scoreboard, and pause.
  HUD packs declare their own pause and scoreboard companions so Escape / TAB
  stay consistent with the pack you chose.
- **Designed for stock servers** — game state is mirrored into UI-facing cvars
  and bindings on the client. Presentation can evolve without changing what a
  retail dedicated server understands on the wire.
- **Performance-conscious draw path** — chrome is batched; world and model
  views composite into the UI layer instead of a pile of one-off draw calls.
- **Author once, scale cleanly** — flex layout, imports, templates, and design
  tokens keep large surfaces (settings, browser, HUDs) maintainable instead of
  copy-pasted rectangle math.

## What you can build with it

| Surface | Examples |
|---------|----------|
| Menus | Play / settings / browser, modals, keybinds, video options |
| HUD packs | Full-screen in-match chrome; pack-specific pause & scoreboard |
| Overlays | Hold-TAB scoreboard, kill feed, center print, spectator text |
| Previews | Crosshair / sniper host regions, player models in menus |

Assets live in `assets/main/ui/modern/` (copied into the client build’s
`main/ui/modern/`; sync there when testing outside the CMake tree).

## Dig deeper

**Canonical implementer / agent contracts:**

| Document | Role |
|----------|------|
| [designformat.md](../LLM-helpers/designformat.md) | Version-1 XML design format (`uidesign` + assets) |
| [ui-rendering-pipeline.md](../LLM-helpers/ui-rendering-pipeline.md) | `uirender` compositor phases and draw primitives |
| [LLM-helpers overview](../LLM-helpers/README.md) | Index of those contracts |

**Also useful:**

| Document | Role |
|----------|------|
| [allied-logo-shape.md](allied-logo-shape.md) | Allied star as generic SVG shape data |
| [`code/uirender/LIFECYCLE.md`](../../code/uirender/LIFECYCLE.md) | Runtime lifecycle (startup, connect, `vid_restart`) |

Legacy path: set `ui_legacy 1` to keep stock UIFAKK menus. Default Omaha play
uses this modern stack.
