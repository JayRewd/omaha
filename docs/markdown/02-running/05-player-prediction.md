# Player prediction

Project: Omaha can **show other players a little ahead on your screen** so aiming
online feels closer to what you see. This is **display only**: nothing extra is
sent to the server, and it works on normal retail / stock servers.

Find it under **Settings → Gameplay → Remote prediction**.

## Modes

| Mode | What it does |
|------|----------------|
| **Off** | Stock look — other players stay where the last network updates put them. |
| **Safe** (default) | Fills small gaps when your client has already used the newest update and is waiting for the next one. Steadier motion; little help with leading shots. |
| **Lead** | Advances other players by roughly your ping (capped by **Prediction max lead**), so you usually don’t have to aim as far ahead of the model. |

**Prediction max lead** (0–150 ms, default **100**) only applies in **Lead**. It
caps how far ahead the display may go. Set it to `0` to turn lead distance off
while leaving the mode selected.

## Accuracy (Lead mode)

Measured by comparing the predicted spot to where the player actually was a
moment later (spectate overlap tests). Distances are in game units — smaller is
better. Roughly: **under 8** is very tight; **under 16** is still close on the
model; **32+** is a clear miss of the prediction.

| Your ping (about) | Average miss | Within 8 | Within 16 | Within 32 |
|-------------------|--------------|----------|-----------|-----------|
| ~30 ms | 3.8 | 90% | 99% | 100% |
| ~60 ms | 6.9 | 68% | 94% | 99% |
| ~90 ms | 6.2 | 73% | 88% | 100% |
| ~110 ms | 10.4 | 51% | 76% | 97% |
| ~130 ms | 14.5 | 38% | 64% | 92% |

**Takeaway:** through about **100–120 ms**, Lead stays useful for most shots.
Past that, misses grow and you may want a lower max lead — or **Safe** / **Off**.

The ~90 ms row had far fewer samples than the others; treat it as a soft check,
not a hard guarantee.

## Limits

- Sudden jukes or jumps that haven’t shown up in a network update yet cannot be
  guessed correctly.
- Players in water, on ladders, or in noclip fall back toward stock behavior.
- Turning prediction while airborne is limited.
- This only changes **how others look on your machine**. Server hit detection is
  unchanged.

## Console / config

| Cvar | Default | Notes |
|------|---------|-------|
| `cg_remotePrediction` | `1` | `0` Off, `1` Safe, `2` Lead |
| `cg_remotePredictionMaxLead` | `100` | Ceiling in ms for Lead (`0`–`150`) |

Also documented under [Configuration](../03-configuration/01-configuration.md).
