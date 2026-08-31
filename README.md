# Project: Omaha

**Project: Omaha** is an independent, client-focused fork of [OpenMoHAA](https://github.com/openmoh/openmohaa) for **Medal of Honor: Allied Assault** (including Spearhead and Breakthrough). It builds on the OpenMoHAA / [ioquake3](https://github.com/ioquake/ioq3) / [F.A.K.K. SDK](https://code.idtech.space/ritual/fakk2-sdk) foundations with a modern XML-driven UI, HUD packs, and related client work.

This project is **not** affiliated with or endorsed by the OpenMoHAA team or Electronic Arts. Binary and config path names may still use `openmohaa` for compatibility with existing installs.

## License

Distributed under the **GNU General Public License version 2 or later**. See [`COPYING.txt`](COPYING.txt). Third-party components keep their own licenses under `code/thirdparty/`.

If you receive binaries, you are entitled to the corresponding source (this repository, or a written offer that accompanies the build). Keep `COPYING.txt` with redistributed packages.

## Getting started

Inherited OpenMoHAA docs still describe many install and runtime details (paths, assets, expansions):

- [Installing](docs/markdown/01-intro/01-installation.md)
- [Running](docs/markdown/02-running/01-running.md)
- [FAQ](docs/markdown/02-running/03-faq.md)
- [Building from source](docs/markdown/04-coding/01-compiling.md)

Primary targets remain `openmohaa` (client) and `omohaaded` (dedicated). This fork emphasizes **stock-server-compatible client** features; do not assume custom server/`game.so` requirements for Omaha UI work.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). AI-assisted work is allowed **in this repository**. Do not submit Omaha work to official OpenMoHAA as if it complied with their generative-AI ban.

## Modern UI contracts

- [`docs/LLM-helpers/designformat.md`](docs/LLM-helpers/designformat.md)
- [`docs/LLM-helpers/ui-rendering-pipeline.md`](docs/LLM-helpers/ui-rendering-pipeline.md)

## Foundation credit

Game preservation and the engine port that this fork starts from are the work of the OpenMoHAA project and earlier ioquake3 / F.A.K.K. contributors. Project: Omaha is a separate effort built on that GPL foundation.
