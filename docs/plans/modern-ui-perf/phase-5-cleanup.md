# Phase 5 — Cleanup, defaults, documentation

Read `00-README.md` first. Precondition: the user has signed off Phases 1-4 (or explicitly
skipped some) and the final protocol run meets the targets in `00-README.md` section 3.

## Goal

Leave the codebase with one clear path per feature, documented knobs, and the measurement
overlay kept as a permanent zero-cost tool.

## Steps

1. For every cvar in the registry (`00-README.md` section 5) decide with the user, per item:
   - **Keep as knob** (default = new behaviour, old path stays): only where a real fallback is
     useful (`r_uiVbo` for drivers without buffer procs, `r_uiResolveRects` debug value 2,
     `ui_perf_hud`, `ui_perf_gpu`).
   - **Remove toggle, keep new path**: delete the old branch and the cvar
     (`r_uiSyncQueries`, `ui_d2d_dedup`, `r_uiClearMode`, `ui_replay_clip_dedup`,
     `ui_paint_regions`, `ui_live_translate_cache`, `ui_live_opacity_cache`, `ui_bind_deps`,
     `cg_hud_push_cache`) once the user confirms they never want to flip them again.
   - **Remove entirely** (feature rejected): e.g. `ui_batch_softclip` if not approved,
     `ui_live_opacity_cache` if the alpha LSB deviation was rejected. Delete the code, not just
     the cvar.
   `ui_bind_deps_verify` is removed (debug only) unless the user wants to keep it.
2. Remove any temporary debug prints introduced during the phases (grep `PRINT_DEVELOPER`
   additions from this program, `TODO(perf)` markers).
3. Update docs:
   - `docs/LLM-helpers/ui-rendering-pipeline.md`: new section "Performance architecture"
     describing tracked GL state (no `glGet*` rule), region-limited resolve, per-region paint
     chunks, live translate/opacity caches, dependency-indexed bind sync, and the `ui_perf_hud`
     overlay with the meaning of each field. Add the surviving cvars to the cvar table there
     (~line 150).
   - `docs/LLM-helpers/designformat.md`: a short note for HUD authors: which constructs stay
     cheap under caching (bound `translate-x/y`, foreach lifetime fades, text changes inside a
     region) and which force a full re-record (`STRUCTURE` changes, masks/shape clips inside
     animated subtrees).
   - Move this folder's Results tables into a single `RESULTS.md` (baseline → final) so the
     numbers survive.
4. Final protocol run on the clean build; record in `RESULTS.md`; confirm targets.

## Acceptance

- `rg -n "r_uiSyncQueries|ui_d2d_dedup|..."` finds only the knobs the user chose to keep.
- Docs updated; `ui_perf_hud 1` still works; `ui_perf_hud 0` measurably free.
- Final numbers recorded.

## Todo

- [ ] Per-cvar decision with user, code removed/kept accordingly
- [ ] Temporary debug output removed
- [ ] Docs updated (pipeline, designformat, RESULTS.md)
- [ ] Final protocol run recorded
