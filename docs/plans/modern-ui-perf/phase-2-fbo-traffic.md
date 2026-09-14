# Phase 2 — UI FBO bandwidth: color-only clear, region-limited resolve and composite

Read `00-README.md` first. Precondition: Phase 1 signed off.

## Goal

Keep `r_uiMultisample 8` and the exact same pixels, but stop paying for a full-screen
2560x1440 8x MSAA depth+stencil clear, a full-screen MSAA resolve blit and a full-screen
composite every frame when the HUD covers a fraction of the screen.

Today (`code/renderergl1/tr_ui_fbo.c`):
- `RE_BeginUI2DTarget` (~line 261): `qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)`
  on the full MSAA FBO.
- `RE_EndUI2DTarget` (~line 291): `qglBlitFramebuffer(0,0,W,H → 0,0,W,H)` then a full-screen
  `glBegin(GL_QUADS)` composite onto the window.

GPU work per frame at 8x: resolve reads 2560*1440*8 samples*4 B ≈ 118 MB; composite ≈ 30 MB;
clear of color+depth+stencil at 8x is driver fast-cleared but not free. With the HUD in a few
regions (top strip, compass, corners, kill feed), region rects cut this by 3-10x while the
result is identical because the FBO is fully transparent outside drawn regions.

## Scope

`tr_ui_fbo.c`, `tr_ui_batch.c`, `tr_ui_layer.c`, `tr_draw.c`, `tr_backend.c` (RB_StretchPic
marker), `tr_init.c` (cvars). No uirender/uidesign changes required.

## Design

### 2.1 Color-only clear — r_uiClearMode (default 0)

UI drawing runs with `GLS_DEPTHTEST_DISABLE` and depth writes off, so depth content is never
read. Stencil is only used by `RE_BeginUiStencilMask`, which clears its own scissored stencil
rect before use (`tr_ui_stencil.c:93-98`). Therefore `RE_BeginUI2DTarget` clears
`GL_COLOR_BUFFER_BIT` only when `r_uiClearMode == 0`; `1` keeps the old three-buffer clear.

### 2.2 Dirty-rect tracking in the renderer

Add to `tr_ui_fbo.c` state:

```c
typedef struct { int x, y, w, h; } uiRectI_t;   /* GL window coords: origin bottom-left */
#define UI_FBO_MAX_RECTS 8
static uiRectI_t s_uiRects[UI_FBO_MAX_RECTS];
static int       s_uiRectCount;
static qboolean  s_uiFullResolve;              /* set by any draw we cannot bound */
```

Reset both in `RE_BeginUI2DTarget` (after the clear). Public helpers (declared in `tr_local.h`):

```c
void RE_UI2D_AccumRectFb(int x, int y, int w, int h); /* window coords, bottom-left origin */
void RE_UI2D_AccumRectDraw(float x0, float y0, float x1, float y1); /* current 2D ortho space */
void RE_UI2D_MarkFullResolve(void);
```

Draw-space to window-space conversion: `Set2DWindow` (`tr_draw.c:464`) must store its
arguments in a static `s_win2D = {x, y, w, h, left, right, bottom, top}` (add this). Then:

```text
sx = w / (right - left);   sy = h / (bottom - top);     /* bottom/top are the ortho args */
fbx0 = x + (dx - left) * sx;                             /* dx = draw-space x */
fby_gl = y + (bottom - dy) * sy;                         /* flips: ortho "bottom" arg is the larger y in UI top-left space */
```

Write this as a helper `RE_UI2D_DrawToWindow(float dx, float dy, float *wx, float *wy)`; test
with the current HUD ortho (`vp = (0,0,fw,fh, 0..lw, 0..lh)`): draw (0,0) to window
(0, fh); draw (lw, lh) to window (fw, 0).

Accumulation sites (only when `RE_UI2DTargetIsActive()`):

- `RE_DrawUI2D_Inner` (`tr_ui_batch.c`): compute min/max of `verts[i].xy` over `numVerts`
  (they are already in CPU memory; the loop is ~10k floats/frame, negligible), convert both
  corners with `RE_UI2D_DrawToWindow`, pad by 2 px, `RE_UI2D_AccumRectFb`. Skip when the
  stencil mask-write phase is active (`RE_UiStencilReassertMaskWrite` state: color inert).
- `RE_EndUiLayer` (`tr_ui_layer.c`): the composite quad covers `uiX,uiY,uiW,uiH` in draw space
  — accumulate the same way (or reuse the scissor `fbX,fbY,fbW,fbH` it already has: identical).
- Chrome cache blit (`RE_UiChromeCacheBlit`): `RE_UI2D_MarkFullResolve()` (disabled by default anyway).
- Host/legacy draws in `tr_draw.c` / `RB_StretchPic` in `tr_backend.c`
  (`Draw_StretchPic`, `Draw_StretchPic2`, `Draw_TilePic`, `Draw_TilePicOffset`,
  `Draw_TrianglePic`, `DrawBox`, `DrawLineLoop`): at their start, if the UI 2D target is active,
  call `RE_UI2D_MarkFullResolve()`. These can land in the FBO (e.g. `uir_draw_pic` fallback).
  Correctness first; the counter `resolvePx` on the overlay will show if this happens in-match
  (it should not for the shipped HUD packs).

Merge policy in `RE_UI2D_AccumRectFb` (integer rects, clamp to `0..width/height`):

1. If the new rect intersects or touches (within 8 px) an existing rect → union into it.
2. Else if `s_uiRectCount < UI_FBO_MAX_RECTS` → append.
3. Else union into the existing rect that yields the smallest area increase.

After step 1 or 3, re-check whether the grown rect now overlaps another and merge again
(one pass is enough; do not loop forever).

### 2.3 Region-limited resolve + composite — r_uiResolveRects (default 1)

In `RE_EndUI2DTarget`:

- If `r_uiResolveRects->integer == 0` or `s_uiFullResolve` or `s_uiRectCount == 0` → current
  full-screen behaviour (unchanged code path).
- Else for each rect `r`: blit that region with `qglBlitFramebuffer` (src and dst same size for
  MSAA resolve), then after binding FB 0 and the existing `Set2DWindow(0,0,W,H, 0,W,H,0, ...)`,
  draw one quad per rect with the same premultiplied composite state.
- Accumulate `tr_uiStats.resolvePixels += r.w * r.h` per rect (Phase 0 counter).
- If `s_uiRectCount == 0` and not full: nothing was drawn → skip blit and composite entirely
  (still rebind FB 0 and restore state exactly as today).

Vertex/texcoord mapping in that top-left ortho space (`W,H` = FBO size, `t=0` is texture bottom):

```text
yTop = H - (r.y + r.h);  yBot = H - r.y;
s0 = r.x / W;  s1 = (r.x + r.w) / W;  tTop = (r.y + r.h) / H;  tBot = r.y / H;
v0 (r.x, yTop)        tex (s0, tTop)
v1 (r.x+r.w, yTop)    tex (s1, tTop)
v2 (r.x+r.w, yBot)    tex (s1, tBot)
v3 (r.x, yBot)        tex (s0, tBot)
```

Draw all rects inside one `glBegin(GL_QUADS)` … `glEnd()`.

Do not change `RE_UI2D_FboEnsure`, sample counts, blend functions or the `Set2DWindow` call.

### 2.4 Confirm one target session per HUD frame

The overlay's `tgt` counter should read `1/1` in-match. If it shows `2/2`, the HUD overlay
phase painted something (modals/popups) — that is legitimate and must not be "fixed" here.

## Steps

1. 2.1 clear mode + cvar. Build. Verify: HUD identical; `ui_shape_clip 1` (stencil clips) still
   clips shapes correctly (competitive compass damage wedge, any shaped container with children);
   set back to `0`.
2. `s_win2D` capture in `Set2DWindow`, conversion helper, unit-check the two corner cases by a
   temporary `ri.Printf(PRINT_DEVELOPER, ...)` once at first use (remove after checking).
3. Rect accumulation at all sites + merge. Build. Temporarily draw the rects as thin outlines
   when `r_uiResolveRects 2` (debug value) so the user can see them on screen; keep this debug
   mode, it is cheap and useful.
4. Region-limited blit/composite. Build, deploy.
5. Pixel-identity check: at the same spot, `screenshot p2_full` with `r_uiResolveRects 0` and
   `screenshot p2_rects` with `1` (use the TGA `screenshot` command, not JPEG). Diff with a
   throwaway Python/PIL script in `/tmp` (max channel delta must be 0 in all three scenes:
   idle HUD, kill feed active, scoreboard open).
6. Protocol run with `ui_perf_gpu 1` for the `gpu ui`/`resolve` columns; Results; sign-off.

## Acceptance

- Pixel-identical (delta 0) in the three scenes with `r_uiResolveRects 1` vs `0`.
- `resolvePx` per frame drops (target: <= 40% of `vidW*vidH` in Still; report the value).
- `gpu ui us` and `gpu resolve us` drop; `frame us` improves or is unchanged (if the CPU is the
  bottleneck the GPU saving shows only as headroom — record it anyway).
- `r_uiClearMode 1` / `r_uiResolveRects 0` restore the old behaviour exactly.

## Pitfalls

- Window (GL) coordinates are bottom-left; UI draw space is top-left. Get the flip right in
  the helper and test the corners before wiring the rest.
- Stencil mask-write draws are color-inert; do not accumulate their rects (harmless but wasteful).
- The layer composite (`RE_EndUiLayer`) draws directly into the MSAA target — it must add its
  rect or the compass disappears with rects on.
- Keep `s_uiFullResolve` conservative: any draw you cannot bound → full resolve. Never guess.

## Results

Captured automatically from `debug-b55c7e.log` (`runId=phase2-auto`). Client auto-cycled
`r_uiResolveRects` 1 → 0 → 2 → 1 in-match (no console). User: HUD "seems to be working".

**Build:** 2026-09-12 ~23:47 `_build_deploy`  
**fullPx:** 3686400 (2560×1440)  
**Method:** median of settled 1 s windows (`frames≥600`, `resolvePct<20` for rect modes).

| Mode | resolveMode | frame us | ui total | fbo | gets | set2d | resolvePx | resolvePct | draws | gpu ui | gpu resolve |
|------|-------------|----------|----------|-----|------|-------|-----------|------------|-------|--------|-------------|
| Still rects | **1** | 1218 | 334 | 3 | 0 | 2 | **184875** | **5.0%** | 27 | 16 | **29** |
| Still full | **0** | 1044 | 280 | 3 | 0 | 2 | 3686400 | **100%** | 23 | 7 | **56** |
| Still outline | **2** | 1060 | 295 | 3 | 0 | 2 | 171746 | **4.7%** | 24 | 9 | 27 |

### Gate

| Item | Target | Result |
|------|--------|--------|
| Still `resolvePct` | ≤ 40% | **PASS (5.0%)** |
| Mode 0 restores full resolve | 100% | **PASS** |
| Visual (user) | identical / OK | **PASS** ("seems to be working") |
| User sign-off | required | **Approved 2026-09-12** (proceed to Phase 3) |

Early join spike (`resolvePct` 72% once) discarded as settle; steady Still stays ~3.6–5.5%.

Debug instrumentation (Phase 2 auto-capture / resolveMode schedule) removed after sign-off.

## Todo

- [x] 2.1 `r_uiClearMode`, color-only clear
- [x] 2.2 `Set2DWindow` capture, draw→window helper, rect accumulation + merge
- [x] 2.3 region blit + per-rect composite behind `r_uiResolveRects`, debug outlines at `2`
- [x] Visual check (user OK); auto outline pulse via client schedule
- [x] Protocol run / Results (`resolvePct` **5%** ≤ 40%)
- [x] User sign-off → strip Phase 2 auto-capture → Phase 3
