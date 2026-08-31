# Modern UI documentation

Human-oriented notes and shape references for the modern disconnected menu.

**Canonical LLM / implementer contracts** (XML grammar, compositor phases, invoke registry) live in **[`docs/LLM-helpers/`](../LLM-helpers/)**:

| Document | Role |
|----------|------|
| [designformat.md](../LLM-helpers/designformat.md) | Version-1 XML design format |
| [ui-rendering-pipeline.md](../LLM-helpers/ui-rendering-pipeline.md) | `uirender` compositor and draw primitives |

| Document (here) | Role |
|-------------------|------|
| [allied-logo-shape.md](allied-logo-shape.md) | Allied star as generic SVG shape data (no C++ geometry) |

Runtime lifecycle checklist: [`code/uirender/LIFECYCLE.md`](../../code/uirender/LIFECYCLE.md).

Assets: `assets/main/ui/modern/` (sync to game `main/ui/modern/` when testing outside the CMake build tree).
