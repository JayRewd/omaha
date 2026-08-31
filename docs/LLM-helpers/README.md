# LLM helpers — modern UI contracts

Canonical, implementation-aligned references for agents and implementers working on the modern disconnected menu, XML design format, and `uirender` draw stack.

| Document | Role |
|----------|------|
| [designformat.md](designformat.md) | Version-1 XML design format (`uidesign` + assets) |
| [ui-rendering-pipeline.md](ui-rendering-pipeline.md) | `uirender` compositor phases and draw primitives |

Related (outside this folder):

| Document | Role |
|----------|------|
| [../modern-ui/allied-logo-shape.md](../modern-ui/allied-logo-shape.md) | Allied star as generic SVG shape data |
| [../../code/uirender/LIFECYCLE.md](../../code/uirender/LIFECYCLE.md) | Runtime lifecycle checklist (startup, connect, vid_restart) |
| [../../code/uidesign/tests/test_uir_design.cpp](../../code/uidesign/tests/test_uir_design.cpp) | Executable grammar/layout regression tests |

Asset root: `assets/main/ui/modern/` (copied to `build-*/Release/main/ui/modern/` on client build; deploy separately to the game `main/ui/modern/` tree when testing outside the build dir).

**Canonical contracts live in this folder.** Snapshots under `artifacts/UI/` (older `designformat.md` / `ui-rendering-pipeline.md`) are historical only — do not treat them as a second source of truth.
