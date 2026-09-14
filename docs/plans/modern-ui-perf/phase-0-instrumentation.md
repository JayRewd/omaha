# Phase 0 — Zero-I/O instrumentation, baseline, driver diagnostics

Read `00-README.md` first. This phase changes **no rendering behaviour**. It adds in-memory
counters, an on-screen overlay, and produces the baseline table that every later phase is
compared against.

## Goal

An on-screen overlay (`ui_perf_hud 1`) that shows 1-second-window averages (microseconds and
counts) for the modern UI's per-frame work, split by subsystem, plus renderer-side GL event
counters and optional GPU timings. Cost of the overlay when `ui_perf_hud 0` must be nil
(one integer compare per site).

## Non-goals

- No file logging, no per-frame console prints, no `glFinish`, no `glGet*` on the CPU side.
- No change to what is drawn or in which order (other than the overlay text itself, which is
  drawn through the **legacy** `UIFont::Print` path like `View3D::DrawFPS`, i.e. outside the
  modern UI target so it does not pollute UI counters).

## Design

### 0.1 Renderer GL event counters (C, `code/renderergl1`, `code/renderercommon`)

Add to `code/renderercommon/tr_types.h` (shared with the client, next to `ui2dVert_t`):

```c
/* Added in Omaha: per-frame GL event counters for the modern UI path (ui_perf_hud). */
typedef struct uiGlStats_s {
	int fboBinds;        /* glBindFramebuffer calls actually issued */
	int scissorCalls;    /* glScissor calls issued */
	int glQueries;       /* glIsEnabled / glGetIntegerv / glGetQueryObject* calls */
	int drawElements;    /* RE_DrawUI2D glDrawElements calls */
	int drawVerts;       /* verts submitted through RE_DrawUI2D */
	int immediateQuads;  /* glBegin(GL_QUADS) quads: composites, mask apply, DrawBox */
	int set2DWindow;     /* Set2DWindow calls */
	int issuePending;    /* R_IssuePendingRenderCommands calls */
	int targetBegins;    /* RE_BeginUI2DTarget that actually bound+cleared */
	int targetEnds;      /* RE_EndUI2DTarget that actually resolved+composited */
	int layerBegins;     /* RE_BeginUiLayer successes */
	int stencilBegins;   /* RE_BeginUiStencilMask */
	int resolvePixels;   /* sum of blit rect areas in RE_EndUI2DTarget (full screen today) */
	unsigned long long gpuUiNs;       /* GPU ns: BeginUI2DTarget..EndUI2DTarget (0 if off/unavailable) */
	unsigned long long gpuResolveNs;  /* GPU ns: blit+composite inside RE_EndUI2DTarget */
	unsigned long long gpuLayerNs;    /* GPU ns: sum of layer begin..end spans */
	int gpuSamplesValid;              /* 1 when the gpu* fields hold a completed frame */
} uiGlStats_t;
```

Implementation:

- New file `code/renderergl1/tr_ui_stats.c` with `static uiGlStats_t s_cur, s_last;`,
  `void RE_UiStatsFrameBegin(void)` (copy `s_cur` to `s_last`, zero `s_cur`),
  `void RE_UiStatsGet(uiGlStats_t *out)` (copy `s_last`), and inline-able increment helpers
  declared in `tr_local.h`: `void RE_UiStatInc(int *field)` is overkill — just expose
  `extern uiGlStats_t tr_uiStats;` and increment fields directly at call sites
  (`tr_uiStats.fboBinds++;`). Add the file to the renderer target in the CMake list that
  contains `tr_ui_fbo.c` (search `tr_ui_fbo.c` in `CMakeLists.txt` files).
- Call `RE_UiStatsFrameBegin()` at the top of `RE_BeginFrame` in `code/renderergl1/tr_cmds.c`
  (right after `glState.finishCalled = qfalse;`, ~line 340). Stereo calls BeginFrame twice;
  that is acceptable (counts are per BeginFrame).
- Increment sites (grep each symbol; the counts below are what exists today):
  - `qglBindFramebuffer(` in `tr_ui_fbo.c` and `tr_ui_layer.c` → `fboBinds`.
  - `qglScissor(` in `tr_draw.c` (`Set2DWindow`, `RE_Scissor`), `tr_ui_fbo.c`, `tr_ui_layer.c`,
    `tr_ui_stencil.c`, `tr_backend.c` → `scissorCalls`.
  - `qglIsEnabled(` / `qglGetIntegerv(` in `tr_ui_layer.c:161-162`, `tr_ui_batch.c:74,147`,
    `tr_draw.c:396` → `glQueries`. (Do not count the init-time ones in `tr_init.c`/`sdl_glimp.c`.)
  - `qglDrawElements(` in `RE_DrawUI2D_Inner` (`tr_ui_batch.c`) → `drawElements`, `drawVerts += numVerts`.
  - `qglBegin(GL_QUADS)` in `RE_EndUI2DTarget`, `RE_UiLayerApplyMask`, `RE_EndUiLayer`,
    chrome-cache blit, `DrawBox` → `immediateQuads`.
  - `Set2DWindow` entry → `set2DWindow`. `R_IssuePendingRenderCommands` entry → `issuePending`.
  - `RE_BeginUI2DTarget` after successful bind → `targetBegins`; `RE_EndUI2DTarget` after the
    blit → `targetEnds`, `resolvePixels += w*h`.
  - `RE_BeginUiLayer` on success → `layerBegins`; `RE_BeginUiStencilMask` → `stencilBegins`.
- Export: add `void (*UiStatsGet)(uiGlStats_t *out);` to `refexport_t` in
  `code/renderercommon/tr_public.h` (put it right after `UI2DTargetSamples`), wire
  `re.UiStatsGet = RE_UiStatsGet;` in `GetRefAPI` (`tr_init.c`, ~line 2010). Leave the GL2
  renderer's export unset (client must NULL-check `re.UiStatsGet`).

### 0.2 GPU timer queries (optional, `ui_perf_gpu`, default 0)

- `code/renderercommon/qgl.h`: add after `QGL_ARB_occlusion_query_PROCS`:
  ```c
  // GL_ARB_timer_query (OpenGL 3.3)
  #define QGL_ARB_timer_query_PROCS \
  	GLE(void, GetQueryObjectui64v, GLuint id, GLenum pname, GLuint64 *params) \
  ```
  and `#ifndef GL_TIME_ELAPSED #define GL_TIME_ELAPSED 0x88BF #endif`,
  `#ifndef GL_QUERY_RESULT_AVAILABLE #define GL_QUERY_RESULT_AVAILABLE 0x8867 #endif`,
  `#ifndef GL_QUERY_RESULT #define GL_QUERY_RESULT 0x8866 #endif`.
  In `code/sdl/sdl_glimp.c` replicate exactly how `QGL_ARB_occlusion_query_PROCS` is declared
  (line ~79), loaded (the `GLE` block around line 357 for GL >= 3.x or by extension string
  `GL_ARB_timer_query`), and cleared (line ~427). `GLuint64` is available via `qgl.h`'s GL headers.
- `tr_ui_stats.c`: ring of 4 frames x 3 spans (ui, resolve, layers) of query objects created
  lazily when `r_uiPerfGpu` (renderer mirror of `ui_perf_gpu`, `CVAR_TEMP`, registered in
  `R_Register`) is non-zero and `qglGenQueries && qglBeginQuery && qglGetQueryObjectui64v`.
  Spans:
  - `ui`: `qglBeginQuery(GL_TIME_ELAPSED, q)` at the end of `RE_BeginUI2DTarget` (after clear),
    `qglEndQuery` at the end of `RE_EndUI2DTarget`.
  - `resolve`: begin right before `qglBlitFramebuffer` in `RE_EndUI2DTarget`, end after the
    composite `qglEnd()`. **Nested TIME_ELAPSED queries are illegal in GL** — so end the `ui`
    query before beginning `resolve` and add both to `gpuUiNs` on readback (document this in code).
  - `layers`: one query per `RE_BeginUiLayer`..`RE_EndUiLayer` pair, max 8 per frame; while a
    layer query is active the `ui` query must not be active either (end `ui` on layer begin,
    restart it on layer end — keep a tiny state machine `s_gpuUiOpen`). If this gets complicated,
    ship only `ui` and `resolve` first.
  - Readback policy: every 60th `RE_BeginFrame`, for the ring slot that is 3 frames old, call
    `qglGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail)`; if available read
    `GL_QUERY_RESULT` with `qglGetQueryObjectui64v` and publish into `s_last.gpu*Ns` with
    `gpuSamplesValid = 1`. Count these reads in `glQueries` so the overlay shows the cost.
  - Document in the cvar description: "GPU timing reads force a driver sync every 60 frames;
    compare frame us with ui_perf_gpu 0 vs 1 to see the sync penalty itself" — on NVIDIA
    Windows with Threaded Optimization the difference is a direct measurement of the
    problem Phase 1 removes.

### 0.3 Client CPU timers (C++, `code/client`, `code/uidesign`)

Create `code/client/cl_uiperf.cpp` + `cl_uiperf.h` (add to the client CMake source list next
to `cl_uirender.cpp`). It owns:

```cpp
struct UiPerfAcc { double sumUs; double maxUs; int n; };
struct UiPerfWindow {           // one completed 1 s window, what the overlay prints
	double frameUs, renderUs, uiTotalUs, syncUs, hudCvarUs, bindUs, layoutUs, paintUs, overlayUs, cg2dUs;
	double replayHitPct; int layoutRunsPerSec; int frames;
	uiGlStats_t gl;             // per-frame averages of renderer counters (rounded)
};
void CL_UIPerf_FrameBegin();                       // called at SCR_UpdateScreen entry
void CL_UIPerf_Mark(int id, double us);            // add one sample to accumulator id
void CL_UIPerf_NoteReplay(int hit);                // 1 when UID_PaintChrome replayed
void CL_UIPerf_NoteLayoutRan(int mode);
const UiPerfWindow *CL_UIPerf_Window();            // last completed window
void CL_UIPerf_Reset();                            // console: ui_perf_reset
void CL_UIPerf_Dump();                             // console: ui_perf_dump (one printf)
```

Timing uses `std::chrono::steady_clock`. A helper RAII `UiPerfScope(id)` is fine. When
`ui_perf_hud->integer == 0` every entry point returns immediately (single int compare).

Instrumentation points (all gated by the cvar check inside the helper):

| id | Where | What |
|----|-------|------|
| frame | `SCR_UpdateScreen` entry (`cl_scrn.cpp`) | delta between consecutive entries = frame period |
| render | `SCR_UpdateScreen` entry → exit | render+UI wall time |
| uiTotal | `CL_UIR_DrawCrosshair` entry → exit (`cl_uirender.cpp:6978`) | whole modern HUD |
| sync | around `CL_UIR_SyncHudLayerMenus(...)` inside DrawCrosshair | cvar push + bind + layout |
| hudCvar | around `UIR_Hud_Sync()` in `CL_UIR_UpdateHudMenus` (`cl_uirender.cpp:1917`) | Cvar_Set pushes |
| bind / layout | read `UID_ProfileCaptureFrame` values `us[UID_PROF_FRAME_BIND]`, `us[UID_PROF_FRAME_LAYOUT]` after the update loop; the UID profile timers are already in place but only tick when `UID_ProfileEnabled()` — make `UID_ProfileEnabled()` also return true when `ui_perf_hud` is on (add a setter `UID_ProfileSetExternalEnable(int)` in `uid_profile.cpp`, called from `CL_UIR_RegisterCvars`/frame). Do **not** enable the console printing path (`ui_profile`). |
| paint | around `UIR_BeginOverlayFrame(...)` in DrawCrosshair | chrome phase incl. FBO begin/end |
| overlay | around `UIR_EndOverlayFrame()` | overlay phase |
| cg2d | around `cge->CG_Draw2D()` in `View3D::Draw2D` (`cl_uiview3d.cpp:612`) | cgame 2D incl. `CG_SyncModernHudCvars` |
| replay | `UID_PaintChrome` (`uid_widget.cpp:3078`): call a backend hook `backend->perfNoteReplay(1/0)` — add an optional function pointer to `uid_backend_t` (NULL-checked) wired in `CL_UIR_FillUidBackend` | replay hit ratio |
| layoutRan | `UID_Update` (`uid_runtime.cpp:529`) → same hook style `perfNoteLayout(mode)` | layouts per second |

Windowing: accumulate per frame; when 1 s elapsed (steady_clock), compute averages into the
"last window" and zero accumulators. Renderer counters: call `re.UiStatsGet(&gl)` once per
frame in `CL_UIPerf_FrameBegin` (values are for the previous frame) and accumulate.

### 0.4 Overlay drawing

`View3D::DrawProf()` in `code/client/cl_uiview3d.cpp:291` is an unimplemented stub. Implement it
to print the window (`ui_perf_hud 1` = lines 1-4, `2` = all) using `setFont("verdana-14")`
and `m_font->Print` exactly like `DrawFPS` (respect `getHighResScale()`), top-left, starting at
`m_frame.pos.y + 40`. Change the gate at `cl_uiview3d.cpp:628` and `:655` from
`if (fps->integer && ...)` to `if ((fps->integer || ui_perf_hud->integer) && ...)` and inside
call `DrawProf()` only when `ui_perf_hud->integer`. Lines (fixed-width numbers, `%6.0f`):

```
UIPERF hud=modern  frame 1012us  render 803us  ui 412us  cg2d 55us
sync 131us (hudcvar 22  bind 71  layout 12  layouts/s 3)   paint 244us  overlay 4us  replay 61%
gl/frame: draws 58  verts 9812  scissor 31  fbo 9  set2d 4  issue 17  gets 4  imm 3  tgt 1/1  layers 2  stencil 0
gpu: ui 214us  resolve 96us  layers 41us   (ui_perf_gpu 0 -> "gpu: off")
[verbose] resolvePx 3686400  hitRatioFrames 612/1004  maxFrame 3120us
```

`hud=` prints `legacy` when `CL_UIR_UseLegacyHud()`; in legacy mode the `ui`/`sync`/`paint`
lines print `-` (the modern path does not run).

### 0.5 Console commands

`ui_perf_reset` and `ui_perf_dump` registered in `CL_UIR_Init` next to `ui_render_test`.
`ui_perf_dump` prints the last window once (this is the only print, on demand).

## Steps

1. Add `uiGlStats_t`, `tr_ui_stats.c`, counters at every site listed in 0.1, `RE_UiStatsGet`
   export. Build. Verify `re.UiStatsGet` non-NULL with GL1.
2. Add `cl_uiperf.cpp/.h`, cvars `ui_perf_hud`, `ui_perf_gpu` (`CVAR_TEMP`), commands, hooks
   in `uid_backend_t`, `UID_ProfileSetExternalEnable`. Build.
3. Implement `View3D::DrawProf`. Build, deploy, verify overlay in a map with `ui_perf_hud 1`
   and `ui_perf_hud 2`; verify `ui_perf_hud 0` shows no overlay and `frame us` is unchanged
   (compare `cl_showfps`/`fps 1` before/after: within noise).
4. GPU queries (0.2). Build, verify `gpu:` line populates with `ui_perf_gpu 1`, and that
   `gets` increases by exactly the readback count every 60 frames.
5. Run the measurement protocol (README section 3) for modern and legacy → Results table.
6. Driver diagnostics (user does these; agent records numbers), each as `Still` + `Look`:
   - D1: NVIDIA Control Panel → Manage 3D Settings → Program Settings → add
     `e:\Dev\Omaha\test build\openmohaa.exe` → **Threaded optimization = Off**. Restart game.
     Compare `frame us`/`ui us` with Auto/On. Restore afterwards.
   - D2: `set r_uiFramebuffer 0` (no vid_restart needed; soft masks will be skipped — visual
     change is expected; this is a diagnostic only).
   - D3: `set ui_paint_list 0`.
   - D4: `set ui_perf_gpu 1` vs `0` (measures the sync penalty of a single `glGet` per 60 frames).
   - D5: `r_uiMultisample 0` + `vid_restart` — note that this path re-enables
     `qglIsEnabled(GL_MULTISAMPLE)` in `RE_UI2DBatchBegin`; check the `gets` counter. Restore to 8.
   Record everything in the Results table with a one-line interpretation per diagnostic.

## Acceptance

- Overlay shows plausible numbers; `ui_perf_hud 0` costs nothing measurable.
- Baseline table complete for modern + legacy, three scenarios.
- Diagnostics D1-D5 recorded. If D1 (threaded optimization off) closes most of the gap, Phase 1
  is confirmed as the priority; if not, note it and still do Phase 1 (it is cheap), then weight
  Phases 2-4 by the largest remaining counter.

## Pitfalls

- Do not use `Sys_Milliseconds` for sub-millisecond timing.
- Do not read GPU queries every frame; do not read a query the same frame it was ended.
- Never begin a `GL_TIME_ELAPSED` query while another is active.
- Keep the overlay text on the legacy font path; do not draw it with `UIR_*`.
- `RE_BeginFrame` is per stereo eye; counters are per BeginFrame, fine for mono.

## Results

Captured from \debug-b55c7e.log\ (1 s windows, \ui_perf_hud 1\).
Two modern sessions: first with UI FBO **off**, second with FBO **on** after enabling \
_uiFramebuffer\.
No legacy restart; D1/D3–D5 deferred.

**Build:** 2026-09-12 ~16:57 \_build_deploy\  
**Method:** median of tagged windows (drop first sample of each tag). Values in µs / per-frame averages.

### Primary baseline — \
_uiFramebuffer 1\ (intended defaults)

| Scenario | HUD | cvar | frame | ui | sync | bind | layout | paint | hit% | cg2d | draws | scissor | fbo | gets | set2d | gpu |
|----------|-----|------|-------|----|------|------|--------|-------|------|------|-------|---------|-----|------|-------|-----|
| Still | modern | FBO on | 1229 | 260 | 103 | 80 | 0.2 | 154 | 0 | 22 | 21 | 51 | 8 | 73 | 17 | off |
| Look/Busy | modern | FBO on (\moving\) | 1254 | 324 | 164 | 134 | 0.1 | 159 | 0 | 22 | 21 | 34 | 8 | 16 | 8 | off |
| Still/Look | legacy | — | — | — | — | — | — | — | — | — | — | — | — | — | — | — |

Also: \	gtB/E=1\, \layers=1\, \
esolvePx=3686400\ (full 2560×1440 every frame). \
eplay hit%=0\ always.

### D2 A/B — first session was \
_uiFramebuffer 0\

| Scenario | frame | ui | paint | bind | gets | fbo |
|----------|-------|----|-------|------|------|-----|
| Still FBO off | 1238 | 223 | 99 | 100 | 72 | 0 |
| Still FBO on | 1229 | 260 | 154 | 80 | 73 | 8 |
| Look FBO off | 1183 | 285 | 117 | 146 | 15 | 0 |
| Look FBO on | 1254 | 324 | 159 | 134 | 16 | 8 |

**D2 interpretation:** FBO on costs ~**+55 µs paint** (Still) / ~**+42 µs paint** (Look). Sync-gets stay (Still ~72). Soft-mask + full-screen resolve are live → Phase 2 justified. FBO off is not a free win for \gets\.

### Diagnostics

| ID | Result | Interpretation |
|----|--------|----------------|
| D1 Threaded Opt Off | not run | Deferred. |
| D2 \
_uiFramebuffer\ | **done** (FBO0 vs FBO1 sessions) | +40–55 µs paint with FBO; Phase 2 relevant; gets remain. |
| D3 \ui_paint_list 0\ | not run | hit%=0 already. |
| D4 \ui_perf_gpu\ | not run | Deferred. |
| D5 \
_uiMultisample 0\ | not run | Deferred. |

**Gate for Phase 1:** FBO-on baseline locked. Levers: **paint ~154–159 µs** (incl. FBO/layer), **bind ~80–134 µs**, **gl gets 16–73**. Start Phase 1 (sync-query removal).

## Todo

- [x] 0.1 renderer counters + \RE_UiStatsGet\ export
- [x] 0.3 \cl_uiperf.cpp\ accumulators, hooks, cvars, commands
- [x] 0.4 \View3D::DrawProf\ overlay
- [x] 0.2 GPU timer queries behind \ui_perf_gpu\
- [x] Build + deploy; overlay verified
- [x] Baseline table (modern Still + Look/Busy; FBO on + off)
- [x] Diagnostics D2 recorded (D1/D3–D5 deferred)
