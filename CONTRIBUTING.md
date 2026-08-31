# Contributing to Project: Omaha

All contributions (issues, discussions, code comments, commit messages, documentation) must be written in **English**.

## Scope of this fork

Project: Omaha is an **independent** client-focused fork. It is not the official OpenMoHAA project and will not be contributed upstream as a matter of course.

- Prefer **stock-server-compatible client** changes (`code/client/`, `code/cgame/`, UI, renderer). Do not change `code/server/` or `code/fgame/` unless the task explicitly overrides that rule.
- Keep retail MOHAA net/assets/scripts/SP compatibility unless the change intentionally alters them.

## Generative AI

**Allowed in this repository.** AI-authored and AI-assisted code, docs, commits, and refactors are permitted.

Upstream OpenMoHAA’s “no generative AI” policy applies to **their** repository only. Do **not** submit Omaha AI-assisted work to official OpenMoHAA as if it met their policy.

## Forking / licensing (hard requirements)

This tree is a **GPL-2+** derivative. See [`COPYING.txt`](COPYING.txt).

| Requirement | Rule |
| --- | --- |
| Notices | Keep copyright and license notices intact on inherited files (Id, OpenMoHAA, ioquake3, third-party). |
| Same license | New and modified Omaha code stays GPL-2+ (or later, matching in-tree wording). No proprietary relicensing. |
| Change notice | On inherited files you edit, prefer `// Added\|Changed\|Fixed\|Removed in OPM` (or equivalent). Do not replace upstream copyright lines with Omaha-only credit. |
| License text | Keep `COPYING.txt` and third-party licenses with redistributed binaries. |
| Source | Binary recipients must get corresponding source (this tree) or a written offer. |
| No false endorsement | Do not claim to be official OpenMoHAA or endorsed by the OpenMoHAA team / EA. |
| Third-party | Do not rebrand or rewrite third-party LICENSE files. |

New fork-owned files use the Project: Omaha header template: [`docs/markdown/05-contributing/01-license-header.md`](docs/markdown/05-contributing/01-license-header.md).

## Coding style

Match the file you edit. Prefer existing patterns over new abstractions.

- Annotate meaningful Omaha changes: `// Added|Changed|Fixed|Removed in OPM`.
- Format touched modern C++ with clang-format; do not mass-reformat tabbed legacy files.
- English comments only; no `// @Author` tags.
- See `.cursor/skills/openmohaa/SKILL.md` for architecture and module boundaries.

Further inherited style notes (Event layout, Class patterns) remain useful in:

- `docs/markdown/04-coding/01-code/01-creating-class.md`
- Upstream-oriented sections of older docs (treat product name “OpenMoHAA” there as historical unless updated)

## Issues

When reporting bugs, include:

- Clear description and steps to reproduce
- Relevant `qconsole.log` / crash backtrace
- OS and full `version` console string (should show **Project: Omaha**)
