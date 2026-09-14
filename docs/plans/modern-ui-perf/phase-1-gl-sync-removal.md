# Phase 1 — Remove GL sync points and redundant driver traffic (renderer GL1 + compositor)

Read `00-README.md` first. Precondition: Phase 0 overlay exists and the baseline table is filled.

## Goal

Zero synchronous GL queries per frame on the modern HUD path, no redundant framebuffer
binds / `Set2DWindow` / scissor calls, without changing a single output pixel.

## Why this is first

`RE_BeginUiLayer` (`code/renderergl1/tr_ui_layer.c:161-162`) executes
`qglIsEnabled(GL_SCISSOR_TEST)` and `qglGetIntegerv(GL_SCISSOR_BOX)` for every soft
`mask-image` element every frame (compass body always, damage pip when visible).
`DrawBox` (`tr_draw.c:396`) and the UI batch begin when no MSAA FBO is active
(`tr_ui_batch.c:74,147`) execute `qglIsEnabled(GL_MULTISAMPLE)`. On the Windows NVIDIA driver
with Threaded Optimization these are round-trip stalls that also serialize the queued 3D
frame; on Linux they are cheap. Removing them is small, safe, and testable
(`gets` counter on the overlay must read 0).

## Scope

Files: `code/renderergl1/tr_backend.c`, `tr_local.h`, `tr_init.c`, `tr_draw.c`, `tr_ui_fbo.c`,
`tr_ui_layer.c`, `tr_ui_stencil.c`, `tr_ui_batch.c`; `code/uirender/uir_draw2d.c`,
`uir_compositor.c`, `uir_batch.c`; `code/client/cl_uirender.cpp` (cvar registration only).

Non-goals: no change to MSAA, FBO sizes, blit extents, blend modes, draw order.

## Design

### 1.1 Software-tracked GL state (replaces glGet/glIsEnabled)

Extend `glstate_t` in `code/renderergl1/tr_local.h` (~line 1241):

```c
	/* Added in Omaha: tracked state so UI paths never call glGet*/glIsEnabled. */
	qboolean	scissorEnabled;
	int			scissorBox[4];      /* x, y, w, h as last passed to glScissor */
	qboolean	multisampleEnabled; /* GL_MULTISAMPLE enable bit */
	GLuint		fboDraw;            /* current GL_DRAW_FRAMEBUFFER binding (0 = window) */
	GLuint		fboRead;            /* current GL_READ_FRAMEBUFFER binding */
	qboolean	fboKnown;           /* qfalse forces the next bind through */
```

Add wrappers in `code/renderergl1/tr_backend.c` next to `GL_State` (~line 219), declared in
`tr_local.h`:

```c
void GL_Scissor(int x, int y, int w, int h);        /* qglScissor only if box changed */
void GL_ScissorEnable(qboolean enable);              /* qglEnable/Disable(GL_SCISSOR_TEST) only if changed */
void GL_MultisampleEnable(qboolean enable);          /* qglEnable/Disable(GL_MULTISAMPLE) only if changed (no-op if GL_MULTISAMPLE undefined) */
void GL_BindFramebuffer(GLenum target, GLuint fbo);  /* GL_FRAMEBUFFER sets both halves; READ/DRAW set one; skips if unchanged and fboKnown */
void GL_InvalidateFramebufferBinding(void);          /* sets fboKnown = qfalse; call after glDeleteFramebuffers */
```

Counting: the wrappers are where `tr_uiStats.scissorCalls` / `fboBinds` increment (Phase 0
placed them at the raw call sites; move the increments into the wrappers so the overlay shows
**issued** calls after dedup).

Rules for the wrappers:
- `GL_Scissor`: compare with `glState.scissorBox`; if equal, return without calling GL.
- `GL_ScissorEnable`: compare with `glState.scissorEnabled`.
- `GL_BindFramebuffer`: if `!glState.fboKnown` → call GL, set both halves from the target,
  `fboKnown = qtrue`. Else if the requested half(s) already equal → return.
  When `qglBindFramebuffer` is NULL (no FBO support) → return.
- `GL_MultisampleEnable`: compiled to a no-op when `GL_MULTISAMPLE` is not defined.

Initialization in `GL_SetDefaultState` (`tr_init.c:1038`): after the existing
`qglEnable(GL_SCISSOR_TEST)` (line ~1075), set `glState.scissorEnabled = qtrue`,
`glState.scissorBox = {0,0,glConfig.vidWidth,glConfig.vidHeight}` and issue
`qglScissor(0,0,vidWidth,vidHeight)` once so tracked == actual; set
`glState.multisampleEnabled = qtrue` (GL default is enabled) and issue
`qglEnable(GL_MULTISAMPLE)` once under `#ifdef GL_MULTISAMPLE`; set `fboDraw = fboRead = 0`,
`fboKnown = qtrue` and issue `qglBindFramebuffer(GL_FRAMEBUFFER, 0)` once if the proc exists.
`GL_SetDefaultState` runs on every `vid_restart`, so tracking re-syncs there.

Route every existing call site through the wrappers. Grep in `code/renderergl1` for
`qglScissor(`, `GL_SCISSOR_TEST`, `GL_MULTISAMPLE`, `qglBindFramebuffer(` and replace each one
(current sites: `tr_backend.c` SetViewportAndScissor; `tr_draw.c` `Set2DWindow`, `RE_Scissor`,
`DrawBox`; `tr_ui_fbo.c` `RE_BeginUI2DTarget`, `RE_EndUI2DTarget`, `RE_UI2DTargetRebind`,
`RE_UI2D_FboEnsure`, `RE_UI2D_FboShutdown`; `tr_ui_layer.c` layer begin/apply/end, chrome cache
ensure/capture/blit/shutdown, `RE_UiLayerRebind`; `tr_ui_stencil.c` `RE_BeginUiStencilMask`;
`tr_ui_batch.c` batch begin/end/inner). After every `qglDeleteFramebuffers` call
`GL_InvalidateFramebufferBinding()` (GL unbinds a deleted FBO implicitly). Inside
`RE_UI2D_FboEnsure` / `RE_UiLayer_Ensure` / chrome-cache ensure (creation paths), keep using
the wrapper — creation binds are rare.

Then:
- `RE_BeginUiLayer`: replace lines 161-162 with
  `s_uiLayer.savedScissorEnabled = glState.scissorEnabled; memcpy(s_uiLayer.savedScissor, glState.scissorBox, sizeof(...))`.
  Keep the restore logic in `RE_EndUiLayer` (now via wrappers).
- `RE_UI2DBatchBegin`, `RE_DrawUI2D_Inner`, `DrawBox`: replace `qglIsEnabled(GL_MULTISAMPLE)`
  with `glState.multisampleEnabled` and the enable/disable pairs with `GL_MultisampleEnable`.

A/B toggle: cvar `r_uiSyncQueries` (`R_Register`, `CVAR_ARCHIVE`, default `"0"`). When `1`,
the three sites use the old `qglIsEnabled`/`qglGetIntegerv` reads (keep the old code in an
`if (r_uiSyncQueries->integer)` branch). This exists purely so the user can flip it in-game and
watch the `gets` counter and `frame us` move.

### 1.2 Redundant FBO binds

Automatic through `GL_BindFramebuffer`. Today `RE_UI2DTargetRebind` is called from every
`Set2DWindow`, batch begin, `DrawBox`, stencil begin, `Draw_StretchPic` etc. and rebinds the
same MSAA FBO. Expected effect: `fbo` counter drops from ~8-12 to ~3-4 per HUD frame
(target bind, layer bind + back per mask, resolve read/draw + 0).

### 1.3 Compositor: skip redundant `Set2DWindow` and the empty preview phase

`code/uirender/uir_draw2d.c` `UIR_Draw2D_Begin` (line ~76): add a static
`g_d2dApplied` (viewport copy + valid flag). If `ui_d2d_dedup` is on and the incoming `vp`
equals the applied one and the flag is valid → skip `set2DWindow` and `scissor`, but still
`UIR_BatchFlush()` and `UIR_InvalidateAppliedClip()` are NOT needed (nothing changed) — only
skip when nothing else invalidated it. Invalidate the flag (`UIR_Draw2DInvalidate()`):
- in `UIR_BatchBeginFrame` (`uir_batch.c`) — start of every frame;
- in `UIR_BatchTargetBegin` / `UIR_BatchTargetEnd` / `uir_batch_ensure_target` after the
  backend call (the renderer's `RE_EndUI2DTarget` calls `Set2DWindow` itself with FB coords);
- in `UIR_BeginImageMask` / `UIR_EndImageMask` (`uir_layer.c`), `UIR_BeginSvgShapeClip` /
  `UIR_EndShapeClip` (`uir_stencil.c`), chrome-cache capture begin/end/blit (`uir_compositor.c`);
- in `UIR_ModelPreviewDraw` / `UIR_MenuWorldDraw` entry (they change projection).
Expose `UIR_Draw2DInvalidate()` in `uir_draw2d.h`. The compositor's `uir_restore_fullscreen_2d`
keeps calling `UIR_Draw2D_Begin` + `UIR_ResetClipStack`; `UIR_ResetClipStack` already dedups
via `g_appliedClipValid` — but it force-sets `g_appliedClipValid = 0`; change it to only
reset when the dedup flag says the applied clip differs (compare rects before forcing).

`uir_compositor.c` `UIR_EndOverlayFrame` (line ~493): when `g_previewCount == 0`, skip the
preview block entirely (both `uir_restore_fullscreen_2d()` calls and the phase switch) —
go straight to `UIR_BatchTargetBegin()` for the overlay phase. Keep behaviour identical when
previews exist. Toggle: `ui_d2d_dedup` (`CL_UIR_RegisterCvars`, `CVAR_ARCHIVE`, default `"1"`),
passed to uirender via a setter `UIR_Draw2DSetDedup(int)` like `UIR_SetClipDedup`.

### 1.4 Batch draws: `glDrawRangeElements`

Client-array `glDrawElements` makes the driver scan the index array to find the vertex range
before copying. `RE_DrawUI2D_Inner` knows the range (`0 .. numVerts-1`). Add
`GLE(void, DrawRangeElements, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices)`
to a new `QGL_1_2_PROCS` list in `qgl.h` loaded like `QGL_1_3_PROCS` in `sdl_glimp.c`
(desktop GL >= 1.2 always has it). In `RE_DrawUI2D_Inner` and `RE_DrawUiStencilMaskTris` use
`qglDrawRangeElements(GL_TRIANGLES, 0, numVerts - 1, numIndexes, GL_UNSIGNED_SHORT, indexes)`
when the proc is non-NULL, else fall back to `qglDrawElements`. No toggle needed (it is
semantically identical); Phase 3 may replace the whole path with a VBO.

## Steps

1. Add `glstate_t` fields + wrappers + init in `GL_SetDefaultState`. Build.
2. Route all call sites listed in 1.1 through the wrappers; move Phase 0 counters into the
   wrappers. Build. Run a map: HUD must look identical; `fbo` and `scissor` counters drop.
3. Replace the three query sites; add `r_uiSyncQueries`. Build. Verify `gets` reads 0 with the
   cvar at 0 (also during a damage flash / fade so `DrawBox` is exercised, and with the
   damage pip visible so two masks are active) and returns to the baseline value at 1.
4. 1.3 compositor dedup + preview skip behind `ui_d2d_dedup`. Build. Verify `set2d` per frame
   drops (baseline ~4 → 2: one in `UIR_BeginOverlayFrame`, one inside `RE_EndUI2DTarget`)
   and `issue` drops accordingly. Open the pause menu (model previews) and the main menu to
   confirm nothing regressed when previews exist.
5. 1.4 `glDrawRangeElements`. Build.
6. Deploy; run the measurement protocol; fill Results; request sign-off.

## Verification checklist (visual)

- Compass tape fade edges intact (soft mask still applied), damage pip fade intact.
- Weapons bar skew shapes, health pips, kill feed, chat, objectives, scoreboard (hold TAB),
  pause menu with player model preview, main menu with 3D world — all identical to before.
- `vid_restart` mid-match, then check again (state re-sync).
- `r_uiFramebuffer 0` still works (non-FBO path uses the multisample tracking branch).

## Acceptance

- `gets` = 0 in Still/Look/Busy with `r_uiSyncQueries 0`.
- `fbo` <= 4 and `set2d` <= 2 per frame in Still.
- No pixel changes (spot-check `ui_compare_shot` before/after at the same spot).
- Results table filled; delta vs Phase 0 baseline recorded.

## Pitfalls

- Never call `qglScissor`/`qglEnable(GL_SCISSOR_TEST)`/`qglBindFramebuffer` directly anymore
  in `renderergl1`; grep before finishing. Missing one site desynchronizes tracking and
  causes clipped/black UI.
- `RE_EndUI2DTarget` binds `GL_READ_FRAMEBUFFER` and `GL_DRAW_FRAMEBUFFER` separately for the
  blit, then `GL_FRAMEBUFFER 0`. The wrapper must handle the per-half update correctly.
- The legacy (`ui_legacy 1`) renderer paths (`RB_BeginDrawingView`, `SetViewportAndScissor`)
  also go through `GL_Scissor` now — verify legacy HUD still renders and the console scissor
  works.
- Do not touch `GL_State` blend handling.

## Results

Build: 2026-09-12 ~23:32 `_build_deploy` (set2d/fbo gate fixes + site auto-capture).  
Source: `e:\Dev\Omaha\debug-b55c7e.log` (`runId=phase1-sites`, modern HUD).  
Visual: user confirmed normal (incl. compass soft fade) after in-place mask.

### Post-fix Still (stable windows, median)

| Metric | Before fix | After fix | Gate |
|--------|------------|-----------|------|
| gets | 0 | **0** | ≤0 **PASS** |
| set2d | 6 | **2** (`s2dBegin=1` + `s2dEnd=1`) | ≤2 **PASS** |
| fbo | 5 | **3** (`beg+dr+0`; `fboLay=0` `fboReb=0`) | ≤4 **PASS** |

Site anatomy after fix (Still): `tgtB=1` `tgtE=1` `layers=1` (in-place, no layer FBO hop).

Busy sample (same session): `gets=0` `set2d=2` `fbo=3`.

### Medians (modern, Phase 1 defaults) — gate-pass session

| Scenario | HUD | cvar state | frame us | ui total | draws | scissor | fbo | gets | set2d | gpu ui |
|----------|-----|------------|----------|----------|-------|---------|-----|------|-------|--------|
| Still | modern | new | ~8000* | ~550 | ~29 | ~14 | **3** | **0** | **2** | off |
| Busy | modern | new | ~8000* | ~840 | ~56 | ~37 | **3** | **0** | **2** | off |

\*Session was FPS-limited (~125); GL counters are the Phase 1 gate.

### Delta vs Phase 0 baseline (modern FBO on Still)

| Metric | Phase 0 Still | Phase 1 Still (gate pass) | Δ |
|--------|---------------|---------------------------|---|
| gets | 73 | **0** | −73 |
| fbo | 8 | **3** | −5 |
| set2d | 17 | **2** | −15 |

### Gate status

| Item | Status |
|------|--------|
| `gets` = 0 | **PASS** |
| `fbo` ≤ 4 Still | **PASS** (3) |
| `set2d` ≤ 2 Still | **PASS** (2) |
| Visual (compass fade / HUD) | **PASS** (user: everything looks normal) |
| User sign-off | **Approved 2026-09-12** (proceed to Phase 2) |

### Fixes landed during Phase 1 verify (keep)

- Soft-tracked GL wrappers + `r_uiSyncQueries` / `ui_d2d_dedup` / `glDrawRangeElements` (plan)
- Per-menu `UIR_ResetClipStack` + `UIR_BatchCloseDrawSession` (scoreboard→HUD clip/blend isolation)
- FBO `BlendFuncSeparate` restore after `GL_State` / UI batch (score brightness)
- Scoped layout: `%` uses `percentBase*` not assigned box (`uid_layout.cpp`) — score row left shift
- Gate close: View3D/gmbox/dmbox skip wasted `Set2D`; `Set2DWindow` dedup; soft-mask in-place on UI MSAA

Debug instrumentation (auto-capture to `debug-b55c7e.log`, site tallies, forced cvars, scenario auto-tag) removed after sign-off.

## Todo

- [x] 1.1–1.4 plan items
- [x] Measurement protocol + gate `gets`/`fbo`/`set2d`
- [x] User sign-off (Approved 2026-09-12; debug instrumentation removed)
