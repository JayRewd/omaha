# Modern UI rendering pipeline

**Audience:** Implementers / LLMs recreating or extending the draw path.  
**Scope:** `code/uirender/` compositor, 2D primitives, menu world, model previews, in-game crosshair chrome.  
**Out of scope here:** XML layout grammar ([designformat.md](designformat.md)), widget hit-testing inside `uidesign`.

**Host entry points:**

| Call | When |
|------|------|
| `CL_UIR_RenderDisconnectedMain` | Disconnected: topmost open menu with `backdrop="menu-map"` drives WORLD + layered CHROME/OVERLAY |
| `CL_UIR_RenderModernOverlay` | In-match: layered CHROME + PREVIEWS + OVERLAY for open menus (`draw-order` ascending) |
| `CL_UIR_DrawCrosshair` | Active gameplay: paints HUD-layer menus only (`draw-order` ≤ 4, e.g. active HUD pack at 4, hold-TAB `scoreboard` at 3) |

**HUD packs:** When `ui_om_hud` selects a modern pack (`classic`, `modern`, or a PK3 pack), `CL_UIMenu_SyncAutoMenus` auto-opens the active pack from `ui/modern/huds/`. In-match crosshair is drawn procedurally in cgame (`CG_DrawCrosshair` → `code/qcommon/crosshair_draw.c`), not via an XML menu. Legacy HUD (`ui_om_hud legacy` or `ui_legacy 1`) closes modern HUD packs and restores UIFAKK `hud_*` menus plus retail texture crosshair. Host sync: `UIR_Hud_Sync` + `CG_SyncModernHudCvars` each frame; pack XML binds `ui_om_hud_*` cvars. Settings previews use `role="crosshair-preview"` / `role="sniper-preview"` host regions — HUD chrome itself is declarative XML, not host-region paint.

**Menu dispatcher:** `code/client/cl_uimenu_dispatcher.cpp` maintains a VFS registry (`ui/**/*.xml` with `menu-id`) and a separate HUD registry (`ui/modern/huds/*.xml` with `hud-id`). Commands: `ui_open menu <id>`, `ui_close menu <id>`, `+ui_open menu <id>`, `-ui_open menu <id>`, `ui_menu_list`, `ui_menu_reload`, `ui_hud_list`. Paint and input iterate open menus sorted by `draw-order` (input: highest first).

---

## Compositor phases (disconnected main)

Per frame, `UIR_BeginDisconnectedFrame` → `UIR_EndDisconnectedFrame`:

```
IDLE → WORLD → CHROME → PREVIEWS → OVERLAY → IDLE
```

| Phase | What runs |
|-------|-----------|
| **WORLD** | `UIR_MenuWorldDraw` — full-window BSP backdrop from `menu-map-views` catalog |
| **CHROME** | Host callback (`UID_DrawChrome`) — 2D panels, text, shapes, queued preview registration |
| **PREVIEWS** | `UIR_ModelPreviewDraw` for each `UIR_QueueModelPreview` rect from CHROME |
| **OVERLAY** | Host callback (`UID_DrawOverlay`) — modals / dropdown overlays above previews |

2D drawing (`UIR_DrawSolidRect`, `UIR_FillPolygon2D`, `UIR_FillPath2D`, `UIR_StrokePath2D`, fonts, images) is only legal in **CHROME** or **OVERLAY** phases.

`UIR_EndDisconnectedFrame` restores fullscreen 2D before PREVIEWS so per-rect 3D viewports do not inherit chrome scissors.

### Connected overlay (in match)

`UIR_BeginOverlayFrame` skips WORLD:

```
IDLE → CHROME → PREVIEWS → OVERLAY → IDLE
```

Gameplay world remains visible behind chrome. `<model>` widgets queued during CHROME are drawn in PREVIEWS (team selection / connected main), then dropdowns and modal overlays paint above them.

### When each path runs

| State | Modern UI draw |
|-------|----------------|
| `CA_DISCONNECTED`, intro finished, not `server_loading`, not local `com_sv_running` | Disconnected main (`main` menu, `backdrop="menu-map"`) |
| `CA_ACTIVE`, connected modern `main` overlay open (`draw-order` 8) | Overlay frame over live gameplay |
| `CA_ACTIVE`, gameplay only | HUD-layer menus (active HUD pack from `huds/` at draw-order 4, hold-TAB `scoreboard` at 3); crosshair in cgame |
| Connecting / loading / cinematic | Auto-managed menus closed; legacy connecting/loading menus |

### Manual test checklist

1. Disconnect → `main` auto-opens with menu-world backdrop.
2. Join MP match → procedural crosshair visible in cgame; no full-screen chrome.
3. Escape → modern `dm_pause` when a modern HUD pack is active (`ui_om_hud` not `legacy`); legacy HUD still opens UIFAKK `dm_main`.
4. Main Menu in `dm_pause` → `ui_close` + `pushmenu main` (connected overlay via existing `pushmenu main` remap).
5. Escape with connected `main` or `dm_pause` open → closes that surface, returns to gameplay.
6. Repeat steps 3–5 several times; nothing latches (mouse look / HUD / KEYCATCH_UI restore each time).
7. Hold Tab (`+ui_open menu scoreboard`) → scoreboard at draw-order 3; release closes it.
8. With connected overlay open, `vid_restart` and a resolution change must keep `main` laid out (not blank). Surface is pushed on every open via `CL_UIR_ApplyMenuSurfaceNow` / unconditional `CL_UIMenu_ApplySurface` in `CL_UIR_UpdateModern` — menus opened after the last resolution change must still receive the surface.
9. `ui_menu_list` shows registered ids; PK3 override of same `menu-id` wins after `ui_menu_reload`.
10. Hold TAB in MP → `scoreboard` menu opens; K/D column populated.
11. TDM → Allied/Axis section headers and team row colors; FFA → Players/Spectators sections.
12. TOW / Liberation → objective strip or toggle row visible in scoreboard header.
13. `ui_legacy 1` → Escape → `dm_main` → Main Menu → retail legacy main; scoreboard stays legacy `UIListCtrl`.
14. Modern pause (`ui_om_hud classic`): full modern XML pause tree (Escape + team/weapon/vote) — **no UIFAKK**. Stage is full canvas as 640×480 virtual (URC `%` of stage). `mpoptions` → modern main Play; Escape Main Menu → Settings.
15. Spectate follow (modern HUD): combat HUD shows followed ammo/weapons; banner shows `Following <name>` above spectator prompt; no classic target-info panel.
16. Legacy isolation: `+set ui_om_hud legacy` and/or `+set ui_legacy 1` — Escape → UIFAKK `dm_main`; team/weapon/vote stay URC. Chase-spectate still receives followed combat stats in the snap (for any client); UIFAKK chrome does not use the modern cvar HUD.

### Modern MP pause (`dm_pause`)

When `CL_UIR_UseModernHudPack()` is true, Escape opens the pause menu named by the active HUD pack’s `pause-menu` attribute (`draw-order` 7). Panel router: `ui_om_pause_panel`.

| HUD pack | `pause-menu` | Menu file | Notes |
|----------|--------------|-----------|-------|
| `classic` | `dm_pause` | [`menus/dm_pause.xml`](../../assets/main/ui/modern/menus/dm_pause.xml) | Retail URC layout + art/models |
| `modern` | `dm_pause_modern` | [`menus/dm_pause_modern.xml`](../../assets/main/ui/modern/menus/dm_pause_modern.xml) | Main-menu styled shell (no art/models) |
| `competitive` | `dm_pause_modern` | [`menus/dm_pause_modern.xml`](../../assets/main/ui/modern/menus/dm_pause_modern.xml) | Same shell as modern |

Companions are declared only on the HUD pack XML — the host looks them up via `CL_UIR_DmPauseMenuId` / `CL_UIR_ScoreboardMenuId` (registry), not hard-coded HUD ids.

**Scale:** Root stage is `width/height 100%` treated as retail **640×480 virtual** (same stretch as UIFAKK `vid/640` × `vid/480`). Plaques and hits use URC rects as `%` of that stage (or `%` of the plaque).

**Look:** Base `escmenu` / `selectteam` / `selectprimary` / … — labels baked in textures; invisible hits, including the stock outside-plaque dismiss zones. Vote lists use gold text panels (no wood art).

**Remaps:** `dm_main`, `SelectTeam` / `SelectFFAModel` / `ObjSelectTeam`, `SelectPrimaryWeapon*`, `mpoptions`, vote menus → `CL_UIR_OpenDmPause`. `options` redirects to connected modern main Play. Legacy HUD unchanged.

### Spectate combat HUD (modern only)

Server copies followed combat stats in `Player::CopyHudCombatStats` for chase-spectate (not first-person follow). `CG_SyncModernHudCvars` sets `ui_om_hud_following_text`; classic banner binds it. Client also falls back to `STAT_INFOCLIENT_HEALTH` for the health bar when combat bits are not yet in the snap.

---

## Viewport and coordinates

- Logical layout space: SDL window logical W×H (`UID_SetSurface`).
- Framebuffer ortho: drawable W×H with scale mapping (`UIR_ViewportMakeOrtho`).
- Resolved UI lengths use `refScale × ui_scale` (see designformat § sizing; ref = 1920×1080 contain).
- Clip stack: `UIR_PushClipRect` / `PopClipRect` intersects scissor in framebuffer space (OpenGL bottom-left scissor Y corrected).

---

## A. Axis-aligned 2D solids

| API | Purpose |
|-----|---------|
| `UIR_DrawSolidRect` | Solid fills (panels, 1px rules as thin boxes) |
| GPU path (`ui_gpu_draw 1`) | `UIR_BatchQuad` → `re.DrawUI2D` batched triangles |
| CPU fallback (`ui_gpu_draw 0`) | `UIR_Draw2D_Box` → `re.DrawBox` immediate quads |

Do not route axis-aligned fills through the polygon AA path.

---

## B. Filled polygons and SVG paths

**Default (GPU, `ui_gpu_draw 1`):** `UIR_TessFillPath` / `UIR_TessStrokePath` expand paths to triangle meshes. Fills use **libtess2** (SVG even-odd / non-zero winding) in draw space; edges get a **4-step analytic fringe** (alphas 255→170→85→0, width ≈ `max(1px, 1.25×scale)` FB). Strokes offset in draw space with matching fringe rings. Meshes accumulate in `UIR_BatchTriangles`, flushed via `re.DrawUI2D` (`tr_ui_batch.c`).

**Fallback (CPU, unsupported geometry or `ui_gpu_draw 0`):** `UIR_PathFill` / `UIR_PathStroke` coverage rasterizer → horizontal `re.DrawBox` spans. `UIR_PathContainsPoint` matches CPU fill semantics and is used by unit tests as the golden reference.

**Path API:** `UIR_FillPath2D`, `UIR_StrokePath2D` after SVG parse (`uir_svg` / `UIR_DrawSvgGeometry`). Parsed/mapped paths are cached per frame in `uir_backend.c` (64-entry direct map).

### CPU coverage rule (fallback only)

Compute coverage in **framebuffer pixels**. Pre-scale fill + upscale causes magnified stair-steps.

Algorithm sketch:

1. Polygon/path bounds → integer FB pixel range (cap `UIR_MAX_POLYGON_AREA` = 4096×2160 FB pixels).
2. Per pixel: sample center mapped to draw space; `cover ∈ [0,1]`.
3. Run-length encode horizontal spans with `SetColor(α × cover)`.

| Vertex count | Method |
|--------------|--------|
| ≤ 4 (convex) | Edge signed-distance × winding; soft edge ~1 FB pixel |
| &gt; 4 | Even-odd point-in-polygon + 8×8 supersample |

Used for: chamfers, skew tabs, chevrons, Allied mark (see [allied-logo-shape.md](../modern-ui/allied-logo-shape.md)), crosshair shape previews.

`shape-rotation` applies at paint time around geometry center.

### GPU controls

| Cvar | Default | Purpose |
|------|---------|---------|
| `ui_gpu_draw` | `1` | Enable batched GPU UI path; `0` restores legacy span/`DrawBox` behavior (MSAA FBO requires `1`) |
| `r_uiFramebuffer` | `1` | Optional offscreen MSAA UI target (GL1 FBO path); `0` = direct backbuffer |
| `r_uiMultisample` | `8` | MSAA samples when `r_uiFramebuffer 1` (latched; clamped 2/4/8) |
| `ui_render_stats` | `0` | Dump `sampled/runs/batches/batchTris/tessFallbacks/tessLibFails/tessContoursIn/Out` to stderr |

When `r_uiFramebuffer 1` and FBO procs are available, CHROME/OVERLAY render to an MSAA FBO (`tr_ui_fbo.c`). **Fringe is disabled only when MSAA samples &gt; 0**; if FBO creation fails or `r_uiMultisample 0`, fringe stays enabled as fallback AA. `Draw_StretchPic`, `Set2DWindow`, and `RE_DrawUI2D` preserve the active UI FBO binding; MSAA is not globally disabled while drawing into the MSAA target.

**MSAA FBO alpha compositing:** UI layers inside the FBO use straight-alpha blending (`SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`) into a transparent clear, so the resolved texture RGB is **premultiplied** (`rgb×α`) while alpha stays straight. `RE_EndUI2DTarget` composites over the game with **`GL_ONE`, `GL_ONE_MINUS_SRC_ALPHA`** (premultiplied-over). Using `SRC_ALPHA` on composite would apply alpha twice and darken semi-transparent fills/strokes.

---

## C. Fonts (TrueType atlas)

**Faces:** Oswald Medium (`body`, 400), Oswald SemiBold (`control`, 600) — static TTFs under `fonts/`.

**Bake:** `stb_truetype` at pixel height matched to current FB scale; use **em-square** scale (`stbtt_ScaleForMappingEmToPixels`), not ascent−descent-only height.

**Draw:** Per glyph — `SetColor`, FB-snapped quad, `DrawPicStretched` with half-texel UV inset, advance by `xadv`. Rebake on resolution / scale change (`UIR_FontInvalidateGpu` on `vid_restart`).

---

## D. Images

**Registry** (`lib/images.xml` / HUD image libs): `<images><image id="…" src="…"/></images>` maps ids to VFS paths.

**Decorative backgrounds:** any paint box may set `background-image` + `background-fit` (`stretch`, `repeat`, `contain`, `cover`) + optional `background-scale`. The image does **not** drive layout size — the box comes from `width`/`height` / children.

**Content leaf `<image>`:** replaced element with `src` / `fit` / `scale`. Layout sizes from texel measure (`imageMeasure`): both `auto` → natural DIP size; one fixed axis + other `auto` → aspect-preserved. Prefer this for icons in rows (killfeed, etc.).

Paint order for **axis-aligned / background boxes:** **image → fill → stroke**. For **SVG path shapes**, paint order is **stroke then fill** when both are set (see designformat Stroke). Shape clipping uses stencil when available; axis-aligned rects may use scissor only.

---

## E. Menu world backdrop (`menu-map-views`)

Catalog: `assets/main/ui/modern/lib/sources.xml`, source id `menu-map-views`. Active entry: cvar `ui_om_menu_map_view` (default `remagen`).

Per-item fields: `bsp`, `vieworg`, `pitch`/`yaw`/`roll`, `fov`. Fog and weather come from the loaded map (BSP worldspawn + companion `.scr`), not the catalog.

### Load / switch

1. Resolve view from document + cvar (fallback: source `default`, then built-in Remagen).
2. **Same BSP** — update camera only.
3. **Different BSP** — `CM_LoadMap`, `LoadMenuWorld`, parse map env (fog entities + rain script).
4. Missing BSP — solid black `DrawBox` fallback once (`UIR_MenuWorldEnsureLoaded` → UNAVAILABLE).

### Camera (Remagen default)

| Field | Value |
|-------|-------|
| `vieworg` | `(947.23, -649.70, -68.50)` eye Z (standing viewheight +82 vs feet) |
| angles | pitch −31.15°, yaw −67.03°, roll 0 |
| `fov_x` base | 80° at 4:3 |

### Map fog (from BSP worldspawn)

Parsed at load via `UIR_MapEnvLoad`: `farplane`, `farplane_color`, optional `farplane_bias` (defaults to `farplane × 0.18` when absent). Example: mohdm3 Remagen uses `farplane 7000`, color `(0.5, 0.4, 0.2)`; mohdm5 Snowy Park uses `farplane 1800`, dark gray tint.

### Rain / snow (from BSP + map script)

- `func_rain` brush volumes from the BSP entity lump define where particles spawn.
- Companion map script (`maps/dm/<map>.scr`) supplies `level.rain_*` params (density, shader, speed, etc.). Example: mohdm5 sets `textures/snow0` with 12 shader variants.
- Respects user `cg_rain` cvar. No `func_rain` volumes → no weather.
- Menu cameras are fixed catalog viewpoints (often outside in-world rain brushes), so particles spawn in a shell around `vieworg` when the map has rain; script params still apply.

### FOV widen (match `CG_CalcFov`)

```
aspect   = width / height
fovRatio = aspect * (3/4)
fov_x    = 2 * atan(tan(radians(80/2)) * fovRatio)   // when aspect ≠ 4:3
fov_y    = from view distance and height
```

Draw: `refdef` pixel rect → `ClearScene` → rain polys (if map has weather) → `RenderScene` → restore 2D. No player entities in this pass.

---

## F. Character model previews (`<model>`)

Queued in CHROME via `UIR_QueueModelPreview`; drawn in PREVIEWS phase.

XML attributes (see `designformat.md`): `model` or `bind` (or `team` for cvar fallback), `anim`, `anim-variant` (0-based index into a TAF_RANDOM alias group — **required** when factions share underlying `.skc` files), `anim-phase` `[0,1)`, optional `angles`, `fov`, `scale`, `offset`, `bbox`, `bbox-from-model`, `framing-scale`, `color`.

**Animation note:** `dtikianim_t::m_aliases[]` stores indices into the **global** skeletor cache (deduplicated by `.skc` path). Two different tikis can share the same global anim index; use `anim-variant` in XML to pick distinct clips explicitly.

Per preview rect (destination pixels):

| Refdef / entity | Value |
|-----------------|-------|
| `rdflags` | `RDF_HUD \| RDF_NOWORLDMODEL` |
| `fov_x` | `fov` attribute or 30° default |
| `fov_y` | `2 * atan(tan(fov_x/2) * (h/w))` — **required** on non-square rects |
| `viewaxis` | **identity** (do not apply live eye angles) |
| `time` | client `realtime` (idles must advance) |
| model | TIKI handle from `model` / `bind` cvar / team fallback |
| `angles` | `angles` attribute or `(10, 180, 0)` stock facing |
| anim | `anim` attribute or team default idle |
| anim index | `TIKI_Anim_NumForNameVariant(tiki, anim, anim-variant)` |
| origin framing | `bbox` / `bbox-from-model` or stock player bbox + `framingScale` |

Draw per rect: `ClearScene` → `AddRefEntityToScene` → `RenderScene` → restore 2D.

Overlays painted in OVERLAY phase stack above previews.

---

## G. In-game crosshair (modern HUD)

Modern HUD packs draw the crosshair procedurally in **cgame**, not via XML overlay menus:

- `CG_DrawCrosshair` → `CG_DrawModernCrosshair` when `CG_UseModernHudPack()` and `cg_crosshair_mode != none`.
- Shared geometry in `code/qcommon/crosshair_draw.c`; CS2-compatible cvars `cg_crosshair*` (+ `cl_crosshair*` aliases).
- Settings preview uses `role="crosshair-preview"` host region (same resolver, static dynamic factor).
- **Dynamic crosshair spread** (client-only): `cg_crosshair_spread.c` mirrors MOHAA `Weapon::GetSpreadFactor` + `firespreadmult` using weapon profiles extracted from retail `.tik` files; shot events hook `viewkick` in cgame. Cvars: `cg_crosshair_dynamic`, `cg_crosshair_dynamic_movement` (movement vs spray-only dynamic).
- Legacy HUD keeps retail texture crosshair (`cg_crosshair` shader path).

---

## H. Renderer lifecycle hooks

| Event | `uirender` response |
|-------|---------------------|
| `UIR_OnRendererRegistration` | Reset compositor, invalidate font/image GPU, `MenuWorldMarkNeedsReload` |
| `UIR_OnResolutionChanged` | Invalidate font/image GPU caches |
| `CL_UIR_OnRendererRegistration` (client) | Re-wire backends, refresh surface if modern main still active |
| Shutdown | `UIR_Shutdown` → fonts, menu world, compositor |

See [LIFECYCLE.md](../../code/uirender/LIFECYCLE.md) for client integration checklist.

---

## I. Renderer selection (OpenGL1 default)

| Context | Renderer | How |
|---------|----------|-----|
| Normal play (menu, HUD, in-game) | **OpenGL1** | Default; forced on every `vid_restart` unless overridden at launch |
| Weapon PNG bake (`ui_bake_*`) | OpenGL2 (when export is built) | Must pass `+set cl_renderer opengl2` on the **command line** |

Rules (added in OPM):

- `cl_renderer` is **not** `CVAR_ARCHIVE` — saved configs cannot switch the play renderer.
- In-console `cl_renderer opengl2` is ignored on the next renderer init; only `+set cl_renderer …` at process startup counts.
- Menu world backdrops, compositor, fonts, and HUD use the same ref API on both renderers; GL1 is the supported play path.

Bake scripts under `artifacts/bake_mp_weapons*.sh` and `code/uirender/tools/uir_bake_*.py` already pass `+set cl_renderer opengl2`.

---

## J. Port checklist

- [ ] GPU path: `ui_gpu_draw 1` batches paths/fonts/images via `re.DrawUI2D`; `ui_gpu_draw 0` matches legacy spans
- [ ] CPU fallback still samples FB pixel centers when tessellation returns `UIR_ERR_UNSUPPORTED`
- [ ] Oswald baked with em-square at destination pixel size; glyphs FB-snapped + UV inset
- [ ] Compositor order: WORLD → CHROME → PREVIEWS → OVERLAY (disconnected)
- [ ] Overlay path skips WORLD; crosshair is separate HUD hook
- [ ] `menu-map-views` + staged BSP swap; FOV widen from 4:3 base 80°
- [ ] Model previews: `fov_x=30`, aspect `fov_y`, identity viewaxis, realtime anim
- [ ] Path stroke pipeline for crosshair outlines and SVG chrome
- [ ] Paint modals/overlays after 3D previews when they must sit on top
