# Display refresh rate options (`r_displayRefresh`)

**Research date:** 2026-08-31  
**Scope:** What values to expose for modern UI Refresh Rate (`display-refresh` / `r_displayRefresh`), and how to source them.

## Executive summary

Do **not** ship a fixed curated Hz list that tops out at 200. Common gaming panels report **240 Hz** (and often 360+); the legacy `Cvar_CheckRange(..., 0, 200)` also blocked those values even if the UI listed them.

**Recommended direction:** host-back `display-refresh` from **SDL display modes** on the active display (unique positive `refresh_rate` values + **Default / 0**), and raise the latch cvar max so those rates can be applied. This matches how OS display settings and most engines populate the control.

## Verified findings

### Engine behavior (this repo)

- `r_displayRefresh` is `CVAR_LATCH`; historically checked to **0–200** in `code/renderergl1/tr_init.c` and `code/renderergl2/tr_init.c`.
- Exclusive fullscreen only: `code/sdl/sdl_glimp.c` passes the cvar into `SDL_DisplayMode.refresh_rate` via `SDL_SetWindowDisplayMode`.
- SDL documents `refresh_rate` as Hz, or **0 for unspecified** (`SDL_video.h` / [SDL_DisplayMode](https://wiki.libsdl.org/SDL2/SDL_DisplayMode)).
- Renderer already enumerates SDL modes in `GLimp_DetectAvailableModes` but **dedupes by resolution only** (drops distinct refresh rates for `r_availableModes`).

### Industry practice

- Windows Settings / advanced display UI lists rates the **driver/EDID report for that monitor** (often 60, 100, 120, 144, 165, 240, …), not a single global product catalog ([Display EX setup notes](https://display-ex.github.io/high-refresh-rate-144hz-setup.html); Win32 `EnumDisplaySettingsEx` / DXGI mode lists).
- Unity and similar engines expose `Screen.resolutions` from OS mode enumeration rather than a hardcoded Hz table ([Unity refresh-rate discussion](https://discussions.unity.com/t/refresh-rate-rounding-on-windows/887788)).
- SDL2 provides the same data via `SDL_GetNumDisplayModes` / `SDL_GetDisplayMode` ([SDL2 wiki](https://wiki.libsdl.org/SDL2/SDL_GetNumDisplayModes)).

### Common marketed tiers (context only)

Widely sold panels: **60, 75, 100, 120, 144, 165, 180, 200, 240, 360**; specialty panels go to **480–720 Hz**. These are useful as a **fallback** if SDL returns nothing, not as the sole catalog ([CORSAIR comparison](https://www.corsair.com/us/en/explorer/gamer/monitors/60hz-vs-120hz-vs-144hz-vs-165hz-vs-240hz-refresh-rates-compared/); vendor marketing for 360+).

### Upstream note

ioquake3 removed `r_displayRefresh` in places after it was poorly connected; this fork still uses it for exclusive fullscreen. Effectiveness still depends on OS/driver accepting the requested mode ([ioq3 issue #433](https://github.com/ioquake/ioq3/issues/433)).

## Alternatives considered

| Approach | Pros | Cons | Decision |
|----------|------|------|----------|
| Static XML list (≤200 Hz) | Simple | Omits 240+; fights latch max; incomplete forever | Rejected as primary |
| Expanded static list (to 360/720) | Easy | Still misses odd EDID rates; stale | Fallback only |
| Host SDL unique rates + Default(0) | Matches hardware; includes 240 on user’s display | Needs video init; display-dependent | **Chosen** |

## Constraints

- Cyclic UI source must be **host-backed** (not XML), otherwise XML wins over `queryCollectionItems` (`docs/LLM-helpers/designformat.md`).
- Register id in `IsHostCollectionSource` (`code/uidesign/uid_compile.cpp`).
- Latch max must allow at least common competitive rates (raise well above 200; e.g. 1000).

## Validation

- On a 240 Hz monitor, Refresh Rate cyclic includes **240 Hz** after menu load.
- Setting 240 + Apply in exclusive fullscreen issues `vid_restart` and does not clamp the cvar.
- Default (0) remains first and means SDL “unspecified” refresh.

## Sources

- [SDL2 SDL_DisplayMode](https://wiki.libsdl.org/SDL2/SDL_DisplayMode)
- [SDL2 SDL_GetNumDisplayModes](https://wiki.libsdl.org/SDL2/SDL_GetNumDisplayModes)
- [Microsoft EnumDisplaySettingsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaysettingsexw)
- [CORSAIR: 60–240 Hz compared](https://www.corsair.com/us/en/explorer/gamer/monitors/60hz-vs-120hz-vs-144hz-vs-165hz-vs-240hz-refresh-rates-compared/)
- [ioquake3 r_displayRefresh issue #433](https://github.com/ioquake/ioq3/issues/433)
- In-repo: `code/sdl/sdl_glimp.c`, `code/renderergl1/tr_init.c`, `code/renderergl2/tr_init.c`
