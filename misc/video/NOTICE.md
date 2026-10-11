# Video encode dependencies (H.264 demo export)

Project: Omaha optionally links a **minimal static** build of:

- **x264** (GPL-2+) — H.264 video encoder  
- **FFmpeg** (GPL-2+, built with `--enable-gpl`) — AAC audio, MP4 mux, colorspace convert  

These libraries are compiled by `misc/video/build-ffmpeg-x264.sh` and linked into the `openmohaa` client binary. End users do **not** need to install FFmpeg or x264.

License texts (copied from the pinned sources at build time) live in `misc/video/licenses/` when the build script has been run. Both projects are GPL-2+ compatible with Project: Omaha / OpenMoHAA.

Pinned versions are recorded in `misc/video/build-ffmpeg-x264.sh` (URL + SHA256).
