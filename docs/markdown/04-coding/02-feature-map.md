# Feature map for cherry-picking (history-v2)

Commits after `a2f3401` are stacked so each feature can be reviewed or
cherry-picked with explicit dependencies.

## Independent / early

| Commit | Notes |
|--------|-------|
| `chore: stop tracking build/ omtests assets` | Hygiene |
| `chore: establish Project: Omaha identity` | Branding only |
| `feat(config): omahaconfig, portable homepath, first-run defaults` | Needs identity |
| `fix(win32): keep nested Sys_ListFilteredFiles subdirectory paths` | Standalone |
| `fix(sound): OpenAL local-listener dry path, gain cap, WAV data size` | Standalone |
| `fix(sound): roll over capped sound voices` | Needs OpenAL fix |
| `fix(renderer): guard unallocated debug-line buffers` | Standalone |
| `feat(gamespy): scheduled pipelined internet server discovery` | Standalone (FAKK browser too) |

## Gameplay (cgame)

| Commit | Depends-on |
|--------|------------|
| `feat(cgame): remote player prediction` | debug-line guards |
| `feat(cgame): client-side first-person spectate` | — |
| `feat(cgame): visible and audible hitmarkers` | OpenAL local-listener fix |
| `feat(crosshair): procedural dynamic crosshair` | — (settings UI later) |

## Modern UI stack (take as a series)

| Commit | Depends-on |
|--------|------------|
| `feat(renderer): UI rendering backend` | — |
| `feat(ui): modern XML UI, HUD packs and menus` | config, gamespy, hitmarkers, crosshair, renderer UI. Internal UI/HUD state lives in the client-only `cl_uivars` store, not console cvars. Includes MP stabilize fixes that do not require the dirty paint layer (kill-feed icons, settings/video modes, HUD/host coherence, assets). |
| `perf(ui): scoped dirty layout/paint and retained HUD caches` | modern UI. Single perf commit: dirty-tracking paint/style extraction **and** retained FBO/live caches / `cl_uiperf` / restart-mask fixes. |
| `chore(config): purge cvars left by pre-release builds` | remote prediction, modern UI. Optional: only cleans up old `omahaconfig` entries (UI state names, former browser/spectate knobs). Skip it if you never ran a pre-release build. |

## CI / docs (trailing)

| Commit | Notes |
|--------|-------|
| `build(windows): Win7 API floor, ARM64 exempt` | cmake |
| `ci: release builds skip tests; unit tests and CodeQL on demand` | — |
| `ci: ship modern UI assets in zip, tarball and MSI packages` | needs modern UI + Win7 cmake |
| `ci: stop publishing the rolling dev release` | — |
| `docs: public README with screenshots` | — |

## Intentionally not in this history

- Server `CopyHudCombatStats` / `om_hud` userinfo path (`code/fgame/player.*`)
- `code/server/sv_main.c` frame-timing always-on sampling
- Internal research under `docs/plans/modern-ui-perf/` and agent-only LLM helper README (format docs live under `docs/modern-ui/`)
