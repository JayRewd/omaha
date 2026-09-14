# Phase 3 — Fewer draw calls and cheaper submission (uirender + renderer batch)

Read `00-README.md` first. Precondition: Phase 2 signed off (or explicitly skipped by the user
after the Phase 1 decision rule).

## Goal

Cut the number of `glDrawElements` / `glScissor` calls per HUD frame and make each remaining
draw cheaper for the driver, with no visible change (3.1, 3.2). 3.3 is an opt-in that changes
clip-edge rasterization and needs visual sign-off.

Today a batch flush (one draw call) happens on: shader change, clip rect change, buffer
overflow, mask/stencil boundary, `LIVE_SUBTREE` boundary, phase end. Replay adds a forced
flush + `glScissor` for **every** recorded CLIP command because `UID_PaintListTryReplay`
(`code/uidesign/uid_paint.cpp:356-359`) calls `UIR_ForceClipRect`, which bypasses the dedup in
`uir_apply_clip_scissor` (`code/uirender/uir_compositor.c:215-247`). Batches are submitted as
client-side arrays (`tr_ui_batch.c:187-193`), which NVIDIA copies synchronously per call.

## 3.1 Replay clip dedup — `ui_replay_clip_dedup` (default 1)

- `uir_compositor.c`: add `void UIR_ApplyClipRect(float x, float y, float w, float h)` that
  builds the rect and calls `uir_apply_clip_scissor(&clip)` **without** resetting
  `g_appliedClipValid` (so an identical rect is a no-op). Keep `UIR_ForceClipRect` for callers
  that need it.
- `uid_paint.cpp` replay, `UID_PAINT_CMD_CLIP` branch: when the cvar is on, drop the explicit
  `UIR_BatchFlush()` and call `UIR_ApplyClipRect(...)` (the scissor path flushes by itself only
  when it really changes the scissor). When off, keep the old two lines.
- `UID_PaintListEndRecord` (`uid_paint.cpp`): post-process `list->cmds`: for runs of consecutive
  `CLIP` commands keep only the last one; drop a `CLIP` whose rect equals the previous
  effective clip. Also drop a trailing `CLIP` at the very end of the list.
- Expected effect on the overlay: `scissor` per frame in Still drops to the number of *distinct*
  clip changes (~10-18 → ~5-10), `draws` drops by the same amount.

## 3.2 VBO streaming for UI batches — `r_uiVbo` (default 1)

- `code/renderercommon/qgl.h` already declares `QGL_1_5_PROCS` (`BindBuffer`, `GenBuffers`,
  `BufferData`, `BufferSubData`, `DeleteBuffers`) but `code/sdl/sdl_glimp.c` does not declare /
  load / clear them for GL1. Add them exactly like `QGL_ARB_framebuffer_object_PROCS` (declare
  ~line 80, load in the version/extension block ~line 357 when GL >= 1.5 or
  `GL_ARB_vertex_buffer_object` is present, clear ~line 428).
- `tr_ui_batch.c`: lazily create two buffers (`s_uiVbo`, `s_uiIbo`) when
  `r_uiVbo->integer && qglGenBuffers && qglBufferData`. In `RE_DrawUI2D_Inner`:
  ```c
  qglBindBuffer(GL_ARRAY_BUFFER, s_uiVbo);
  qglBufferData(GL_ARRAY_BUFFER, numVerts * sizeof(ui2dVert_t), verts, GL_STREAM_DRAW); /* orphan + upload */
  qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_uiIbo);
  qglBufferData(GL_ELEMENT_ARRAY_BUFFER, numIndexes * sizeof(unsigned short), indexes, GL_STREAM_DRAW);
  qglVertexPointer(2, GL_FLOAT, sizeof(ui2dVert_t), (const void *)offsetof(ui2dVert_t, xy));
  qglColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ui2dVert_t), (const void *)offsetof(ui2dVert_t, rgba));
  qglTexCoordPointer(2, GL_FLOAT, sizeof(ui2dVert_t), (const void *)offsetof(ui2dVert_t, st));
  qglDrawRangeElements(GL_TRIANGLES, 0, numVerts - 1, numIndexes, GL_UNSIGNED_SHORT, (const void *)0);
  ```
  Check the real field names of `ui2dVert_t` in `tr_types.h:283-288` before writing offsets.
- **Unbind both buffers** (`qglBindBuffer(target, 0)`) in `RE_UI2DBatchEnd` and at the end of
  the non-session path of `RE_DrawUI2D_Inner`; the legacy tess path and `RE_DrawUiStencilMaskTris`
  use client arrays and would read garbage offsets otherwise.
- Delete buffers in `RE_UI2D_FboShutdown`'s neighbour `RE_Shutdown` path (`tr_init.c` ~line 1748)
  and invalidate on `vid_restart`.
- With `r_uiVbo 0` or missing procs the existing client-array path runs unchanged.
- If `glBufferData` orphaning per draw shows up as slow (unlikely on NVIDIA), switch to a
  3-slot ring with `glBufferSubData` appends; not needed unless measured.

## 3.3 (opt-in) Geometric clipping instead of scissor — `ui_batch_softclip` (default 0)

Only do this if, after 3.1, the `scissor` counter is still > 8 per frame in Busy and the user
agrees to evaluate the visual difference: clip edges become MSAA-antialiased fractional edges
instead of hard integer scissor edges (visible only where content is actually cut by an
`overflow="hidden"` box: compass tape ends, scrolling lists).

Design sketch (implement only after the go-ahead):
- `uir_compositor.c`: expose `int UIR_CurrentClipRect(uir_rect_t *out)` (top of `g_clipStack`)
  and, when soft clip is on, make `uir_apply_clip_scissor` only update `g_appliedClip`
  (no GL scissor) after issuing the full-viewport scissor once per target session.
- `uir_batch.c` `uir_batch_append`: if soft clip is on and the current clip is not the full
  viewport, clip every incoming triangle against the rect (Sutherland–Hodgman, interpolate
  `s,t,r,g,b,a`), fan-triangulate, then append. Fast path for axis-aligned quads
  (4 verts / 6 idx with the corner pattern from `uir_batch_append_quad_tris`): clamp the rect
  and lerp `s,t`.
- Non-batch fallbacks (`UIR_Draw2D_*` paths that call `g_d2d.drawPic` etc.), image-mask begin,
  stencil begin: flush, apply the hardware scissor for the logical clip, draw, then restore the
  full-viewport scissor before the next batched draw (lazy flag).
- Verification: side-by-side screenshots of compass tape ends and a scrolled scoreboard list at
  `ui_batch_softclip 0/1`; user decides. If rejected, leave the cvar at 0 (or remove in Phase 5).

## 3.4 (future, not in this program) Single UI texture atlas

Fonts (one 1024x1024 atlas per face+size, `uir_font.cpp`), HUD images (48 shaders in
`hud_modern_images.xml`), gradients and the white fill texture each force a shader switch =
flush. Packing them into one atlas would make most of the HUD a handful of draws. Recorded here
so nobody "quick-fixes" it inside Phases 3-4; it needs its own plan (image registry changes,
atlas paging, mip/filtering rules for icons).

## Steps

1. 3.1 `UIR_ApplyClipRect`, replay branch, EndRecord CLIP collapsing, cvar. Build. Verify
   `scissor`/`draws` drop in Still; scroll the scoreboard (TAB, mouse wheel where applicable),
   toggle HUD packs (classic/modern/competitive) — clipping must be identical.
2. 3.2 proc loading, VBO path, unbinds, cvar. Build. Verify legacy HUD (`ui_legacy 1`), console,
   main menu, pause menu previews, stencil clips (`ui_shape_clip 1`) all render — these share the
   client-array paths that must see buffers unbound.
3. Deploy; protocol run; Results; sign-off. Then decide on 3.3 with the user.

## Acceptance

- `draws` per frame in Still <= 40 (report), `scissor` <= 10, no visual change.
- `r_uiVbo 0` and `ui_replay_clip_dedup 0` restore old behaviour.
- CPU `paint us` decreases (replay path issues fewer GL calls).

## Pitfalls

- Never leave a VBO/IBO bound when returning to any non-UI path.
- `offsetof` requires `<stddef.h>` in C.
- The CLIP collapsing in `EndRecord` must not remove a CLIP that precedes a `LIVE_SUBTREE`,
  `IMAGE_MASK_*` or `SHAPE_CLIP_*` command — only collapse runs of consecutive CLIPs.

## Results

Play-Still from `debug-b55c7e.log` (`runId=phase3-img-clip`). Not spectate. TAB closed.

**Build:** 2026-09-13 ~00:53 `_build_deploy`  
**Method:** settled play-Still windows (`fbo=3`, `set2d=2`, `sciSten=0`). 3.3 not implemented.

| Scenario | HUD | cvar | frame us | ui total | paint | draws | scissor | fbo | gets | set2d | replay% | gpu ui |
|----------|-----|------|----------|----------|-------|-------|---------|-----|------|-------|---------|--------|
| Still (play) | modern | new | ~8000* | ~780 | ~550 | **33–39** | **2** | 3 | 0 | 2 | **0** | ~130 |
| Busy (TAB) | modern | new | — | — | — | — | 45–52 | 3 | 0 | 2 | 0 | — |

\*Frame us in this capture was vsync-bound (~125 Hz); gate metrics are `draws` / `scissor`.

Leftover Still scissors: compass overflow `746,1333,1068×43` + fullscreen restore. Health stencil begin-scissor skipped (H6). Unclipped image dest scissors removed (H7).

### vs Phase 1 Still

| Metric | Phase 1 | Phase 3 | Δ |
|--------|---------|---------|---|
| draws | ~29 | **~35** | +6 (more live HUD images; still ≤ 40) |
| scissor | ~14 | **2** | −12 |

### Gate

| Item | Target | Result |
|------|--------|--------|
| Still `draws` | ≤ 40 | **PASS (~35)** |
| Still `scissor` | ≤ 10 | **PASS (2)** |
| Visual | no change | **PASS** (user: proceed) |
| User sign-off | required | **Approved 2026-09-13** (proceed to Phase 4) |

`replayHitPct` stayed **0%** — whole-document list never records while the HUD is dirty every frame (`foreach_text` / `fill` / `visible_expr`). That is Phase 4, not a Phase 3 miss. 3.3 skipped.

Debug ingest / unique-scissor boxes stripped after sign-off. Site counters and H6/H7 behavior stay.

## Todo

- [x] 3.1 `UIR_ApplyClipRect`, replay dedup, EndRecord collapsing, `ui_replay_clip_dedup`
- [x] 3.2 GL 1.5 buffer procs in `sdl_glimp.c`, VBO/IBO streaming, unbinds, `r_uiVbo`
- [x] H6 stencil begin-scissor skip; H7 dest-clip skip for unclipped images/gradients
- [x] Build, deploy, protocol / Results
- [x] Visual sign-off (user 2026-09-13); `scissor≤10` met in play-Still
- [x] 3.3 skipped (gate already met; no user OK for softclip)
