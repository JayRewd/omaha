# Phase 4 — Deliberate caching for a live HUD (uidesign / client / cgame)

Read `00-README.md` first. Precondition: Phases 1-3 signed off (or skipped by the decision
rule) and the Phase 3 Results show which counters are still above target. Execute the items
below in order; each is independent and behind its own cvar, so stop after any item if the
targets are met.

## Why the current retained paint list fails in a real match

`UID_MarkDirty` (`code/uidesign/uid_document.cpp:289-311`) invalidates the **whole document's**
paint list on any PAINT/LAYOUT/STRUCTURE dirt. In a match something is always dirty:

| Event | Frequency | Effect today |
|-------|-----------|--------------|
| Kill-feed / chat / game-message fade step (32 steps over 1.5-2 s) | ~20/s per fading row | `foreach_fade` PAINT → whole HUD re-painted + re-recorded |
| Timer text (`{floor(cvar.ui_om_hud_time_seconds / 60)}` ...) | 1/s | whole HUD |
| Health / ammo / score digits | on change | whole HUD |
| Compass heading | every frame while turning | handled by `LIVE_SUBTREE` (list stays valid) but the tape subtree (~60-100 nodes incl. text) is re-painted live every frame |
| New kill / chat line | per event | `STRUCTURE` → row clone, layout, whole HUD re-record |

So the "replay hit %" in Busy is low and every miss costs a full `PaintChromeNode` walk with
text shaping. The fixes below make the cache granular and make the two continuous
animations (translate, fade) pure vertex transforms of cached geometry.

Data model recap (read these before coding): `uid_document_t` (`uid_document.h` ~line 470+:
`nodes`, `states`, `dirty`, `dirtyLayoutNodes`, `pendingTranslateDeltas`, `paintList`),
`uid_paint_list_t` / commands (`uid_paint.cpp:42-79`), `PaintChromeNode` (`uid_widget.cpp:1779`),
`UID_PaintChrome` (`uid_widget.cpp:3071`), `UID_Update` (`uid_runtime.cpp:484`),
`UID_ApplyPendingTranslateDeltas` (`uid_layout.cpp:3003`),
`ApplyForeachLifetimeOpacity` + callers (`uid_collection.cpp:1293, 1632-1638, 1734`),
`UID_SyncBindings` (`uid_binding.cpp:2609`).

## 4.1 Per-region retained paint chunks — `ui_paint_regions` (default 1)

### Data

Add to `uid_document_t`:
```cpp
std::vector<uid_node_id_t> parentOf;   /* node -> parent, UID_INVALID_NODE_ID for roots; rebuilt when STRUCTURE clears */
std::vector<int>           regionOf;   /* node -> region index, -1 if not under the chrome root */
bool                       regionsStale; /* set on STRUCTURE / load / foreach rebuild */
void                      *paintRegions; /* opaque: std::vector<uid_paint_region_t> owned by uid_paint.cpp */
```
In `uid_paint.cpp`:
```cpp
struct uid_paint_region_t { uid_node_id_t root; uid_paint_list_t list; bool valid; };
```
Region roots = the direct children of the chrome root (`menu_root` if present, else
`doc->rootNode`, same lookup as `UID_PaintChrome`). Region index 0 is reserved for the chrome
root's own background/content ("root self"). Helper `UID_RebuildParentMap(doc)` (move the loop
from `UID_ApplyPendingTranslateDeltas:3010-3017` into it and make that function use
`doc->parentOf`) and `UID_RebuildPaintRegions(doc)` (fills `regionOf` by walking up `parentOf`
until the parent is the chrome root; clears and resizes the region vector, all `valid=false`).
Rebuild both in `UID_Update` right where `UID_DIRTY_STRUCTURE` is cleared
(`uid_runtime.cpp:530`) and on first use when `regionsStale`.

Regions are disabled (fall back to the single list) when the chrome root has a `mask-image`
or a non-rectangle `shape` with children (its children could not be painted independently).
Check once in `UID_RebuildPaintRegions`.

### Invalidation rules (`UID_MarkDirty`)

- `PAINT` only: `regionOf[nodeId] >= 0` → that region `valid=false`; `nodeId < 0` or
  `regionOf == -1` under the chrome root → all regions invalid. Region 0 is invalidated only
  when `nodeId == chromeRoot`.
- `LAYOUT`: same as PAINT (the node's region) **plus** in `UID_Update` after layout: if the
  layout was full (`layoutMode == 1`) → all regions invalid. Scoped layout stays within a
  boundary inside one region, so the region-level invalidation already done is sufficient.
- `STRUCTURE`: all regions invalid + `regionsStale = true` (node ids are remapped by
  `RemoveExpandedForeach`; `LIVE_SUBTREE`/cached node ids would dangle).
- Keep `doc->dirty |= flags` and the old single-list invalidation untouched when the cvar is off.

### Paint (`UID_PaintChrome`)

When the cvar is on and regions are usable:
1. Ensure regions/parent map are fresh. `UIR_BatchFlush()`.
2. Compute root visibility/opacity exactly like `PaintChromeNode` does (visible → else return;
   `rootOpacity = NodeOpacity(root) * st->lifetimeOpacityMul`); push the root's `effectiveClip`
   via `PushClip` (mirrors the top of `PaintChromeNode`).
3. Region 0 (root self): paint `UID_PaintNodeBackground` + `UID_PaintNodeContent` of the root
   into chunk 0 (record when invalid, replay when valid). Refactor `PaintChromeNode` so a
   `paintChildren=false` flag skips the children loop and the shape-child-clip block; call it
   with that flag here. No mask on root (guaranteed by the usability check).
4. For each child `c` in `root.children` (regions 1..N in order):
   - `UIR_ApplyClipRect(rootEffectiveClip)` (Phase 3.1 helper; dedup'd, normally a no-op) so
     every chunk starts from the same ambient clip.
   - If `region.valid` and the chunk's `uiPxScale/logicalW/logicalH` match the doc → replay the
     chunk with the existing replay loop generalized to take a `uid_paint_list_t *`
     (factor `UID_PaintListTryReplay`'s loop into `ReplayList(doc, list, backend)` returning
     0/1; on failure mark the region invalid and paint it live this frame).
   - Else `BeginRecord(&region.list)`, `PaintChromeNode(doc, c, backend, true, rootOpacity)`,
     `EndRecord(&region.list)`; `valid = list.valid` (false if `sawHostDraw` or empty; an empty
     chunk for a culled/hidden region should still count as valid — treat `cmds.empty()` as
     valid-empty, not invalid, otherwise hidden regions repaint every frame).
5. `PopClip`, then `doc->dirty &= ~UID_DIRTY_PAINT`.

`BeginRecord/EndRecord/ClearList/OnRecordDraw/OnRecordClip` already operate on a
`uid_paint_list_t *` via `g_recording`; keep that mechanism, just point it at the region's list.
Recording now happens during the dirty paint itself (not on the next clean frame) — that is
intentional; the memcpy is cheaper than a second full paint.

### Verification

- Overlay `replay hit %` in Busy must jump (target >= 90% before 4.2/4.3, >= 95% after).
- Add to the overlay verbose line: `regions dirty/total` per frame (hook in `uid_backend_t`).
- Scenarios: standing still (all regions replay), timer tick (only the top strip region
  re-records), kill (messaging region + STRUCTURE → all once), health change, HUD hide/show via
  `ui_om_hud_show`, pack switch classic/modern/competitive, `vid_restart`, scoreboard hold,
  pause menu open/close, sniper zoom toggle. Screenshots identical with `ui_paint_regions 0/1`.

## 4.2 Cached geometry for bound translate subtrees — `ui_live_translate_cache` (default 1)

Today (`uid_widget.cpp:1839-1858`, `uid_paint.cpp:232-242, 348-354`) a node with a bound
`translate-x/y` is recorded as `LIVE_SUBTREE` and **re-painted live on every replay**. For the
compass tape that is a full paint of ~60-100 nodes (ticks + labels + text shaping) every frame
while turning.

### Design

Extend `LIVE_SUBTREE` with a cache stored in the region's list:
```cpp
struct uid_live_cache_t {
	uid_node_id_t node; bool valid; bool unsupported;
	float originX, originY;              /* borderBox.x/y of the node at record time */
	std::vector<uir_vert_t> verts; std::vector<unsigned short> idxs;
	std::vector<uid_paint_cmd_t> cmds;   /* DRAW commands only, offsets into verts/idxs */
};
```
- **Record**: the first time a `LIVE_SUBTREE` is hit during replay (or during region record),
  paint the subtree with a *nested* recorder that captures DRAW commands into the live cache.
  Any `onClip` callback, `IMAGE_MASK_*`, `SHAPE_CLIP_*` or a nested `LIVE_SUBTREE` inside the
  subtree sets `unsupported = true` (fall back to the current live re-paint forever for that
  node until the region is re-recorded). **Disable paint culling for this record**: pass a flag
  so `PaintChromeNode` skips the `UID_OPT_PAINT_CULL` early-outs inside the live subtree —
  otherwise ticks that are currently outside the compass window are not recorded and would
  never appear when they scroll in. The fixed ancestor clip (compass body `overflow="hidden"`,
  recorded as the ambient scissor) cuts them at replay exactly as live painting does.
- **Replay**: `dx = st->borderBox.x - originX`, `dy = ...`. If `unsupported` or the cache is
  invalid → live re-paint (today's path). Else for each cached DRAW: copy its verts into a
  scratch array adding `dx/dy` to `x/y` and call `UIR_BatchTriangles` (same shader, same
  indices). Provide `UIR_BatchTrianglesOffset(shader, verts, nv, idx, ni, dx, dy)` in
  `uir_batch.c` that does the add during its existing copy loop so there is no extra pass.
- **Invalidation**: the live cache lives inside its region's list, so it is dropped whenever the
  region re-records (any non-translate PAINT dirt in that region) — simple and safe. Translate
  deltas (`UID_ApplyPendingTranslateDeltas`) do not invalidate anything (unchanged).
- Precision: positions are floats; the result is within float rounding of a live paint.

### Verification

Compass turning: `paint us` in Look drops to near the Still value; `replay hit %` unchanged.
Visual: compass ticks/labels enter and leave the window correctly at both ends; objective
arrows/pips (they also use bound translate-x) move correctly; damage pip translate + opacity ok.
`ui_live_translate_cache 0` restores live re-paint.

## 4.3 Cached geometry for foreach lifetime fades — `ui_live_opacity_cache` (default 1)

`ApplyForeachLifetimeOpacity` changes `lifetimeOpacityMul` on each foreach row wrapper and the
caller marks `PAINT` (`uid_collection.cpp:1636`), re-painting the messaging region 20 times per
second per fading row.

### Design

- Mark every row wrapper of a foreach with `hasForeachLifetime` as a **live-opacity block**:
  in `PaintChromeNode`, when recording and `node->foreachGenerated && parent foreach has lifetime`,
  emit a new command `UID_PAINT_CMD_LIVE_OPACITY {node, recordedMul}` and record the wrapper's
  draws into a nested cache exactly like 4.2 (same `uid_live_cache_t`, reuse code), with the
  row's `lifetimeOpacityMul` **forced to 1.0 during the record** so cached alpha bytes are the
  un-faded values. `unsupported` rules as in 4.2 (masks/clips inside a row → fall back).
- Replay: `mul = st->lifetimeOpacityMul`; if `mul <= 0.001` skip the block; else copy verts
  scaling `a = uir_batch_byte((a/255.0f) * mul)` (add `UIR_BatchTrianglesScaledAlpha`).
- In `uid_collection.cpp:1632-1638`: when the cvar is on and every changed wrapper has a valid
  live-opacity block (track a flag on `uid_node_state_t`: `liveOpacityCached`), **do not**
  `UID_MarkDirty(PAINT)`; just update `lifetimeOpacityMul`. If any changed wrapper is not
  cached → mark dirty as today.
- Deviation to confirm with the user: alpha is quantized to a byte at record and again after
  scaling, so a fading row can differ from a live paint by at most 1/255 in alpha. Fully-opaque
  and fully-faded rows are exact. If the user rejects this, keep the cvar at 0 and remove in
  Phase 5.

### Verification

Busy scenario with 3+ fading rows: `replay hit %` ~100%, `paint us` flat. Rows fade smoothly and
disappear at end of lifetime (the removal is a collection/STRUCTURE change and still rebuilds
normally). `ui_live_opacity_cache 0` restores today's behaviour.

## 4.4 Cvar dependency index for bind sync — `ui_bind_deps` (default 1), `ui_bind_deps_verify` (default 0)

`UID_SyncBindings` walks the whole tree three times per frame (`applyVisibility` x2 +
`syncRecursive`), doing string-keyed property lookups per node, even when no relevant cvar
changed. Implement this only if `bind us` is still > 25 in Still after 4.1-4.3.

### Design (skip untouched subtrees; keep the walk order and semantics)

- At `UID_CompileDocument` time and after every foreach rebuild (`regionsStale` moment), build:
  `doc->depCvars: std::vector<std::string>`, `doc->depNodes: std::vector<std::vector<uid_node_id_t>>`
  (parallel), `doc->depLastMod: std::vector<unsigned>`, `doc->impureNodes: std::vector<uid_node_id_t>`.
  For each node collect cvar names from: `visibleExpr`, `enabledExpr`, `styleExprs` values,
  `cvarBoundProps` (map prop → cvar name), `exprBoundProps` values, `bind` (`cvar:<name>`),
  label `text` brace interpolations. Reuse the tokenizer the AST cache uses
  (`uid_expr*.cpp` — find where the `cvar.` prefix is recognized and add
  `UID_ExprCollectCvarNames(const std::string &expr, std::vector<std::string> *out)`).
  A node is **impure** (always synced) if `!NodeBindBodyIsCvarPure(node)` or it is
  `foreachGenerated` or has hover/focus/press interaction state (`NodeBindInteractionDirty`).
- Per frame at the top of `UID_SyncBindings`: for each `depCvars[i]` read the host modification
  count (there is already a host describe cache keyed by `cvar_t*` + `modificationCount` in
  `cl_uirender.cpp` ~line 1058-1107 — add a backend function `cvarModCount(const char *name)`
  that returns it without formatting the value) and compare with `depLastMod[i]`; changed →
  mark all `depNodes[i]`. Also mark all `impureNodes`, all nodes with `bindSyncCached == false`,
  and every node when `doc->dirty & UID_DIRTY_BINDING` came from `UID_SyncCollections`
  (collections still run every frame as today).
- Propagate to ancestors via `parentOf` into `subtreeTouched[]`; in `applyVisibility` and
  `syncRecursive` add `if (!subtreeTouched[id]) return;` as the first statement. Everything else
  in `UID_SyncBindings` stays byte-for-byte the same, so semantics are preserved: the nodes we
  skip are exactly the nodes whose body `trySkipBindBody` would have skipped, plus their
  `visible` string re-read.
- `ui_bind_deps_verify 1` (debug): after the targeted sync, snapshot `doc->dirty` and a hash of
  every node's `properties` + `text`, run the full walk (deps disabled), compare, and
  `Com_Printf` once per divergence with node id and property. Use it in every scenario before
  sign-off; it must stay silent.

### Verification

`bind us` in Still <= 15, in Look <= 25 (compass heading changes only touch the tape/obj nodes).
Verify mode silent across all scenarios listed in 4.1.

## 4.5 cgame: skip unchanged HUD string pushes — `cg_hud_push_cache` (default 1)

`CG_SyncModernHudCvars` (`code/cgame/cg_drawtools.cpp:1686-1990`) formats and `cgi.Cvar_Set`s
~40 `ui_om_hud_*` strings every frame (vote, info/attacker, spectator, stopwatch, scores).
`Cvar_Set` with an identical string early-outs, but the hash lookups and `Com_sprintf` still run.
Measured cost is small (order of 10 us); do this last and only if `cg2d us` is still notable.

- Add `static void CG_HudSetCached(const char *name, const char *value)` with a fixed table
  (64 slots, name pointer/string → last value `char[256]`), calling `cgi.Cvar_Set` only when the
  value differs. Reset the table in `CG_Init` (map load) and when `cg_hud_push_cache` toggles.
  Replace the `cgi.Cvar_Set("ui_om_hud_...", ...)` calls in `CG_SyncModernHudCvars` (and in
  `CG_SyncModernObjectives` / `CG_UpdateCountdown` if they set `ui_om_hud_*`) with it. Keep the
  `Com_sprintf` calls (they are needed to know the value) unless the value is derived from a
  small integer that changed — do not over-engineer.

## Steps

1. 4.1 regions (data, invalidation, paint). Build. Scenario list + screenshots at `0/1`.
2. 4.2 live translate cache. Build. Compass/objective checks.
3. 4.3 live opacity cache. Build. Fade checks; confirm the 1/255 deviation with the user.
4. Protocol run → Results. If `bind us` > 25: 4.4 with verify mode; protocol again.
5. If `cg2d us` notable: 4.5.
6. Sign-off.

## Acceptance

- Busy: `replay hit % >= 95`, `paint us <= 40`, `bind us <= 25`, `ui total us <= 150`.
- No visual difference at cvar 0/1 except the documented alpha LSB during fades (4.3).
- Kill-feed row appear/disappear, chat, game messages, scoreboard hold, pack switching,
  `vid_restart`, map change all behave as before (no stale geometry, no missing rows).

## Pitfalls

- Node ids are remapped by foreach rebuilds; anything caching a node id must be invalidated on
  `STRUCTURE` (regions, live caches). Never cache `uid_node_def_t*`/`uid_node_state_t*` across
  `CloneForeachSubtree` (vectors reallocate).
- Do not record live-translate caches with paint culling enabled (see 4.2).
- A hidden region (`visible=false` or culled) must produce a *valid empty* chunk, not an invalid
  one, or it repaints every frame.
- Keep `doc->dirty & UID_DIRTY_PAINT` semantics (`UIR_ChromeCacheRequestRebuild` and the
  dispatcher read it).
- `UID_PaintOverlay` (modals/popups) is not retained; leave it alone.

## Results

### 4.1 — per-region retained paint (`ui_paint_regions 1`)

**Build:** 2026-09-13 ~01:15 `_build_deploy`  
**Method:** `debug-b55c7e.log` `runId=phase4-regions`. Play or spectate match (spectate hides weapons only). `ui_paint_list 1` (user archive; not force-set).

Gate (`replay >= 90%` before 4.2): **PASS**.

| window (match HUD) | replay% | hits/total | reg dirty/total | paint us | bind us | draws |
|--------------------|---------|------------|-----------------|----------|---------|-------|
| first up | 97.4 | 370/380 | 0 / 5 | 249 | 163 | 13 |
| settled | 98.9–100 | e.g. 4749/4750 | 0 / 5 | 140–158 | 81–101 | 19–23 |

HUD `region_paint` (`nreg=5`): 210/215 samples `dirty:0 replay:5`. Occasional `dirty:1 replay:4` from `foreach_fade` region 1 (killfeed) or `visible_expr` region 3 (health). Scoreboard (`nreg=3`) stays `dirty:1 replay:2` from `foreach_text` region 2 — expected, not the Still gate.

Whole-doc replay was 0% before 4.1 because any HUD dirt dropped the single list. Regions isolate that dirt.

### 4.2 — live translate cache (`ui_live_translate_cache 1`) — verify 2026-09-13

**Build:** 2026-09-13 ~01:23 `_build_deploy`  
**Method:** `debug-b55c7e.log` `runId=phase4-live-tx` + overlay windows. One accidental TAB (excluded below).

Cache path: **works**. Node 36 (compass tape) `live_record` 21× (`draws=25`, `unsup=0`); `live_offset` 576×; **0** `live_unsup` / `live_miss`. `dx` moves while turning, `dy=0`.

| slice | paint us | bind us | draws | replay% | notes |
|-------|----------|---------|-------|---------|-------|
| 4.1 Still (prior) | 140–158 | 81–101 | 19–23 | 99–100 | live tape re-paint |
| 4.2 HUD settled | 161–188 | 107–152 | 36–46 | 98–100 | offset tape |
| 4.2 Look (dx≠0) | 172–201 | 137–158 | 40–49 | 98–99 | Look ≈ Still |
| TAB (exclude) | 260 | 356 | 82 | 99 | `regTotal=4`, scissor 16 |

Look paint is near Still (gate). Absolute paint did **not** drop vs 4.1 — no-cull records the full tape, so draws rose ~+18. Bind is still the larger leftover (4.4). Tape recaptures when its region re-records (~every few seconds).

User verdict: pending.

### 4.3 — live opacity cache (`ui_live_opacity_cache 1`) — verify 2026-09-13

**Build:** 2026-09-13 ~01:30 `_build_deploy`  
**Method:** `debug-b55c7e.log` `runId=phase4-live-op` + overlay windows. Killfeed/chat fade ~10s.

Cache path: **works**. `op_record` 16× (`unsup=0`); `op_scale` 2541× with mul stepping `1.00 → 0.03` (e.g. node 219 `0.91…0.03`, node 215 `0.81…0.41`); **0** `op_unsup`. `fade_skip_dirty` 28 (throttled, foreach 155, n=1…5). `foreach_fade` `MarkDirty` **1** (late recapture) vs many skips.

| slice | paint us | bind us | draws | replay% | reg dirty |
|-------|----------|---------|-------|---------|-----------|
| 4.2 HUD settled | 161–188 | 107–152 | 36–46 | 98–100 | 0 / 5 |
| 4.3 fade windows | 167–226 | 75–110 | 36–53 | 99.4–100 | 0 / 5 |

Fades no longer dirty the messaging region every step. Replay stays ~100%. Absolute `paint us` did not drop further vs 4.2 (already replaying). Bind leftover is 4.4 (`bind us` still 75–110 vs Phase 4 gate ≤ 25).

### 4.4 — bind dependency index (`ui_bind_deps 1`) — verify 2026-09-13 (gate pass ~13:50; compass corrected ~14:36)

**Build:** 2026-09-13 afternoon `_build_deploy` (interned peeks +
`RemoveExpandedForeach` in-place compaction). Compass path corrected later the same day.  
**Method:** `debug-b55c7e.log` overlay `ui_perf_window` + `col_spike`. Compass verify:
`runId=post-fix-tape9` (`compass_publish` / `compass_layout_delta`).

**Root cause of earlier FAIL:** overlay `bind us` mean was a heavy-tail average. A few
foreach rebuild frames spent **1.0–1.8 ms** in `RemoveExpandedForeach` (deep-copy whole
`doc->nodes`/`states`). Steady bind was already ~7–11 µs. In-place `std::move` compaction
cut `exRemove` from **1013–1580 µs → 38–51 µs**.

**Compass (4.2 leftover, corrected 14:36):** a second paint-time heading integrator
(unwrapped raw heading + period/continuity rebases, hardcoded `parentW/180`) fought the
authored XML (`heading % 360`, `width/fov-deg=270`). That swapped period copies (N where S
was) and blanked the FOV. Fix: publish exact heading into the cvar, let XML/layout own
modulo/period, replay live-translate as `borderBox − origin` only, and drop the
translate-only bind shortcut that could skip expression eval. Post-fix: **0**
`tape_jump`/`tape_stamp`; north wraps show one-period layout deltas (~1414 px at
scaled 800/270); user confirmed visual OK.

| slice (settled HUD) | overlay bindUs | bindSteadyUs | paint us | replay% | notes |
|---------------------|---------------|--------------|----------|---------|-------|
| Still / idle windows | **7–10** | **7–10** | 156–216 | 99.8–100 | Still ≤ 15 **PASS** |
| Look / turn windows | **11–22** | **11–22** | 165–278 | 99.5–100 | Look ≤ 25 **PASS** |
| Spike-contaminated join | 15–48 mean | 8–17 | — | — | exclude; `bindSpikeShare` only |

Gate (Still overlay ≤ 15, Look ≤ 25): **PASS**. Compass correctness: **PASS**.

### Phase 4 Busy gates (post-4.4 / compass fix)

| gate | target | this run | result |
|------|--------|----------|--------|
| replay hit % | ≥ 95 | 99.5–100 | **PASS** |
| paint us | ≤ 40 | 156–278 | **FAIL** (replay cost; hit ~100%) |
| bind us (overlay Still) | ≤ 15 | **7–10** | **PASS** |
| bind us (overlay Look) | ≤ 25 | **11–22** | **PASS** |
| ui total us | ≤ 150 | 191–351 | **FAIL** (dominated by paint) |
| gl gets | 0 | 0 | PASS |
| fbo binds | ≤ 4 | 3 | PASS |
| draws (program) | ≤ 25 | 35–51 | FAIL (tape + feeds; known) |

Paint / ui-total leftovers are retained-list replay cost, not bind. Proceed to 4.5 only for
`cg2d` string-push savings (small; plan says optional).

### 4.5 — cgame HUD push cache (`cg_hud_push_cache 1`) — verify 2026-09-13 ~14:47

**Build:** 2026-09-13 14:44 `_build_deploy` (now builds/copies `cgame.dll`; prior deploys had left a Sept 7 DLL).  
**Method:** `debug-b55c7e.log` `runId=phase4-45` `message=hud_push_cache` (1 Hz skip/set).

Warm-up first second: skipPct **59.4** (cache fill). Settled: **99.8–100.0%** skip, with **2–5** `set`/s (changing values still push). Example settled windows: `skip:8060 set:4`, `skip:35518 set:2`, `skip:45947 set:5`.

| check | result |
|-------|--------|
| Cache enabled in run | **PASS** (`enabled:1`) |
| Unchanged pushes skipped | **PASS** (~100% after warm-up) |
| Changed values still set | **PASS** (nonzero `set` each second) |
| HUD strings (user) | assume OK unless reported |

4.5: **PASS** (optional / small; does not fix Phase 4 paint/ui-total Acceptance misses).

## Todo

- [x] 4.1 parent map, region map, per-region invalidation, per-region record/replay, cvar (verified 2026-09-13)
- [x] 4.2 live translate cache (Look ≈ Still, replay held; verified 2026-09-13)
- [x] 4.3 live opacity cache (record at mul 1, scaled replay, skip-dirty; fade OK 2026-09-13 12:11)
- [x] Protocol run, Results (12:11 cut; Busy paint/ui-total/overlay-bind still over)
- [x] 4.4 dependency index + foreach remove compaction (Still/Look bind gates **PASS** 2026-09-13)
- [x] Compass correctness restored to XML/layout authority (user OK + logs 2026-09-13 ~14:36)
- [x] 4.5 cgame push cache (`cg_hud_push_cache 1`; skip ~100% settled; verified 2026-09-13 ~14:47)
- [ ] Sign-off (Busy paint/ui-total still over plan targets; known replay cost)
