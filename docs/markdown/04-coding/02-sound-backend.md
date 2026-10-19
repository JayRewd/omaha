# Sound backend (Project: Omaha)

## Overview

The client has one sound host. It owns MOH channel policy, music, streams,
cinematic audio and the output device. The core (`snd_dma_new.cpp`, sound info,
aliases, cgame/client `S_StartSound` API) is unchanged.

If no playback device opens, the mixer keeps running on a null device so the
game and demo capture still advance. Everything sound-specific lives in
`code/client/omaha/`.

## Architecture

| File | Role |
|------|------|
| `code/client/snd_host.h` | `sndHost_t`: the 13 `S_Driver_*` entry points, the 18 public sound API functions the core and client call outside them (music, triggered music, cinematic audio, savegame, info), and 5 capture entry points |
| `code/client/snd_host.cpp` | Forwards the public sound API and capture to the Omaha host |
| `code/client/snd_local_new.h` | `S_Driver_*` macros point at `s_sndHost` (the only stock-header change) |
| `code/client/omaha/snd_omaha.cpp` | Omaha host (`S_OMAHA_*`, `s_sndHostOmaha`): channel policy, music, streams, categories, obstruction traces, spatial routing |
| `code/client/omaha/snd_renderer.h` | Voice API (`omSoundId_t`, `omVoiceParams_t`) |
| `code/client/omaha/snd_render_omaha.cpp` | Mixer: Hermite resample, speaker pan or HRTF, EQ / HF duck / limiter, `ma_device` I/O |
| `code/client/omaha/snd_adpcm_omaha.c` | PCM and ADPCM decode |
| `code/client/omaha/snd_hrtf_omaha.cpp` | Headphone HRTF (libmysofa + PFFFT) |
| `code/client/omaha/snd_render_select.cpp` | Renderer lifetime and `S_OMAHA_Capture*` (demo export) |
| `code/client/omaha/snd_stream_async.cpp` | Background music / stream decode |
| `code/client/omaha/snd_category.c` | Mix category classifier (alias + path rules) |
| `code/client/omaha/snd_omaha_overlay.cpp` | `s_hf_duck_meter` HUD overlay, hooked once from `cl_uiview3d.cpp` |

CMake always builds `code/client/omaha/` plus miniaudio, libmysofa and PFFFT.

## Capture / demo export

`S_Capture_Begin` / `S_Capture_Frame` / `S_Capture_End` feed AVI/MP4 audio
through the mixer. Capture stops the device and pulls float, then converts to s16 for the encoder.

## Voice limits

The Omaha host removes:

- Fixed-pool voice steal
- Per-SFX `max_number_playing` cull
- A fixed voice ceiling (the mixer has no source cap)

Same entity + same `CHAN_*` still replaces (MOH semantics), within the same
voice kind (3D, 2D, 2D stream), including overflow voices.

Overflow voices pause with the game. Single-player saves keep the fixed
`soundsystemsavegame_t` layout: overflow voices fill free slots of their kind,
and any beyond that are dropped on restore.

### Overlap ducking

Without a voice cap, bursts of identical entity-less one-shots (debris from a
broken plank or window, a grenade's impacts, MG rounds into one wall) stack
linearly and get very loud. `S_OMAHA_StartSound` ducks each new 3D one-shot
by how much it overlaps what is already sounding:

```
overlap = Σ exp(-age/tau) · clamp(level(v)/level(new), 0, 1) · dirSim(v, new)
g       = (1 + overlap)^-power
g < floor  → the sound never starts
otherwise  → volume *= g   (fixed at start, never modulated)
```

- `level` = channel volume · `min(1, minDist / dist)` at the listener, so a
  quiet far voice barely ducks a loud near one.
- `dirSim` = `((1 + cos θ) / 2)^spread`, θ the 3D angle between the two
  sources seen from the listener. Raised toward 1 when both sources sit inside
  their min distance (direction stops separating them).
- Counted voices: 3D kind, `CHANNEL_FLAG_NO_ENTITY`, not looping, playing,
  same resolved category, `age < 4·tau`.
- Never ducked: anything with an entity (players, weapons, movers), 2D/local
  paths, loops, streams, `SND_CAT_AUTO` / `SND_CAT_NONE`.
- `SND_CAT_EXPLOSIONS` (and any FIRE) use pocket ducking below instead; each
  category goes through exactly one rule.
- `SND_CAT_FOOTSTEPS` / `SND_CAT_BODYFALLS` are skipped: cgame plays them
  entity-less at the ground trace, but they belong to a body, so crowds of
  players never duck each other.

Constants are compile-time (`OM_OVERLAP_*` in `omaha/snd_omaha.cpp`): on,
tau 70 ms, power 0.7, spread 2, floor 0.1. `s_show_sounds 1` prints `overlap` / `g`.

Tuning notes (from the model in `code/client/tests/test_sound_concurrency.py`):

- The weight uses each voice's *ducked* volume, so a burst self-limits softly:
  16 coincident starts sum to ≈6.3× one start (not 16×), the last at g ≈ 0.25.
  At floor 0.1 nothing in such a burst drops; floor ≈0.3 drops the tail.
- `spread 2` is a wide lobe: 45° apart still weighs ≈0.73. Clusters 45° apart
  become nearly independent around `spread 16` (≈0.08).

### Pocket ducking (fire / explosions)

Omaha host only. Several players fighting in one room
fire ~40 rounds/s between them, and each shot rings for hundreds of ms, so the
loudness is sustained density, not exact coincidence. Other players' gunfire and
all explosions are ducked at start by recent same-category voices whose
*origins* are close to the new one (the "pocket"), regardless of where the
listener stands:

```
overlap = Σ exp(-age/tau) · clamp(level(v)/level(new), 0, 1) · pocketW(|origin_v - origin_new|)
pocketW = 1 inside R/2, smoothstep down to 0 at R
g       = max((1 + overlap)^-power, minGain)   (fixed at start, never drops)
```

- **FIRE:** only when the new sound is not local (`S_OMAHA_IsLocalEntity`:
  the listener's entity or a `use_listener` child). Local voices never count,
  and voices of the same entity are excluded, so a lone shooter's full-auto
  sounds stock; only *other shooters* in the pocket duck it.
- **EXPLOSIONS:** all of them, with or without an entity, no same-entity
  exclusion (a server explosion sound and its CGM FX sound count against each
  other). Removed from the overlap rule.
- Counted voices: 3D kind, not looping, playing, same category,
  `age < 4·tau`. Origin of a new entity sound falls back to the entity's
  last known position when no origin is passed.
- Never drops: `OM_POCKET_MIN` (0.35) is a floor, so nobody goes silent.

Model (`test_sound_concurrency.py`): four interleaved full-auto shooters in one
room settle at g ≈ 0.5–0.58 each, ≈2.1× one shooter in total instead of 4×.

Constants are compile-time (`OM_POCKET_*`): on, power 0.7, min 0.35, fire R/tau
512/80 ms, explosion R/tau 512/120 ms. `s_show_sounds 1` prints `pocket` / `pg`.

## Capture / demo export

`S_Capture_Begin` / `S_Capture_Frame` / `S_Capture_End` feed AVI/MP4 audio.
Both renderers produce float; `S_Capture_Frame` is the single float → s16
conversion for the video encoder. Capture stops the device and pulls from the mixer. Surround layouts are folded to stereo for the file.

## Tests

Voice-policy, routing and layout checks:
`python3 code/client/tests/test_sound_concurrency.py`.

## Known limits

- The device callback and a capture pull both mix, so `Om_Mix` holds `s_mixMutex` for that whole mix. Game-thread setters do not take it: they enqueue commands under `s_apiMutex`, and the mixer drains the queue at the start of `Om_Mix`.
- Categories come from the global alias list (cgame resolves aliases to files
  before `S_StartSound`). A file shared by aliases of different categories is
  decided by its path; in retail scripts that affects under 0.3% of aliases,
  all between neighbouring categories (gear foley vs movement, grenade water
  bounce vs puddle step, vehicle vs world doors).

## Loop blocks

`loopstart` / `loopend` from sound info (and Miles `sample_loop_block`) are honored:

- Static voices wrap inside `[loopStart, loopEnd)`
- Streamed music: on each loop restart, decode skips to `loopBlockStart` samples

## Speaker layouts / HRTF

| `s_speaker_type` | Behavior |
|------------------|----------|
| `0` Two Speakers | Stereo pairwise pan, rear cue, three-band peaking EQ, then `s_hf_duck`, then the limiter. |
| `1` Headphones | HRTF. After binaural decode, a fixed EQ (100 Hz +3 dB, 400 Hz +1.5 dB, 4.5 kHz −2 dB, 15 kHz shelf −4.5 dB), then `s_hf_duck` when `s_mix 1`. |
| `2` Surround 5.1 | Pairwise pan to L/R/C/side. LFE stays silent. |
| `3` Quad | Pairwise pan to front and rear pairs. |

Changing `s_speaker_type` latches and triggers `snd_restart`.

### Omaha headphone HRTF

`code/client/omaha/snd_hrtf_omaha.cpp`, driven by `snd_render_omaha.cpp`.
Speaker type `0` output is unchanged.

The SOFA file named by `s_hrtf_file` (archived, sound-latched; default
`sound/prom/hrtf/SADIE_KU100_DFC_256_order_fir_48000.sofa`) is read through the
engine filesystem and resampled to the mix rate by libmysofa. If it fails to
load, omaha falls back to stereo for that session and prints why.

Two paths share the HRTF set:

- **Dry (own sounds):** the listener's own entity and `LOCAL_LISTENER` channels
  stay equal-power stereo with no HRTF and no ambisonic bus, so first-person
  weapons and foley stay present.
- **Direct (per voice):** every entity-attached 3D sound of *another* entity in
  `FIRE`, `FOLEY`, `FOOTSTEPS`, `BODYFALLS`, `VOICE` or `VEHICLES` gets its own
  minimum-phase HRIR (pinna / ILD from the magnitude) plus a separate smoothed
  ITD delay, so crossfades do not comb. At load, the same onset-aligned +
  min-phase pipeline is baked onto a dense elev/az lattice (~1.5° equatorial
  step, full FIR length and float delays). Runtime bilinear-lerps four neighbors’ partitioned spectra. A
  bake-vs-live quality gate must pass before SOFA is closed; on failure (or
  `s_hrtf_live_query 1`) DIRECT keeps live SOFA queries. There is no dry pan
  mix. The filter is re-queried once the direction moves more than ~1° and
  crossfaded over one 128-sample partition. When a voice stops, its filter rings
  out as a tail.
- **Bus:** everything else (world / entity-less sounds, debris, impacts,
  explosions, the listener's own sounds, 2D paths, stereo sources, and sources
  closer than 1 unit) is encoded into a 3rd-order 3D ambisonic bus (16 channels,
  ACN / N3D) and decoded once. The decoder is MagLS over a 50-point Lebedev set:
  complex fit (keeps ITD) below ~2 kHz, magnitude match above with average phase
  continuation (Hold et al.) so HF group delay does not disperse, plus a
  diffuse-field EQ against the direct HRIRs. MagLS is designed from SOFA at
  load only. The bus runs 64 samples (~1.45 ms) behind direct voices.

The route is decided host-side (`S_OMAHA_SpatialRoute` in `omaha/snd_omaha.cpp`)
and sent as `SND_PARAM_SPATIAL_ROUTE`. Entity-less footsteps and bodyfalls
within 48 u horizontally below the listener's head are treated as the
listener's own (dry); others get a direct HRTF voice. The route is
re-evaluated with obstruction and ignored in stereo. Obstruction, category gains, ducking and
the limiter work the same as in stereo.

Convolution is uniformly partitioned overlap-save (128-sample blocks, 256-point
FFT). In HRTF mode the mixer renders whole blocks, so the device period is a
multiple of 128 frames (~20 ms) and filtering adds no latency. With SADIE at
44.1 kHz (236 taps, 2 partitions per direct voice, 3 for the decoder), a direct
voice costs about two 256-point FFT pairs per block. Silent voices and bus
channels skip their convolution once their history has drained.

Third-party code and data (licences next to the sources):

| Component | Use | Licence |
|-----------|-----|---------|
| libmysofa 1.3.5 (`code/thirdparty/libmysofa`) | SOFA reader, HRIR lookup, resampling | BSD-3-Clause |
| PFFFT (`code/thirdparty/pffft`) | FFT and spectral multiply-accumulate | BSD-like (FFTPACK) |
| SADIE II KU100 (`assets/main/sound/prom/hrtf`) | Default HRTF set, University of York | Apache-2.0 |

Ambisonic coefficients are real spherical harmonics (ACN, orthonormal) in Omaha listener space (x front, y left, z up).

## Device cvars

| Cvar | Meaning |
|------|---------|
| `s_khz` | Mix / device rate (11/22/44) |
| `s_speaker_type` | `0` stereo, `1` headphones, `2` 5.1, `3` quad. |
| `s_hrtf_file` | Omaha headphones: SOFA path in the game filesystem (default SADIE KU100 under `sound/prom/hrtf/`). Latched. |
| `s_hrtf_live_query` | Developer: `1` forces DIRECT live SOFA queries (bake still runs for the gate log). Latched. Default `0`. |
| `s_pain_hrtf` | Omaha only: `1` (default) force-loads short combat VO (pain/death) so they use the 3D/HRTF path instead of retail streamed 2D dialog. `0` keeps Miles-style streamed 2D. On force-load failure the cue stays streamed 2D. |
| `s_device` | Output device name; set `developer 1` to list devices at startup |
| `s_volume` | Master. Voices carry content gain only. `s_volume` and fade are one gain applied after the limiter, so raising volume never adds limiting. |
| `s_limiter` | Omaha only: `1` (default) enables the master limiter after mix and before `s_volume` (−8.7 dB fixed pre-trim, −0.2 dBFS ceiling, window-peak brickwall, 1.5 ms lookahead, 2 ms hold, 20 ms release). `0` bypasses. |
| `s_limiter_threshold` | Omaha only: gain-reduction depth in dB below the −0.2 dBFS ceiling (`0` … `−12`, default `−3.6`). Opposite makeup keeps peaks at the ceiling (deeper threshold raises the floor); live-tweaked each mix block. |
| `s_hf_duck` | Omaha: `1` (default) enables a dynamic 15.5 kHz high-shelf duck on the stereo master (after speaker EQ when two-speaker; after HRTF decode when headphones). 8th-order HP sidechain, 0 ms attack / 300 ms release; audio path = 4× cascaded RBJ shelves with slope `S=1`, bypassed at ~0 dB cut. Requires `s_mix 1`. `0` off for A/B. |
| `s_hf_duck_meter` | Omaha: `1` draws the HF duck overlay (left = sidechain + threshold, right = shelf cut). Default `0`. Requires `s_mix` and `s_hf_duck`. |
| `s_hf_duck_db` | Omaha: max shelf cut in dB when the HF sidechain is at 0 dB (default `24`, clamped 0…24). |
| `s_hf_duck_thresh` | Omaha: sidechain dB where shelf cut begins (default `−15`). Maps `thresh`…`0` → cut `0`…`−s_hf_duck_db`. Raise toward the live sidechain level if the duck meter stays pegged. |
| `s_hf_duck_sc_boost` | Omaha: make-up gain in dB on the HP sidechain before the envelope (default `5`, clamped 0…24). |
| `s_obstruction` | Omaha only: `1` (default) enables distance + occlusion low-pass on 3D voices (world-trace occlusion mixed with distance darkening). `0` disables both axes. |
| `s_mix` | Omaha only: `1` (default) applies hardcoded `OM_MIX_*` category gains (fire/foley/impacts/…). `0` forces unity gain for those scales; overlap ducking and pocket ducking stay active. Not used by the stock host. |
| `s_reverb` | Legacy stub (default `0`). No room reverb on the Omaha host. |

Per-category mix amounts / overlap / pocket / obstruction-filter knobs are
**compile-time** (`OM_MIX_*`, `OM_OVERLAP_*`, `OM_POCKET_*`, `OM_OBSTRUCT_*`).
`s_mix` only enables or disables applying `OM_MIX_*`. An archived tune overlay
lives at `artifacts/ui/modern/menus/obstruction_tune.xml`.

### Mix categories

Classification runs once per sample in the Omaha host: dialogue paths are
voice; otherwise the global aliases that play the file (alias-primary), then
wav path (`sound/weapons/fire|impact|zings|explo|foley/`, `vehicle/`, …), then `CHAN_*`. Alias/script/pk3 content volume is clamped to `[0, 127/84]` first (Miles full-knob sample-volume ceiling ≈ 1.51); then `volume *= OM_MIX_*` on StartSound (and loops from path), so category scales may still go above that. Uncontrolled (ambience, UI/notify, music, leftovers) always use gain `1`. See `code/client/omaha/snd_category.c`. `sound/prom/hitmarkers/` is uncontrolled.

## Obstruction (omaha)

Three point traces (source origin ±12 u vertical) against `CONTENTS_SOLID`
only; target = fraction blocked; mixer smooths (~80 ms). The direct path then
applies a low-pass from a **mix** of distance and occlusion cutoff curves
(`OM_OBSTRUCT_*`). Makeup scales with pole `a(fc)` as `((2-a)/a)^0.25` (tempered
white-noise bandwidth restore; open ≈ 1×, deep muffling up to 2×) so a filter
that has only just engaged does not jump in loudness. Distance loudness stays
on the normal spatial path. Exempt: listener's own entity,
`CHANNEL_FLAG_LOCAL_LISTENER`, streams/music. Trace interval is fixed at 100 ms.
`s_obstruction 0` disables the whole filter (distance and occlusion); it does not
leave distance darkening on.

Defaults: mix 0.5, dist range 3000, near/open **20000 Hz**, distance far
**7000 Hz**, occlusion blocked **10000 Hz**.

