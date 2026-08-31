/*
===========================================================================
Copyright (C) 2026 Project: Omaha

Lifecycle validation notes for the modern disconnected-menu renderer
(Project: Omaha; builds on OpenMoHAA foundations).

Manual / CI checklist (code paths covered by this implementation):

1. Startup (default modern)
   - CL_UIR_RegisterCvars() runs in CL_Init before hunk users.
   - ui_legacy CVAR_INIT caches mode; default 0 = modern.
   - CL_UIR_Init() runs from CL_InitializeUI after legacy UI starts.

2. Intro / legal
   - CL_UIR_ShouldRenderModernDisconnected requires CL_FinishedIntro().
   - Intro still uses UI_DrawIntro / UI_ClearBackground (legacy).

3. Disconnect auto-main
   - CL_Frame activates CL_UIR_ActivateModernMain instead of UI_MenuEscape("main")
     when modern; legacy path unchanged with +set ui_legacy 1.

4. Connect / loading
   - Non-disconnected states call CL_UIR_DeactivateModernMain().
   - server_loading / connecting still use UI_ClearBackground (legacy).

5. Missing menu BSP
   - UIR_MenuWorldEnsureLoaded marks UNAVAILABLE and draws solid black fallback once.

5b. Menu map view switching
   - Catalog in sources.xml (`menu-map-views`); cvar `ui_om_menu_map_view`.
   - Same-BSP switches update camera only; different BSP uses staged load + commit.

6. vid_restart / registration
   - CL_UIR_OnRendererRegistration + UIR_FontInvalidateGpu + MenuWorldMarkNeedsReload.
   - Cached ui_legacy selection is retained (CVAR_INIT).

7. Shutdown
   - CL_UIR_Shutdown from CL_ShutdownUI tears down fonts/menuworld/compositor.

8. Renderer modules
   - Default play path: `cl_renderer opengl1` (locked unless `+set cl_renderer` on the
     command line; not archived to config).
   - REF_API_VERSION 17 (ExportModelPreviewPNG slot; GL1 stub, GL2 export when built);
     CreateUIAtlas/UpdateUIAtlas/ClearWorld/LoadMenuWorld/LoadMenuWorldStaged on
     both renderergl1 and renderergl2 (dlopen or static).
   - Weapon PNG bake (`ui_bake_*`) must launch with `+set cl_renderer opengl2`; normal menu/HUD
     play stays on GL1.

9. Legacy parity
   - +set ui_legacy 1 keeps pushmenu main / MenuEscape / ClearBackground behavior.

10. Packaging
   - POST_BUILD copies assets/main/fonts and assets/main/ui/modern next to the
     client binary under main/.
   - install(DIRECTORY ...) ships the same trees to ${INSTALL_BINDIR_FULL}/main/...
     (same relative layout as POST_BUILD; not under INSTALL_DATADIR_FULL).

11. Fluid layout (modern design)
   - Root geometry uses % / fill / flex against the live logical SDL canvas at any
     resolution or aspect. There is no fixed 1280×720 artboard and no runtime
     letterbox/pillarbox. Compare shots may still capture at a fixed size (see
     artifacts/modern-menu-compare/README.md); that size is harness-only.

12. Screenshot compare helpers
   - Console: ui_compare_goto <play|settings> [input|video|audio|gameplay]
     sets visibility on panel_play / panel_settings / settings_page_* for capture loops.
   - Host invokes used by design: refresh-servers, join-selected, apply-profile,
     settings-defaults, settings-apply, apply-video, restart-video, quit.
   - Option sources: video-modes, player-models-allies, player-models-axis.
   - Tools: tools/ui_compare.py (mask + % fail gate), tools/ui_capture_html.py
     (Chromium/Playwright/placeholder HTML reference capture).

Automated coverage:
  - cmake/tests/uirender.cmake — viewport, FOV, path AA, SVG parse (incl. trailing
    junk), fill-rule smoke (even-odd vs nonzero), font UV math, model-preview
    origin/FOV helpers. Scissor Y conversion is static in uir_compositor.c and
    is not unit-tested.
  - cmake/tests/uir_design.cmake — XML parse/expand/layout/bindings/actions,
    DOCTYPE reject, ROM write refuse, template {template.*} interpolation,
    NaN length reject, settings.xml fixture via UID_TEST_FIXTURE_DIR.
===========================================================================
*/
