# UI design format (version 1)

XML is the authoring language for the modern UI. This document is the **version-1 contract** aligned with `code/uidesign/` and `assets/main/ui/modern/`.

**Runtime host:** `code/client/cl_uimenu_dispatcher.cpp` discovers registerable menus under `ui/**/*.xml`, opens them via `ui_open menu <id>` / `+ui_open menu <id>`, and paints through `code/uirender/`. Legacy `ui_legacy 1` keeps the stock `UIFAKK` / `pushmenu` path unchanged.

**Legacy gate:** `ui_legacy` (`CVAR_INIT`, default `0`). When `1`, the stock disconnected main uses legacy `UIFAKK` menus instead of this stack.

**Related docs:** [ui-rendering-pipeline.md](ui-rendering-pipeline.md) (compositor / draw primitives), [LIFECYCLE.md](../../code/uirender/LIFECYCLE.md), [README.md](README.md), regression tests in `code/uidesign/tests/test_uir_design.cpp`. Snapshots under `artifacts/UI/` are historical only.

### Contents

- [Document root](#document-root)
- [Registerable menus (`menu-id`)](#registerable-menus-menu-id)
- [HUD packs (`hud-id`)](#hud-packs-hud-id)
- [Layout and sizing](#layout-and-sizing)
- [Definitions, imports, stock libraries](#definitions-imports-stock-libraries)
- [Lexical contexts (one-way)](#lexical-contexts-one-way)
- [Bindings and actions](#bindings-and-actions)
- [Navigation and screen structure (`main.xml`)](#navigation-and-screen-structure-mainxml)
- [Collection sources (`lib/sources.xml`)](#collection-sources-libsourcesxml)
- [Controls catalog mapping](#controls-catalog-mapping)
- [Scrollbars](#scrollbars)
- [Element grammar (summary)](#element-grammar-summary)
- [Canonical minimal sample](#canonical-minimal-sample)

---

## Document root

```xml
<ui version="1">
    <definitions>...</definitions>
    <canvas>...</canvas>
</ui>
```

- `<ui>` accepts only `version="1"`.
- Exactly one `<definitions>` then one `<canvas>`.
- Property names are lowercase kebab-case (`font-size`, `parent.width`).

Definition-only libraries (for `<import>`) use:

```xml
<ui-library version="1">
    <import src="child/theme.xml"/>
    <defaults .../>
    <vars>...</vars>
    <fonts>...</fonts>
    <shapes>...</shapes>
    <templates>...</templates>
    <modals>...</modals>
    <sources>...</sources>
</ui-library>
```

---

## Registerable menus (`menu-id`)

Menus that participate in the layered dispatcher declare metadata on `<definitions>` only:

```xml
<definitions menu-id="main" draw-order="8" backdrop="menu-map">
```

| Attribute | Required | Values | Purpose |
|-----------|----------|--------|---------|
| `menu-id` | yes | unique string | Console id for `ui_open menu <id>` |
| `draw-order` | yes | `0`–`9` | Paint/input layer (`0` = back, `9` = top) |
| `backdrop` | no | `none` (default), `menu-map` | Whether this menu drives the 3D menu-world backdrop |

- `<ui-library>` import files are **not** registerable (no `menu-id`).
- Draw-order `≥ 5` is treated as interactive (captures input when open).
- Mods ship any `ui/**/*.xml` with these attributes; the engine scans the VFS at init / `ui_menu_reload` (later PK3 entries override duplicate ids).

Built-in menus:

| `menu-id` | `draw-order` | `backdrop` | File |
|-----------|--------------|------------|------|
| `scoreboard` | 3 | none | `ui/modern/menus/scoreboard.xml` |
| `dm_pause` | 7 | none | `ui/modern/menus/dm_pause.xml` |
| `dm_pause_modern` | 7 | none | `ui/modern/menus/dm_pause_modern.xml` |
| `main` | 8 | `menu-map` | `ui/modern/main.xml` |

Host selects pause and scoreboard companions from the active HUD pack’s `<definitions>` (`pause-menu` / `scoreboard-menu`). Pause panel routing uses `ui_om_pause_panel` plus `set-cvar` / `cbuf` invokes — see the pipeline doc’s Modern MP pause section.

---

## HUD packs (`hud-id`)

In-match HUD chrome is selected by **`ui_om_hud`** (`CVAR_ARCHIVE`, default `classic`). Packs live under `ui/modern/huds/*.xml` and use the same document pipeline as menus, but declare **HUD metadata** instead of `menu-id`:

```xml
<definitions hud-id="classic" hud-label="Classic" draw-order="4"
             pause-menu="dm_pause" scoreboard-menu="scoreboard">
```

| Attribute | Required | Purpose |
|-----------|----------|---------|
| `hud-id` | yes | Stable selection id (`classic`, mod `arena`, …) |
| `hud-label` | yes | Display name in settings (`source="hud-packs"`) |
| `draw-order` | yes | Paint layer (HUD chrome uses `4`) |
| `pause-menu` | yes | `menu-id` opened for Escape / MP pause |
| `scoreboard-menu` | yes | `menu-id` opened for hold-TAB scoreboard |

- **No `menu-id`** on HUD packs — the dispatcher opens the active pack by registry id, not `ui_open menu`.
- Built-in **`legacy`** is synthetic (not an XML file): UIFAKK HUD + retail crosshair/scoreboard paths.
- **`ui_legacy 1`** forces legacy for everything; the HUD picker is hidden.
- Mods drop `ui/modern/huds/myhud.xml` into a PK3; after load or `ui_menu_reload` it appears in settings.

Shared modern overlays (when a non-legacy HUD pack is active): the HUD-declared `scoreboard-menu`, connected `main`, and the HUD-declared `pause-menu`. Crosshair is cgame procedural (`cg_crosshair_mode`).

Host-bound state uses temporary `ui_om_hud_*` cvars synced from `cl_hud_host` / `CG_SyncModernHudCvars`. HUD chrome (health, compass, weapons bar, grenades) is **declarative XML** bound to those cvars — not imperative host paint.

Containers with a non-empty `role` call the backend `drawHostRegion` hook (`uid_draw_host_region` in `cl_uirender.cpp`). Live roles:

| `role` | Where | Purpose |
|--------|-------|---------|
| `crosshair-preview` | settings (`controls.xml`) | WYSIWYG crosshair plate |
| `sniper-preview` | settings (`controls.xml`) | Sniper open-reticle plate |
| `server-list` | legacy `<server-list>` only | Imperative browser paint (unused in shipped composable browser) |

Built-in HUD packs:

| `hud-id` | `pause-menu` | `scoreboard-menu` | File |
|----------|--------------|-------------------|------|
| `legacy` | — | — | synthetic (no XML) |
| `classic` | `dm_pause` | `scoreboard` | `ui/modern/huds/classic.xml` |
| `modern` | `dm_pause_modern` | `scoreboard` | `ui/modern/huds/modern.xml` |
| `competitive` | `dm_pause_modern` | `scoreboard` | `ui/modern/huds/competitive.xml` |

Representative import chains:

```xml
<!-- classic.xml -->
<import src="ui/modern/lib/hud_classic_theme.xml"/>
<import src="ui/modern/lib/shapes.xml"/>
<import src="ui/modern/lib/hud_images.xml"/>
<import src="ui/modern/lib/hud_messaging.xml"/>
<import src="ui/modern/lib/hud_classic.xml"/>
<import src="ui/modern/lib/hud_ammo.xml"/>

<!-- modern.xml -->
<import src="ui/modern/lib/theme.xml"/>
<import src="ui/modern/lib/hud_modern_tokens.xml"/>
<import src="ui/modern/lib/shapes.xml"/>
<import src="ui/modern/lib/hud_modern_images.xml"/>
<import src="ui/modern/lib/hud_messaging.xml"/>
<import src="ui/modern/lib/hud_modern.xml"/>
<import src="ui/modern/lib/hud_modern_grenade.xml"/>
```

---

## Layout and sizing

Containers support `type="vertical|horizontal|overlap"` with `halign` / `valign`:

| `type` | Behavior |
|--------|----------|
| `vertical` | Flex column — children pack on the main (vertical) axis |
| `horizontal` | Flex row — children pack on the main (horizontal) axis |
| `overlap` | Children stack in the same parent content box (paint order = document order, first = back). Each child is positioned with its own `width`/`height`, `margin`, `halign`, `valign`, and optional edge offsets `left` / `top` / `right` / `bottom`. `gap` is ignored. The overlap **parent** does not pack children — set alignment on each child (or on the overlap node when it is positioned by an outer container). `equal-spacing` / `space-between` on an **overlap child** are layout errors. |

**`translate-x` / `translate-y`** — length props (default `0`, expr-bindable `{…}px`). Applied **after** flex/overlap packing when writing the node’s boxes (CSS-transform-like). Sibling packing ignores the translate; the node’s subtree lays out from the shifted content box; hit-test and paint follow the shifted boxes. Parent `overflow="hidden"` / `scroll` still clips via `effectiveClip`.

**`rotation` / `rotation-origin`** — paint-time degrees (and optional origin) around the node’s geometry center (or authored origin). Bindable on layout/paint sync (`rotation="{cvar.ui_om_hud_compass_angle}"`). Does not affect flex packing or hit-test boxes. Used heavily by classic compass needles. On text leaves (`<label>`, `<button>`, dropdown value text), the same attrs also rotate glyphs around that shared pivot; layout boxes stay axis-aligned. Parent `rotation` does **not** transform child text (no subtree transform stack). When both `rotation` and `text-skew` are set on a leaf, **rotation wins** (skew is ignored).

**`opacity`** — bindable runtime numeric (`0`–`1`) on layout/paint sync.

For `vertical` and `horizontal`:

| Value | Main axis | Cross axis |
|-------|-----------|------------|
| `start`, `center`, `end` | Pack children | Align children |
| `equal-spacing` | Space-evenly (gaps + outer margins) | **compile error** |
| `space-between` | Space-between (flush ends) | **compile error** |

Leaf text controls also use `halign` / `valign` for **glyph placement** inside the content box:

| Kind | Default text align |
|------|-------------------|
| `button`, `keybind`, `select` | center / center |
| `label`, `input` | start / center |

`<defaults>` do **not** cascade `halign` / `valign` onto nodes. Set packing on containers; override text alignment on leaves when needed.

### Inherited text style (parent → child)

During template expansion, these properties cascade from ancestors to descendants (unless overridden on a child or via `<use>` instance attrs):

`font`, `font-size`, `font-weight`, `color`, `line-height`, `text-skew`, `letter-spacing`, `text-wrap`, `drop-shadow`

Resolution order (lowest → highest priority): built-in defaults → document `<defaults>` → inherited text style from ancestors → template / `<use>` props → explicit node attributes.

`drop-shadow` on a parent `<container>` is inherited by descendant `<label>` / `<button>` nodes; containers still do not paint drop-shadow themselves. Set `drop-shadow="false"` on a child to override an inherited `true`.

`fontDraw` Y is the top of the typographic box (baseline = Y + ascent). Vertical `center` uses cap-optical placement (`midY - 0.62 * ascent`).

Optional `text-skew` (CSS `skewX` degrees) shears glyphs. Ignored when the leaf also has a non-zero `rotation`. `shape="skew-tab"` nudges upright text onto the parallelogram mid-line.

### Lengths and scale

- **`px`** — design-space length at a **1920×1080** reference (not necessarily one framebuffer pixel).
- Resolved size: `authoredPx × refScale × ui_scale`.
  - **`refScale`:** `min(logicalW / 1920, logicalH / 1080)` via `UIR_RefPxScale` (uniform contain; `1.0` if W/H invalid). Same panel at 1080 vs 4K keeps the same screen fraction for `px` chrome.
  - **`ui_scale`:** archived user multiplier (`ui_scale` cvar, default `1.0`, settings slider ~0.25–2 step 0.05). Product clamped ~0.25–8.
- **`%` on a root child** — logical canvas (SDL logical size).
- **`%` on a nested child** — parent content box after padding and child margins.
- **Overlap percentage size + margin** — `width`/`height` percentages resolve against the full parent content box; percentage margins then position the child within that box. This permits direct conversion of virtual-resolution rectangles (`x/W`, `y/H`, `w/W`, `h/H`) without shrinking `w`/`h` by the offset.
- **`auto`** — intrinsic border size: **children + padding** for containers (+ stroke when `stroke-layout` is not `false`); **text metrics** for labels/controls; **shape viewBox** for shape instances; **texel reference size** for leaf `<image>` (via backend `imageMeasure`). Not multiplied by `ui_scale` as a unit keyword — measured values are already in layout px (image/shape apply `UID_ScaleAuthoredPx` where appropriate). One axis `auto` + the other fixed (not `fill`) on `<image>` / intrinsic shapes preserves aspect ratio.
- **`fill`** — flex grow: occupy **remaining parent space** on that axis after fixed/auto siblings are measured; split equally among `fill` siblings. Not multiplied by `ui_scale`. Distinct from the **`fill="…"` paint attribute** (background color).
- **`max-width` / `max-height`** — definite clamps (`px` or `%` only; `auto`/`fill` rejected at compile). Applied after authored / intrinsic / `fill` resolve. Percent base matches `width`/`height` (canvas for root children, parent content box nested). When a flex parent’s main budget is tighter than the sum of children (e.g. a `height="auto" max-height="80%"` panel), children with `overflow="scroll"` or `overflow="hidden"` and main size `auto`/`fill` shrink so nested scroll viewports get a reduced box; non-scroll siblings keep their intrinsic size. Prefer `height="fill"` over `%` height on dividers inside `height="auto"` intrinsic chains — `%` during measure can resolve against the canvas and stretch fullscreen.
- **Fill inside `auto` wrapper** — an `auto` flex item participates in the parent fill split only when its **flex main axis matches the parent’s** and it has a direct `fill` child on that same axis (e.g. horizontal row inside a horizontal row with `width="fill"` children → wrapper acts like `width="fill"`). A horizontal `height="auto"` row with `width="fill"` children does **not** become `height="fill"` in a vertical parent — authored `width` / `height` stay independent.
- **Foreach item wrap defaults** — when `<foreach>` does not set cross-axis size, the per-item wrap uses `auto` (vertical foreach → `width="auto"`; horizontal foreach → `height="auto"`). `<foreach>` may also set container layout attrs (`type`, `width`, `height`, `gap`, `padding`, `margin`, `halign`, `valign`) copied onto each item wrap. If every template child uses `fill` on the foreach main axis, the wrap gets `fill` on that axis.
- **`text-wrap`** — `none` (default) or `word` for multiline labels (`\n` always breaks lines).
- **`line-height`** — multiplier on font size (default `1.4`) or explicit `px` length.
- **`drop-shadow`** — `true` / `false` (default `false`). Affects `<label>` and `<button>` paint only; may be set on a parent container to inherit onto text children. Paint-only: five fixed black offset passes (±1px cardinals at 50% opacity, +1.5,+1.5 at 33%) behind the main glyph, then the normal text pass. Offsets are authored px scaled via `UID_ScaleAuthoredPx` (same as `stroke-width`). Does not affect layout. While painting, the clip is temporarily expanded by 2 authored px (DIP-scaled) so the halo is not cut off by the node’s own clip stack entry; viewport edges and `overflow="hidden"` / scroll ancestors still clip. Classic HUD enables this via `<defaults drop-shadow="true"/>` in `hud_classic.xml`.

### Fill paint

- **Solid** — `fill="#RRGGBB"` / `#RRGGBBAA`, `cvar-rgba:…`, or a style ternary that resolves to those.
- **Gradient (atlas)** — `fill` may be a brush baked at paint time into a UI atlas texture (same `CreateUIAtlas` path as fonts), then stretch-drawn and shape-clipped like `background-image`. No on-disk PNG.

```xml
fill="linear(180deg, #00000000, #000000B3)"
fill="linear(90deg, #1A6FD4FF 0%, #0A3A7AFF 60%, #00000000 100%)"
fill="radial(50% 50%, #FFFFFFFF 0%, #FFFFFF00 70%)"
fill="{item.selected ? linear(180deg, #1A6FD4FF, #0A3A7A00) : #00000000}"
```

- **`linear(angleDeg, color [offset%], …)`** — CSS-like angle (`0deg` = to top, `90deg` = to right). Up to 8 stops; omitted offsets are spaced evenly.
- **`radial([cx% cy%,] color [offset%], …)`** — optional center (default `50% 50%`); radius fits the box as an ellipse. Up to 8 stops.
- Also accepted on `hoverfill` / `hover-fill` (alias) / `pressed-fill` / `focus-fill` / `disabled-fill` / `selected-fill`. Text color variants: `hover-color`, `pressed-color`, `focus-color`, `disabled-color`.
- Stroke still uses solid colors (including `cvar-rgba:…`). When fill is a gradient, shaped widgets draw the gradient clipped to the shape and keep path fill transparent.

### Stroke

`stroke` + `stroke-width` (authored `px`, default `1px` when `stroke` is set) live on the **using element**, not on shape `<path>` defs.

- **`stroke-layout`** — `true` / `false` (**default `true`**). When `true`, authored `width`/`height` / auto intrinsic size include stroke (content box inset like padding; fill geom inset at paint so the stroke ring sits inside `borderBox`). When `false`, stroke is paint-only for layout: auto size is children + padding only; content box is not stroke-inset; paint still draws the stroke ring inside `borderBox`.
- Rectangle strokes → solid edge quads.
- Path strokes → round caps/joins; paint order **stroke then fill** when both set.
- `stroke-width` without `stroke` → compile error.
- `stroke-width="{cvar:cg_crosshair_outlinethickness}px"` resolves at paint time.

Legacy `border*` attributes are **compile errors**. Use 1px child `container` dividers.

### Shapes and templates

- **Shapes** — SVG `<path>` under `<definitions><shapes>`; geometry is data, not C++ hardcoding. Optional shape-def `fit` (`contain` / `stretch`). Instance props may include `skewl` / `skewr` on `skew-rect` / `skew-tab`. Child element `<shape shape="id" …/>` embeds a shape instance as a leaf under a layout parent.
- **Intrinsic vs owner-sized** — shapes with an authored viewBox (`width`/`height` on the `<shape>` def) resolve `px` props in viewBox units; reference scale comes from layout box size + view→dest stretch. Owner-sized shapes (no intrinsic size) resolve `px` props with `uiPxScale` at paint time.
- **Child clip** — a non-default `shape` (e.g. `skew-rect`) does **not** change layout boxes, but **descendants are paint-clipped** to the owner’s shape path (stencil when available; axis-aligned dest scissor fallback). Default `rectangle` and `edge-clip` do not enable this. Nested shaped parents skip an inner clip while an outer clip is active. `edge-clip` is for edge-positioned rects (overlap `left`/`top`/`right`/`bottom`); it does not child-clip.
- **`mask-image`** — soft alpha coverage for the **whole paint subtree** (background, content, and children). Accepts an image registry id, a VFS path, or a **`linear(...)` / `radial(...)` gradient brush** (same syntax as `fill`). Optional `mask-fit` (`stretch` default, `contain`, `cover`; not `repeat`; gradients always stretch). Uses a UI-only layer RT (`r_uiFramebuffer`) with dest-in multiply so gradients / translucent fills / images feather correctly. Nesting depth 1 (inner `mask-image` is skipped). Does not change layout or hit-test. Scrollbar chrome paints outside the mask. Requires GL1 UI FBO; paints unmasked when unavailable.
- **`crisp="true"`** — binary path coverage (no soft AA); dest snapped to the FB pixel grid. Used by crosshair shapes.
- **`shape-rotation`** — paint-only degrees around geometry center; does not affect layout/hit-test. Distinct from node `rotation` (applies to the whole painted subtree).
- **Templates** — reusable subtrees with `<props>`; instantiated via `<use template="id" .../>`. Deferred forms `template="{item.field…}"` resolve per collection expand.

### Default rectangle

Builtin `rectangle` shape when not authored. Elements without `shape` (or `shape="rectangle"`) fill the content box. Optional `radius` on the element rounds corners.

---

## Definitions, imports, stock libraries

`<definitions>` is non-rendering: defaults, fonts, images, shapes, templates, modals, sources, design vars, and `<import>`.

`<canvas>` is the render root. Optional `pointer="{bool expr}"` (or `true`/`false`) controls whether an open menu owns the OS cursor / pointer input while active. Empty/omitted means no cursor ownership (typical HUD overlays). Menus normally place one root `<container>` under canvas.

Example (scoreboard): cursor for spectators, intermission, or sticky in-play TAB:

```xml
<canvas pointer="{cvar.ui_om_intermission == 1 or cvar.ui_om_spectator == 1 or cvar.ui_om_scoreboard_cursor == 1}">
```

Host mirrors those flags into temp cvars each frame (`ui_om_intermission`, `ui_om_spectator`, `ui_om_scoreboard_cursor`).

### Import rules

| Rule | Behavior |
|------|----------|
| Placement | `<import>` only under `<definitions>` or `<ui-library>`. |
| `src` | VFS path; `ui/...` from game root; otherwise relative to importing file's directory. |
| Security | `..` and `\` in import paths → compile error. |
| Merge | Depth-first; **later id wins** (warning on override). |
| `<defaults>` | Per-key merge across imports. |
| Limits | `maxImportDepth` (16), `maxImportFiles` (64), `maxXmlBytes`. |
| I/O | `<import>` requires file reader (`UID_LoadFile`); in-memory parse without I/O fails on import. |

### Design variables (`<vars>`)

Static design tokens (spacing, font sizes, colors, layout lengths) live in an importable `<vars>` block. They are **not** runtime cvars, but resolution timing depends on how they are referenced:

- Whole-value embeds `{var.id}` / `{var:id}` are substituted at compile via `UID_ResolveDocumentVars`.
- `var.*` terms inside numeric `{expr}` blocks (e.g. `width="{var.row-height * 2}px"`) may resolve at runtime via `NumericLookupPath`.
- Style-ternary branches may also resolve `var.*` at eval time.

```xml
<ui-library version="1">
  <vars>
    <var id="text-body" value="14px"/>
    <var id="text-control" value="16px"/>
    <var id="color-primary" value="#EBF0F5E6"/>
  </vars>
</ui-library>
```

Reference in property values:

- Whole value: `{var.text-body}` or `{var:text-body}`
- Numeric expression: `width="{var.row-height * 2}px"` (`var.*` terms in `{expr}` blocks)

Import merge: later `<var id="…">` wins (warning on override). Unknown whole-value `{var.id}` → compile error.

Stock files: `ui/modern/lib/menu_tokens.xml` (imported by `theme.xml` for menus/scoreboard), `ui/modern/lib/pause_tokens.xml` (classic pause menu), `ui/modern/lib/lab_tokens.xml` (layout lab examples), `ui/modern/lib/hud_classic_tokens.xml` (classic HUD via `hud_classic_theme.xml`), and `ui/modern/lib/hud_modern_tokens.xml` (modern HUD pack).

`main.xml` import order (representative):

```xml
<import src="ui/modern/lib/theme.xml"/>
<import src="ui/modern/lib/shapes.xml"/>
<import src="ui/modern/lib/crosshairs.xml"/>
<import src="ui/modern/lib/sources.xml"/>
<import src="ui/modern/lib/controls.xml"/>
<import src="ui/modern/lib/modals.xml"/>
<import src="ui/modern/lib/images.xml"/>
<import src="ui/modern/lib/browser.xml"/>
```

| Library file | Contents |
|--------------|----------|
| `theme.xml` | `<defaults>`, `<fonts>` (Oswald body/control); imports `menu_tokens.xml` |
| `menu_tokens.xml` | Menu/UI `<vars>` (type scale, colors, surfaces) |
| `pause_tokens.xml` | Classic pause menu `<vars>` (imported by `menus/dm_pause.xml`) |
| `lab_tokens.xml` | Layout lab example `<vars>` |
| `hud_classic_theme.xml` | Classic fonts/defaults; imports `hud_classic_tokens.xml` |
| `hud_classic_tokens.xml` | Classic HUD `<vars>` (no `menu_tokens`) |
| `hud_classic.xml` | Classic HUD templates (compass, objectives, ammo chrome, …) |
| `hud_ammo.xml` | Classic ammo strip templates |
| `hud_images.xml` | Classic HUD bitmap registry |
| `hud_modern_tokens.xml` | Modern HUD color/surface `<vars>` (menus import `theme.xml` separately for `menu_tokens`) |
| `hud_modern.xml` | Modern weapons bar, compass, weapon-name sources |
| `hud_modern_grenade.xml` | Modern grenade inventory template |
| `hud_modern_images.xml` | Modern weapon + headshot/glove/skull PNG registry |
| `hud_messaging.xml` | Shared kill-feed / game-msg / chat panels + killfeed icon sources (imports `hud_modern_images.xml`) |
| `shapes.xml` | Shared paths (`skew-rect`, `allied-star`, `favorite-icon`, …) |
| `crosshairs.xml` | Crosshair shapes + `crosshair-display` template |
| `sources.xml` | Static `<source>` catalogs (settings, models, menu-map-views, …) |
| `controls.xml` | Settings rows, scrollbars, route tabs, crosshair panel |
| `modals.xml` | Modal defs (`exit_confirm`, keybind overwrite, …) |
| `images.xml` | Bitmap registry (`background-image`) |
| `browser.xml` | `server-browser` template + `browser-sort-header` |
| `scoreboard.xml` | Scoreboard templates (headers, rows, team banners) |
| `pause_images.xml` | Retail pause art registry (classic `dm_pause`) |
| `pause_modern.xml` | Modern pause panel templates (`dm_pause_modern`) |

Examples: `assets/main/ui/modern/examples/layout_lab.xml`, `layout_lab_nested.xml`, `settings.xml`.

---

## Lexical contexts (one-way)

| Context | Meaning |
|---------|---------|
| `{parent.*}` | Template-use or shape-owner property bag |
| `{var.*}` | Static design token from `<vars>` (expand-time / numeric expr) |
| `{template.*}` | Props passed to current `<use>`. Bare `{template.key}` and `template.key` idents inside larger `{…}` exprs are baked to literals at expand (so mixed `template.*` + `cvar.*` runtime exprs work). |
| `{shape.*}` | Shape-instance props (`radius`, crosshair `size`, …) |
| `{item.*}` | Foreach/collection tokens (`label`, `value`, `display`, `index`, `field.NAME`, `lifetime_alpha`, `selected`, `last`, `count`) |

**Numeric expressions** (`UID_EvalNumber`): literals, references, `+ - * / %`, parentheses, whitelisted calls `abs(x)`, `floor(x)`, `min(a, b)`, `max(a, b)`, `clamp(x, lo, hi)` (if `lo > hi`, bounds are swapped). Unknown function names are errors. No assignment from expressions.

**Bool expressions** (visibility / enabled / style-ternary conditions): same arithmetic paths plus comparisons; whitelisted string helpers `icontains(hay, needle)` and `contains(hay, needle)` (case-insensitive / case-sensitive). Operators: `and`, `or`, `!`, `==`, `!=`, `<`, `<=`, `>`, `>=` (`and` binds tighter than `or`). Prefer parentheses when mixing `and`/`or` with function calls — e.g. `(gate and search == '') or icontains(name, q)` — so grouping matches intent.

**Runtime numeric props** — on layout/paint attributes (`width`, `height`, `top`, `right`, `bottom`, `left`, `opacity`, `rotation`, `translate-x`, `translate-y`, …):

| Form | Behavior |
|------|----------|
| `{cvar.NAME}` or `{cvar:NAME}` | Exact cvar string passthrough (labels, `background-image` paths) |
| `{expr}` or `{expr}px` | Evaluated each sync via `UID_EvalNumber` with `cvar.*`, `item.index`, `item.count`, numeric `item.field.*` |

**Label text** may also embed numeric `{expr}` forms (e.g. `{floor(cvar.ui_om_hud_time_seconds / 60)}:`) and the string aggregate `{join(...)}` (below); they resolve each binding sync into `runtimeValue`. Exact `{cvar.NAME}` in label text remains string passthrough.

**`join(source, field, "sep"[, boolFilter])`** — string aggregate over a collection source (host `queryCollectionItems` or XML `<sources>`). Used in label text braces only (not in numeric `UID_EvalNumber`):

| Form | Meaning |
|------|---------|
| `{join(scoreboard, name, ", ")}` | Join every non-empty `name` field |
| `{join(scoreboard, name, ", ", item.field.is_spectator == 1)}` | Same, keep rows where the bool filter is true |

- Args 1–2: bare source / field ids
- Arg 3: quoted separator (`"` or `'`)
- Arg 4 (optional): bool expr with per-row `item.field.*` / `item.label` / `item.value`
- Empty field values are skipped; output is capped (~2 KB)
- Unknown source at runtime → empty string (not a compile error)
- May mix with numeric embeds: `n={floor(3.9)} {join(...)}`

**Marquee labels** — paint-time auto-scroll when text overflows the content box (clip via parent `overflow="hidden"`):

| Attr | Values |
|------|--------|
| `marquee` | `none` (default), `horizontal` (`x` alias), `vertical` (`y` alias) |
| `marquee-speed` | px/sec length, e.g. `40px` |
| `marquee-gap` | gap between loop copies, e.g. `48px` |
| `marquee-delay` | optional start delay (`500ms` / `1s`) |

No-op when text fits, when speed is 0, or when `text-wrap="word"`. Driven by `doc->updateTimeMs` (no layout dirty each frame).

At **template expand**, any `template.*` / resolvable `parent.*` idents inside those `{expr}` forms (and inside brace-stripped style/bool ternary bodies) are replaced with numeric literals so the leftover runtime expr only needs `cvar.*` / `item.*`.

`background-image` accepts exact cvar substitution only; use `{item.field.texture}` substituted at foreach expand for per-weapon paths.

---

## Bindings and actions

Canonical cvar bind:

```xml
bind="cvar:name"
```

Compatibility (warning): `context="cvar(name)"`, `value="cvar(name)"` → `bind="cvar:name"`.

### Typed actions (`<on event="...">`)

Children: `<set>`, `<set-cvar>`, `<invoke>`, `<show-modal>`, `<hide-modal>`.

Events: `click`, `dblclick`, `change`, `submit`, `cancel`, `focus`, `blur`.

**Modals** — defined under `<definitions><modals>`, shown via cvar `ui_om_modal` (override with `modal-cvar` on keybind/select) or `<show-modal id="…"/>`. Keybind overwrite uses `modal-commit-keybind` after populating `ui_modal_*` cvars. Modal buttons may set `modal-role="confirm|cancel"`.

**Relative modals** (`type="relative"`): opener-anchored popovers (dropdowns, menus). Require exactly one descendant with `role="relative-panel"`. Engine records the opener (`<show-modal>` source node, or `<select modal="…">`) and places the panel with flip/clamp/`PlaceOverlayInViewport` (soft max height; use `overflow="scroll"` on the panel). **Fullscreen dialogs omit `type`** — there is no `type="fullscreen"`. Usable from buttons via `<on event="click"><show-modal id="…"/></on>` or from `<select modal="id">`.

Compatibility shorthand `action="UpdateProp(id, prop, value)"` on buttons compiles to `<set>` on click.

### Registered host invokes (`CL_UIR_RegisterInvokes`)

XML `<invoke name="…"/>` maps to **registered C++ handlers only** — never arbitrary console strings.

| Invoke | Purpose |
|--------|---------|
| `apply-video`, `restart-video` | Commit bindings + `vid_restart` |
| `quit` | `quit` command |
| `refresh-servers` | `CL_ModernBrowser_Refresh` |
| `sort-servers` | Apply `ui_om_browser_sort` / `ui_om_browser_sort_asc` |
| `sort-scoreboard` | Apply `ui_om_scoreboard_sort` / `ui_om_scoreboard_sort_asc` |
| `toggle-server-favorite` | Toggle `ui_browser_favorite_target` in `ui_om_favorite_servers` |
| `join-selected` | Resolve selected server IP, deactivate modern UI, `connect` |
| `apply-profile` | Commit name + `ui_applyplayermodel` |
| `settings-defaults` | Reset draft settings cvars |
| `settings-apply` | Commit + `vid_restart` / `snd_restart` when needed |
| `modal-commit-keybind` | Commit keybind from modal cvars |
| `reset-cvar` | Reset cvar named by `ui_reset_cvar` |
| `navigate` | Set `ui_om_nav_target` + `ui_om_nav_value` (`main_panel` → `ui_om_main_panel`, `settings_tab` → `ui_om_settings_tab`) |
| `back` | `ui_om_main_panel = play` |
| `close` | Clear `ui_om_modal` |
| `legacy-pushmenu` | Push UIFAKK menu named by `ui_legacy_pushmenu_target` |
| `menu-open` / `menu-close` | Open/close menu id from `ui_menu_open_target` / `ui_menu_close_target` |
| `cbuf` | Execute console string in `ui_om_cbuf` (pause menus, weapon picks) |
| `hud-chat-submit` / `hud-chat-cancel` | In-HUD chat compose submit / Escape cancel |

---

## Navigation and screen structure (`main.xml`)

| Cvar / source | Role |
|---------------|------|
| `ui_om_main_panel` | `play` \| `settings` — bound to `menu-panels` source |
| `ui_om_settings_tab` | `input`, `video`, `audio`, `gameplay` — bound to `settings-pages` source |
| `menu-panels` | Items carry `panel="panel_play"` etc. for visibility routing |
| `screen-catalog` | Dev harness: maps values to alternate XML files (`layout_lab`, …) |

Header uses `route-tab-bar-header` template over `menu-panels`. Settings body uses per-tab `visible="{cvar.ui_om_settings_tab == …}"` panels (not dynamic template swap for page bodies).

---

## Collection sources (`lib/sources.xml`)

```xml
<source id="…" default="VALUE">
    <item value="…" label="…" optional-extra-fields="…"/>
</source>
```

**Resolution:** XML definition → host `queryCollectionItems` (`servers`, `video-modes`, …) → compile error if unknown.

Notable sources:

| Source id | Role |
|-----------|------|
| `settings-pages` | Settings tab labels + legacy template ids |
| `menu-panels` | PLAY / SETTINGS (`panel` attribute) |
| `browser-gametypes` | ALL / FFA / TDM / … filter (`ui_om_server_gametype`) |
| `menu-map-views` | Menu backdrop BSP + camera fields (see pipeline doc §E) |
| `crosshair-shape` | Settings picker for `cg_crosshair_mode` (`none` / `open` / `solid` / `dot`) |
| `crosshair-mode` / `crosshair-style` | Sibling catalogs in `sources.xml` (not wired to the gameplay panel) |
| `player-models-allies` / `axis` | Profile model picker |
| `screen-catalog` | Compare / lab screen picker |
| `hud-packs` | Host-backed HUD picker (`queryCollectionItems`; not in XML `<sources>`) |
| `display-refresh` | Host-backed unique SDL refresh rates + Default(0) for `r_displayRefresh` |
| `hud-objectives` | Host-backed classic objectives stack |
| `vote-options` | Host-backed pause vote rows |
| `pause-weapons` / `pause-vote-cast` | Pause menu static catalogs in `sources.xml` |

### Host-backed HUD message collections

| Source | Contents | Row fields |
|--------|----------|------------|
| `hud-messages` | Mixed dmbox (chat + deaths) — legacy/compat | `text`, `color`, `alpha`, `bold` |
| `hud-chat` | Chat / join-leave only (`MESSAGE_CHAT_WHITE`) | same as `hud-messages` |
| `hud-kill-feed` | Structured kills from TA `printdeathmsg` / Base death `print` | `killer`, `victim`, `weapon_class`, `killer_team`, `victim_team`, `icon_team`, `headshot`, `kill_kind`, `friendly`, `text`, `color` |
| `hud-game-messages` | Game/objective notices (gmbox) | `text`, `color`, `alpha`, `bold` |

Shared pack widgets live in `ui/modern/lib/hud_messaging.xml` (`hud-kill-feed-panel`, `hud-game-messages-panel`, `hud-chat-panel`) and are used by both classic and modern HUD packs.

`weapon_class` is the stem only (`pistol` / `rifle` / `sniper` / `smg` / `mg` / `shotgun` / `grenade` / `rocket` / `landmine` / `bash` / `crush` / `telefrag` / `unknown`).  
`killer_team` / `victim_team` / `icon_team` are `allies` / `axis` (empty when unknown for killer/victim; `icon_team` defaults to `allies`). Icon team follows killer if present, else victim.  
Map icons via `killfeed-weapon-icons-allies` / `killfeed-weapon-icons-axis` + `bind="item.field:weapon_class"` + `visible` on `icon_team`. Stock maps cover pistol→bash/glove only; classifier stems without a map row (`landmine`, `crush`, `telefrag`, `unknown`, …) paint an empty weapon image slot. Name colors use `var.color-allied` / `var.color-axis`. Headshot uses `modernhud-headshot-50` when `headshot == 1`; suicide uses `modernhud-skull-50` when `kill_kind == suicide`.  
`kill_kind`: `player` / `suicide` / `world`. `headshot` is `1` only when location text includes head/helmet/neck (requires `g_obituarylocation`); else `0`.

**Collection bind:** `bind="cvar:NAME"` (writable selection) or `bind="item.field:NAME"` (read-only; selects by enclosing foreach item field — used for nested lookups like kill-feed weapon icons).

**Time seconds:** `ui_om_hud_time_seconds` — total seconds left as a decimal string (e.g. `300`), or `""` when no active countdown. Distinct from localized `ui_om_hud_time_message`.

**Score strip (modern HUD):** top-center `Allied|timer|Axis` (team, `cg_gametype > 1`) or `self|timer|leader` (FFA, `cg_gametype == 1`).

| Cvar | Role |
|------|------|
| `ui_om_hud_allied_score` | Allied team total (round wins or team kills by mode); from scoreboard headers + live `STAT_KILLS` when on Allies |
| `ui_om_hud_axis_score` | Axis team total; same sources when on Axis |
| `ui_om_hud_score_self` | FFA: your kills (`STAT_KILLS`) |
| `ui_om_hud_score_leader` | FFA: highest kills (`STAT_HIGHEST_SCORE`) |

Team totals refresh via silent `score` requests (2s throttle shared with TAB). Classic `ui_om_hud_score_text` is unchanged.

### Host temp cvars authors commonly bind

Full sync lives in `cl_hud_host.cpp`, `cl_scoreboard_host.cpp`, `cl_messages_host.cpp`, and `cl_uirender.cpp`. Commonly bound surfaces:

| Surface | Cvars |
|---------|-------|
| Score strip | `ui_om_hud_allied_score`, `ui_om_hud_axis_score`, `ui_om_hud_score_self`, `ui_om_hud_score_leader`, `ui_om_hud_time_seconds`, `ui_om_hud_time_message` |
| Scoreboard meta | `ui_om_scoreboard_team_mode`, `ui_om_scoreboard_deaths_label`, `ui_om_scoreboard_gametype`, `ui_om_scoreboard_cursor`, `ui_om_scoreboard_sort`, `ui_om_scoreboard_sort_asc`, `ui_om_scoreboard_server_name`, `ui_om_scoreboard_gamemode`, `ui_om_scoreboard_spectator_count`, TOW/Liberation `ui_om_scoreboard_tow_*` / `ui_om_scoreboard_lib_toggle*` |
| Pause / vote | `ui_om_pause_panel` (`root` \| `team` \| `weapon` \| `vote_*`), `ui_om_cbuf`, `ui_om_vote_*` |
| Chat compose | `ui_om_hud_chat_open`, `ui_om_hud_chat_label`, `ui_om_hud_chat_text`, `ui_om_hud_chat_mode` |
| Modern weapons | `ui_om_hud_weap_*_state`, `ui_om_hud_item{N}_image` / `_state` / `_name`, `ui_om_hud_primary_name`, `ui_om_hud_sidearm_name`, `ui_om_hud_grenade_count`, `ui_om_hud_active_weapon` |
| Classic compass / obj | `ui_om_hud_compass_angle`, `ui_om_hud_compass_heading`, `ui_om_hud_obj_*`, `ui_om_hud_objectives_visible` |
| Spectate / notices | `ui_om_hud_following_name`, `ui_om_hud_following_team`, `ui_om_hud_centerprint`, `ui_om_hud_show` |

### Composable collections (`source` + `<foreach>`)

| Primitive | Purpose |
|-----------|---------|
| `source="…"` on `<container>` | Collection scope; optional `bind`, `index`, `wrap`, `scroll`, `collection-display`, `value-type`, `default-index` |
| `<foreach mode="all\|selected\|window">` | Expand template per item |
| `<foreach count="{...}">` | Source-independent numeric range `0..count-1`; exposes `item.index`, `item.count`, `item.last`, `item.value` |
| `<foreach lifetime="…" fade-duration="…">` | **Opt-in** row age: keep a host key in the expanded list for `lifetime`, fade opacity over the last `fade-duration` (default `0` → hard remove). Omit both attrs → current immediate expand (menus / browser / scoreboard unchanged). `fade-duration` without `lifetime` is a parse error. Durations: `N`, `Ns`, `Nms`. Automatic wrap opacity; optional `{item.lifetime_alpha}`. Shared messaging panels (`hud_messaging.xml`): kill-feed `6s`/`1.5s`; chat and game messages `8s`/`2s`. |
| `step-index` on `<button>` | Step selection (wrap when `wrap="true"`) |
| `set-index="{item.index}"` on **interactive** nodes (`<button>`) | Select row + write scope `bind` |
| `default-index="N"` on collection scope | Fallback row index when bind/cvar is empty/unmatched and source `default=` does not resolve; omit → no selection (`-1`) |
| `index="…"` on collection scope | Separate writable index bind (alongside or instead of value `bind`) |
| `collection-display="value"` | Exposes `{item.display}` from the item value |
| `visible="{item.selected}"` / `visible="{!item.last}"` | Per-row visibility in `mode="all"` |
| `fill="{item.selected ? #A : #B}"` (any property) | State-dependent style without duplicate nodes; `visible`/`enabled` stay bool exprs |

**Selection resolution** (when bind/cvar empty or unmatched): (1) match bound value → item index; (2) source `default="VALUE"`; (3) scope `default-index="N"`; (4) otherwise no selection (`item.selected` false for all rows; bind not written).

**Server browser (no default row):** `browser_list` uses `source="servers"` with **no** `default-index` — list stays unselected until the user clicks a row. Double-click row body joins via `<on event="dblclick"><invoke name="join-selected"/></on>`.

**Tab bar recipe:** one button per item with style ternaries (do **not** duplicate selected/unselected buttons):

```xml
<button set-index="{item.index}"
        fill="{item.selected ? #1A6FD4FF : #00000000}"
        hoverfill="{item.selected ? #2A7FE4FF : #FFFFFF0F}"
        color="{item.selected ? #FFFFFFFF : #EBF0F5B8}">{item.label}</button>
<container ... visible="{!item.last}"/>
```

**Server browser (`browser.xml`):**

- Template `server-browser` — gametype tabs, sortable header row, windowed list, search, JOIN.
- List scope: `source="servers"` `bind="cvar:ui_selected_server"`.
- Rows: favorite `<button>` + body `<button set-index="{item.index}">` wrapping column labels (containers are not hit-tested as interactive; use buttons for reliable selection).
- Headers: `browser-sort-header` sets `ui_om_browser_sort` and calls `sort-servers`.
- JOIN calls `join-selected`, which reads `browser_list` collection selection (not a stale default row), then `CL_UIR_DeactivateModernMain()` before `connect`.

Host-backed `servers` collection fields: `favorite`, `favorite_fill`, `name`, `map`, `players`, `gametype`, `ping`; `item.value` is `ip:port`.

**Scoreboard (`scoreboard.xml`):**

- Menu `scoreboard` at draw-order `3` (under HUD packs at `4` so messaging paints above; hold TAB via `+scores`).
- Transparent fullscreen root (no scrim); centered panel keeps `fill-panel`.
- Canvas `pointer="{cvar.ui_om_intermission == 1 or cvar.ui_om_spectator == 1 or cvar.ui_om_scoreboard_cursor == 1}"` — OS cursor for spectators / intermission, or sticky in-play TAB cursor.
- List scope: `source="scoreboard"` (no selection bind).
- Columns: `#` (team modes), Name, Kills, **DEATHS** (header currently hardcoded; host still publishes `ui_om_scoreboard_deaths_label` as `"Deaths"` / `"Total"` but shipped XML does not bind it yet), **K/D**, Time, Ping.
- Row fields: `kind` (`header` | `player` | `spacer`), `slot`, `name`, `kills`, `deaths`, `kd`, `time`, `ping`, `text_color`, `row_fill`, `is_header`, `is_spectator`, `is_local`, `team`.
- Meta cvars (refreshed each server `scores` update): see Host temp cvars table above.

Legacy `<server-list>` still parses (and still uses `role="server-list"` host paint); prefer the composable `source="servers"` list in `browser.xml`.

---

## Controls catalog mapping

| Catalog kind | UI pattern |
|--------------|------------|
| 3+ discrete values | `settings-cyclic` / `settings-cyclic-row` (or legacy `<select appearance="cyclic">`) |
| Exactly 2 values | `settings-on-off` / `settings-on-off-row` (not `<toggle>`) |
| Scalar numeric | `settings-slider` / `settings-slider-row` |

**`value-type` transforms** (sync/write): `percent`, `invert-mouse`, `cm360`, `display-mode`. `value-type="none"` clears a transform. May be set on a collection scope container as well as on controls.

**`set-value`** on a bound button writes that string on click. Active option styling uses `fill="{bind.selected ? #active : #idle}"` (not `pressed-fill`). Pointer `pressed-fill` / `hoverfill` remain for real mouse press/hover.

**`commit`** on bound controls: `change` | `submit` | `apply` — when the bound value is written.

**`tab-index`** — focus ordering for keyboard navigation.

### Crosshair

Settings gameplay page uses separate `settings-crosshair-panel` (`role="crosshair-preview"`) and `settings-sniper-panel` (`role="sniper-preview"`) host regions (WYSIWYG via `crosshair_draw.c` / sniper open reticle; both plates use `#808080FF`). Shape picker: `cg_crosshair_mode` (`none` / `open` / `solid` / `dot`) via source `crosshair-shape`. Dynamic spread: `cg_crosshair_dynamic`, `cg_crosshair_dynamic_movement`. Legacy shapes remain in `crosshairs.xml` for reference/tests.

Cvar-backed props in shapes (if used): `fill="cvar-rgba:cg_crosshaircolor_r,…"`, `size="{cvar:cg_crosshair_dot_size}px"`.

---

## Scrollbars

| Piece | Notes |
|-------|-------|
| `overflow="scroll"` on `<container>` | Enables scrolling |
| `scrollbar="template-id"` | Attach chrome; default injects `scrollbar-default` (must exist in definitions) |
| `scrollbar-edge="content\|border"` | Rail placement |
| `<scrollbar>` template | `<track>` + `<thumb>` (not slider parts; kinds `UID_NODE_SCROLLBAR` / `_TRACK` / `_THUMB`) |
| Thumb along-axis | Auto: `track * (viewport / content)`, clamped to ~20 authored px (scaled) … track length. Do not author `height` (vertical) / `width` (horizontal) on the thumb for length — only cross-axis thickness (`width` vertical / `height` horizontal). |
| Visibility | Chrome is hidden when content does not overflow (`maxScroll <= 0`); no full-track thumb when content fits. |

---

## Element grammar (summary)

| Element | Role |
|---------|------|
| `<container>` | Box layout, `overflow`, optional `source` collection scope, optional `role` host region |
| `<label>` | Text; `text-cvar` for dynamic copy; optional `drop-shadow`, marquee attrs |
| `<image>` | Leaf bitmap; `src` (registry id / VFS path / `{cvar}` / `{item.field}` / style ternary), optional `fit` (`contain` default), `scale`. **Intrinsic size** from texel measure: `width`/`height` `auto` use natural DIP size; one axis fixed + other `auto` preserves aspect. Distinct from decorative `background-image` on containers (paint-only, no intrinsic size). Not the same as registry `<images><image id src/>`. |
| `<button>` | Click target; supports `set-index`, `set-value`, shape styles; optional `drop-shadow` |
| `<input>` | `type="text\|number"` |
| `<toggle>` | Optional `true-value` / `false-value` |
| `<slider>` | `min`, `max`, `step`; optional `<track>`, `<range>`, `<thumb>` |
| `<select>` | `source` or `<option>` children; `appearance="dropdown\|cyclic"`; `modal="id"` opens a definitions modal (typically `type="relative"`); optional `modal-cvar` |
| `<keybind>` | `binding`, `slot`, `confirm-modal`, `empty-label`, `capture-label`; optional `modal-cvar` |
| `<model>` | `model`, `bind`, `team`, `anim`, `anim-variant`, `anim-phase`, `angles`, `fov`, `scale`, `offset`, `bbox`, `bbox-from-model`, `framing-scale`, `color` → queues model preview in compositor |
| `<shape>` | Shape instance leaf (`shape="id"`) under a layout parent |
| `<use>` | Template instance (static id or deferred `{item.field…}`) |
| `<foreach>` | Collection expansion |
| `<on>` | Typed actions |
| `<server-list>` | Deprecated imperative browser; prefer composable `source="servers"` |

**Visibility / enabled:** `visible="{bool expr}"`, `enabled="{bool expr}"`. Legacy `visible-if` / `enabled-if` / `visible-if-index` migrate at parse into the same evaluator.

**Bool expressions:** `cvar.NAME`, `item.value`, `item.label`, `item.index`, `item.selected`, `item.last`, `item.count`, `item.field.NAME`, `bind.selected`, `bind.value`; operators `and`, `or`, `!`, `==`, `!=`, `<`, `<=`, `>`, `>=` (`and` binds tighter than `or`); functions `icontains` / `contains`. Unquoted RHS words are string literals. Prefer word ops in XML attributes so you avoid `&amp;&amp;`. Compare operands may be numeric expressions (`+ - * / %`, parentheses, and the same paths), e.g. `visible="{cvar.ui_om_hud_health > item.index * 10}"`. Non-numeric string compares still use the atomic path (`cvar.name == StG 44`). Bool exprs do **not** accept `? :` ternaries — those are style-only. Parenthesize mixed `and`/`or`/calls.

**Style ternaries** on `fill`, `color`, `hoverfill`, `height` (then/else may nest another ternary; branches resolve `var.*` or literals):

```xml
fill="{item.selected ? #1A6FD4FF : #00000000}"
height="{item.selected ? 5px : 3px}"
fill="{bind.selected ? #1A6FD4FF : #00000073}"
fill="{cvar.ui_om_hud_health > item.index * 10 and cvar.ui_om_hud_health < 40 ? var.color-axis : cvar.ui_om_hud_health > item.index * 10 ? var.color-primary : var.fill-panel}"
```

Deprecated: `visible-if-index`, `visible-if`, `enabled-if`, `selected-fill` (still validates/paints; prefer `{bind.selected ? … : …}` / `{item.selected ? …}`), and using `pressed-fill` for active bind state.

**Common box/style attrs:** `id`, `width`, `height`, `padding`, `margin`, `gap`, `fill`, `color`, `shape`, `radius`, `rotation`, `rotation-origin`, `opacity`, `translate-x`, `translate-y`, `left`, `top`, `right`, `bottom`, `background-image`, `background-fit` (`stretch` / `repeat` / `contain` / `cover`), `mask-image`, `mask-fit`, `font`, `font-size`, `hoverfill` / `hover-fill`, `pressed-fill` (pointer), `focus-fill`, `disabled-fill`, `hover-color`, `pressed-color`, `focus-color`, `disabled-color`, `tab-index`, `commit`, …

Unknown elements/attributes are errors unless declared in template/shape `<props>`.

---

## Canonical minimal sample

```xml
<ui version="1">
    <definitions>
        <import src="ui/modern/lib/theme.xml"/>
        <import src="ui/modern/lib/shapes.xml"/>
        <import src="ui/modern/lib/controls.xml"/>
    </definitions>
    <canvas>
        <container id="root" type="vertical" width="100%" height="100%">
            <use template="settings-slider-row" label="FOV" bind="cvar:cg_fov"
                 min="60" max="120" step="1" width="400px" height="40px"/>
            <container id="browser_list" type="vertical" width="100%" height="200px"
                       source="servers" bind="cvar:ui_selected_server">
                <foreach mode="window" row-height="36px">
                    <button width="100%" height="36px" set-index="{item.index}">
                        {item.label}
                    </button>
                </foreach>
            </container>
            <button width="auto" height="auto" padding="8px 16px">
                JOIN
                <on event="click"><invoke name="join-selected"/></on>
            </button>
        </container>
    </canvas>
</ui>
```

Additional shapes (logos, icons, crosshairs) are ordinary `<shape>` definitions — do not add brand geometry in C++.
