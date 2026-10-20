#!/usr/bin/env python3
"""Behavioral contracts for the Omaha sound host: unlimited voices + overlap/pocket.
"""

from dataclasses import dataclass
from pathlib import Path
import math
import unittest


CLIENT = Path(__file__).resolve().parents[1]
SOUND_INFO = CLIENT / "snd_info.cpp"
HOST = CLIENT / "omaha" / "snd_omaha.cpp"
RENDERER = CLIENT / "omaha" / "snd_renderer.h"
OMAHA = CLIENT / "omaha" / "snd_render_omaha.cpp"
CATEGORY = CLIENT / "omaha" / "snd_category.c"


@dataclass
class Channel:
    entity: int
    entity_channel: int
    start_time: int
    playing: bool = True
    kind: int = 0


def pick_channel(channels, incoming_entity, incoming_channel, kind=0):
    """Same-entity+channel of the same kind replaces; otherwise allocate (no capacity steal)."""
    for index, channel in enumerate(channels):
        if (
            incoming_channel
            and channel.kind == kind
            and channel.playing
            and channel.entity == incoming_entity
            and channel.entity_channel == incoming_channel
        ):
            return index, "reuse"
    for index, channel in enumerate(channels):
        if channel.kind == kind and not channel.playing:
            return index, "free"
    return len(channels), "grow"


@dataclass
class Voice:
    origin: tuple
    volume: float
    min_dist: float = 200.0
    age: int = 0
    category: int = 2
    has_entity: bool = False
    looping: bool = False
    kind: int = 0


def level_at_listener(volume, origin, min_dist, listener):
    dist = math.dist(origin, listener)
    if min_dist <= 0 or dist <= min_dist:
        return volume, dist
    return volume * min_dist / dist, dist


def overlap_gain(voices, origin, volume, min_dist, category, listener=(0.0, 0.0, 0.0),
                 tau=70.0, power=0.7, spread=2.0):
    """Mirror of S_OMAHA_OverlapGain: returns (gain, overlap)."""
    new_level, new_dist = level_at_listener(volume, origin, min_dist, listener)
    new_dir = [o - l for o, l in zip(origin, listener)]
    overlap = 0.0
    for v in voices:
        if v.kind != 0 or v.category != category or v.has_entity or v.looping:
            continue
        if v.age >= tau * 4:
            continue
        old_level, old_dist = level_at_listener(v.volume, v.origin, v.min_dist, listener)
        level_w = min(old_level / new_level, 1.0)
        old_dir = [o - l for o, l in zip(v.origin, listener)]
        if new_dist > 0.001 and old_dist > 0.001:
            cos = sum(a * b for a, b in zip(new_dir, old_dir)) / (new_dist * old_dist)
        else:
            cos = 1.0
        dir_sim = min(max(0.5 * (1.0 + cos), 0.0), 1.0) ** spread
        near = 1.0 - max(new_dist / max(min_dist, 1.0), old_dist / max(v.min_dist, 1.0))
        dir_sim = max(dir_sim, near)
        overlap += math.exp(-v.age / tau) * level_w * dir_sim
    return (1.0 + overlap) ** -power, overlap


FIRE = 1
EXPLOSIONS = 4


@dataclass
class PocketVoice:
    origin: tuple
    volume: float
    entity: int
    category: int = FIRE
    age: int = 0
    min_dist: float = 200.0
    local: bool = False
    looping: bool = False


def pocket_gain(voices, origin, volume, entity, category, local=False, listener=(0.0, 0.0, 0.0),
                radius=512.0, tau=80.0, power=0.7, min_gain=0.35, min_dist=200.0):
    """Mirror of S_OMAHA_PocketGain plus the StartSound gate: returns (gain, overlap)."""
    if category == FIRE and local:
        return 1.0, 0.0
    new_level, _ = level_at_listener(volume, origin, min_dist, listener)
    overlap = 0.0
    for v in voices:
        if v.category != category or v.looping or v.age >= tau * 4:
            continue
        if category == FIRE and (v.entity == entity or v.local):
            continue
        dist = math.dist(v.origin, origin)
        if dist >= radius:
            continue
        if dist <= radius * 0.5:
            pocket = 1.0
        else:
            t = (radius - dist) / (radius * 0.5)
            pocket = t * t * (3.0 - 2.0 * t)
        old_level, _ = level_at_listener(v.volume, v.origin, v.min_dist, listener)
        overlap += math.exp(-v.age / tau) * min(old_level / new_level, 1.0) * pocket
    return max((1.0 + overlap) ** -power, min_gain), overlap


def simulate_fire(shooters, duration_ms=2000, interval_ms=86, life_ms=400, **kw):
    """Interleaved full-auto shooters: list of (entity, origin, phase_ms, local). Returns {entity: [gains]}."""
    events = []
    for entity, origin, phase, local in shooters:
        t = phase
        while t < duration_ms:
            events.append((t, entity, origin, local))
            t += interval_ms
    events.sort()
    playing = []
    gains = {s[0]: [] for s in shooters}
    for t, entity, origin, local in events:
        voices = [PocketVoice(origin=o, volume=vol, entity=e, age=t - st, local=lc)
                  for st, e, o, vol, lc in playing if t - st < life_ms]
        g, _ = pocket_gain(voices, origin, 1.0, entity, FIRE, local=local, **kw)
        gains[entity].append(g)
        playing.append((t, entity, origin, g, local))
    return gains


def start_burst(count, origin, floor=0.1, volume=1.0, voices=None):
    """Start `count` identical entity-less one-shots at once; returns the started voices."""
    voices = [] if voices is None else voices
    for _ in range(count):
        g, _ = overlap_gain(voices, origin, volume, 200.0, 2)
        if g < floor:
            continue
        voices.append(Voice(origin=origin, volume=volume * g))
    return voices


class SoundConcurrencyTests(unittest.TestCase):
    def test_same_entity_channel_is_reused(self):
        channels = [
            Channel(entity=7, entity_channel=6, start_time=90),
            Channel(entity=3, entity_channel=6, start_time=110),
        ]
        index, why = pick_channel(channels, incoming_entity=3, incoming_channel=6)
        self.assertEqual(index, 1)
        self.assertEqual(why, "reuse")

    def test_full_pool_grows_instead_of_stealing(self):
        channels = [Channel(entity=i + 1, entity_channel=6, start_time=100 + i) for i in range(10)]
        index, why = pick_channel(channels, incoming_entity=20, incoming_channel=6)
        self.assertEqual(index, 10)
        self.assertEqual(why, "grow")

    def test_free_slot_preferred_over_grow(self):
        channels = [
            Channel(entity=1, entity_channel=6, start_time=100, playing=False),
            Channel(entity=2, entity_channel=6, start_time=110, playing=True),
        ]
        index, why = pick_channel(channels, incoming_entity=9, incoming_channel=6)
        self.assertEqual(index, 0)
        self.assertEqual(why, "free")

    def test_replace_is_scoped_to_voice_kind(self):
        channels = [
            Channel(entity=3, entity_channel=6, start_time=100, kind=0),
            Channel(entity=3, entity_channel=6, start_time=110, kind=1),
        ]
        index, why = pick_channel(channels, incoming_entity=3, incoming_channel=6, kind=1)
        self.assertEqual((index, why), (1, "reuse"))
        self.assertTrue(channels[0].playing)

    def test_channel_numbers_stable_while_playing(self):
        channels = [Channel(entity=i + 1, entity_channel=6, start_time=100 + i) for i in range(4)]
        before = [(c.entity, i) for i, c in enumerate(channels)]
        for n in range(6):
            index, why = pick_channel(channels, incoming_entity=50 + n, incoming_channel=6)
            self.assertEqual(why, "grow")
            channels.append(Channel(entity=50 + n, entity_channel=6, start_time=200 + n))
        self.assertEqual(before, [(channels[i].entity, i) for i in range(4)])

    def test_pick_channel_grows_instead_of_stealing(self):
        source = HOST.read_text()
        pick = source[source.index("static int S_OMAHA_PickChannelBase(") :]
        pick = pick[: pick.index("\n}\n")]
        self.assertIn("S_OMAHA_GrowVoicePool(kind)", pick)
        self.assertIn("S_OMAHA_GrowLoopSounds()", source)
        self.assertNotIn("S_Renderer_IsOmaha", source)

    def test_renderer_seam_and_omaha_backend_exist(self):
        self.assertTrue(RENDERER.is_file())
        self.assertTrue(OMAHA.is_file())
        renderer = RENDERER.read_text()
        self.assertIn("sndRenderer_t", renderer)
        self.assertIn("S_OMAHA_CaptureBegin", renderer)
        omaha = OMAHA.read_text()
        self.assertIn("ma_device", omaha)
        self.assertIn("Om_BuildSinc", omaha)

    def test_host_does_not_include_the_old_audio_api(self):
        for path in (CLIENT / "omaha").iterdir():
            text = path.read_text()
            self.assertNotIn("q" + "al.h", text, path.name)
            self.assertNotIn("q" + "alSource", text, path.name)
        self.assertIn("const sndHost_t s_sndHostOmaha", HOST.read_text())

    def test_surround_is_mixed_by_omaha(self):
        renderer = RENDERER.read_text()
        self.assertIn("5.1", renderer)
        self.assertIn("quad", renderer)
        host = (CLIENT / "snd_host.cpp").read_text()
        self.assertIn("s_sndHost = &s_sndHostOmaha;", host)
        self.assertNotIn("s_sndHostO" + "penAL", host)

    def test_headphone_hrtf_routing(self):
        renderer = RENDERER.read_text()
        self.assertIn("OM_ROUTE_DRY", renderer)
        source = HOST.read_text()
        start = source.index("static int S_OMAHA_SpatialRoute(const omaha_channel")
        route = source[start:source.index("\n}\n", start)]
        direct = route[route.index("switch"):route.index("return OM_ROUTE_DIRECT;", route.index("switch"))]
        for cat in ("FIRE", "FOLEY", "FOOTSTEPS", "BODYFALLS", "VOICE", "VEHICLES"):
            self.assertIn(f"case SND_CAT_{cat}:", direct)
        for cat in ("DEBRIS", "IMPACTS", "EXPLOSIONS"):
            self.assertNotIn(f"SND_CAT_{cat}", direct)
        self.assertIn("S_OMAHA_IsLocalEntity", route)
        self.assertIn("CHANNEL_FLAG_LOCAL_LISTENER", route)
        self.assertIn("OM_ROUTE_DRY", route)
        self.assertIn('"s_hrtf_file"', source)

    def test_category_is_classified_host_side(self):
        source = HOST.read_text()
        self.assertIn("S_SoundCategory_ClassifyName(", source)
        self.assertIn("S_SoundCategory_FromChannel(", source)
        self.assertIn('"sound/prom/hitmarkers/"', CATEGORY.read_text())
        cgame = (CLIENT.parent / "cgame" / "cg_specialfx.cpp").read_text()
        self.assertNotIn("CG_StampSoundEntity", cgame)

    def test_metadata_values_are_consumed_and_floats_stay_fractional(self):
        source = SOUND_INFO.read_text()
        self.assertNotIn("if (!tiki.TokenAvailable(qtrue))", source)
        self.assertIn("max_factor = atof(token);", source)
        self.assertIn("number_of_sfx_infos == MAX_SFX_INFOS", source)


class OverlapDuckingTests(unittest.TestCase):
    def test_coincident_burst_is_bounded(self):
        voices = start_burst(16, (600.0, 0.0, 0.0))
        total = sum(v.volume for v in voices)
        self.assertLess(total, 16 * 0.5)
        gains = [v.volume for v in voices]
        self.assertEqual(gains, sorted(gains, reverse=True))

    def test_later_starts_drop_below_floor(self):
        voices = start_burst(16, (600.0, 0.0, 0.0), floor=0.3)
        self.assertLess(len(voices), 16)
        self.assertGreater(len(voices), 1)

    def test_clusters_45_degrees_apart_barely_interact(self):
        left = (600.0, 0.0, 0.0)
        right = (600.0 * math.cos(math.radians(45)), 600.0 * math.sin(math.radians(45)), 0.0)
        voices = start_burst(4, left)
        g_same, _ = overlap_gain(voices, left, 1.0, 200.0, 2)
        g_default, _ = overlap_gain(voices, right, 1.0, 200.0, 2)
        g_narrow, _ = overlap_gain(voices, right, 1.0, 200.0, 2, spread=16.0)
        self.assertLess(g_same, g_default)
        self.assertGreater(g_narrow, 0.85)

    def test_quiet_far_voice_barely_ducks_loud_near_one(self):
        far = Voice(origin=(4000.0, 0.0, 0.0), volume=0.5)
        g, _ = overlap_gain([far], (300.0, 0.0, 0.0), 1.0, 200.0, 2)
        self.assertGreater(g, 0.95)

    def test_entity_and_looping_voices_are_never_counted(self):
        voices = [
            Voice(origin=(600.0, 0.0, 0.0), volume=1.0, has_entity=True),
            Voice(origin=(600.0, 0.0, 0.0), volume=1.0, looping=True),
            Voice(origin=(600.0, 0.0, 0.0), volume=1.0, kind=1),
            Voice(origin=(600.0, 0.0, 0.0), volume=1.0, category=6),
            Voice(origin=(600.0, 0.0, 0.0), volume=1.0, age=280),
        ]
        g, overlap = overlap_gain(voices, (600.0, 0.0, 0.0), 1.0, 200.0, 2)
        self.assertEqual((g, overlap), (1.0, 0.0))

    def test_start_path_only_ducks_entity_less_one_shots(self):
        source = HOST.read_text()
        self.assertIn("static float S_OMAHA_OverlapGain(", source)
        self.assertIn("!bPocket && OM_OVERLAP_ENABLE && iEntNum == ENTITYNUM_NONE && vOrigin", source)
        self.assertIn("cat != SND_CAT_FOOTSTEPS && cat != SND_CAT_BODYFALLS", source)
        self.assertIn("cat != SND_CAT_AUTO", source)
        start = source.index("void S_OMAHA_StartSound(")
        body = source[start:source.index("\n}\n", start)]
        self.assertLess(body.index("S_OMAHA_OverlapGain("), body.index("S_OMAHA_PickChannel3D("))


class PocketDuckingTests(unittest.TestCase):
    ROOM = [(10 + i, (800.0 + 60 * i, 100.0 * (i % 2), 0.0), i * 21, False) for i in range(4)]

    def test_four_shooters_in_a_room_stay_well_below_4x(self):
        gains = simulate_fire(self.ROOM)
        steady = sum(sum(g[5:]) / len(g[5:]) for g in gains.values())
        self.assertLess(steady, 2.5)
        for g in gains.values():
            self.assertGreaterEqual(min(g), 0.35)

    def test_lone_shooter_is_never_ducked(self):
        gains = simulate_fire([(10, (800.0, 0.0, 0.0), 0, False)], interval_ms=40)
        self.assertEqual(set(gains[10]), {1.0})

    def test_local_fire_neither_ducks_nor_is_ducked(self):
        gains = simulate_fire([(1, (0.0, 0.0, 0.0), 0, True), (10, (100.0, 0.0, 0.0), 5, False)])
        self.assertEqual(set(gains[1]), {1.0})
        self.assertEqual(set(gains[10]), {1.0})

    def test_far_apart_shooters_do_not_interact(self):
        gains = simulate_fire([(10, (800.0, 0.0, 0.0), 0, False), (11, (800.0, 1500.0, 0.0), 10, False)])
        self.assertEqual(set(gains[10]) | set(gains[11]), {1.0})

    def test_quiet_far_shooter_barely_ducks_loud_near_one(self):
        listener = (2300.0, 0.0, 0.0)
        quiet = PocketVoice(origin=(3000.0, 0.0, 0.0), volume=0.5, entity=11)
        loud = PocketVoice(origin=(3000.0, 0.0, 0.0), volume=1.0, entity=11, min_dist=1000.0)
        g_quiet, _ = pocket_gain([quiet], (2700.0, 0.0, 0.0), 1.0, 10, FIRE, listener=listener)
        g_loud, _ = pocket_gain([loud], (2700.0, 0.0, 0.0), 1.0, 10, FIRE, listener=listener)
        self.assertGreater(g_quiet, 0.8)
        self.assertLess(g_loud, g_quiet)

    def test_coincident_explosions_bounded_and_never_dropped(self):
        voices = []
        for _ in range(16):
            g, _ = pocket_gain(voices, (600.0, 0.0, 0.0), 1.0, 0, EXPLOSIONS, tau=120.0)
            self.assertGreaterEqual(g, 0.35)
            voices.append(PocketVoice(origin=(600.0, 0.0, 0.0), volume=g, entity=0, category=EXPLOSIONS))
        self.assertEqual(len(voices), 16)
        self.assertLess(sum(v.volume for v in voices), 16 * 0.5)

    def test_start_path_routes_fire_and_explosions_to_pocket(self):
        source = HOST.read_text()
        self.assertIn("static float S_OMAHA_PocketGain(", source)
        self.assertIn("bPocket     = pSfxInfo->loop_start == -1", source)
        self.assertIn("cat == SND_CAT_EXPLOSIONS || (cat == SND_CAT_FIRE && !S_OMAHA_IsLocalEntity(iEntNum))", source)
        self.assertIn("cat != SND_CAT_EXPLOSIONS && cat != SND_CAT_FOOTSTEPS", source)
        start = source.index("void S_OMAHA_StartSound(")
        body = source[start:source.index("\n}\n", start)]
        self.assertLess(body.index("S_OMAHA_PocketGain("), body.index("S_OMAHA_PickChannel3D("))
        pocket = body[body.index("S_OMAHA_PocketGain("):body.index("Added in Omaha: overlap ducking")]
        self.assertNotIn("return;", pocket)


class OpenALLeftoverTests(unittest.TestCase):
    def test_set_3d_does_not_reset_gain(self):
        source = HOST.read_text()
        start = source.index("void omaha_channel::set_3d()")
        body = source[start:source.index("\n}\n", start)]
        self.assertNotIn("cur.gain", body)

    def test_silent_triggered_music_is_paused_without_play(self):
        source = HOST.read_text()
        start = source.index("\nvoid S_OMAHA_TriggeredMusic_SetupHandle(")
        body = source[start:source.index("\n}\n", start)]
        branch = body.index("if (autostart)")
        self.assertNotIn("play()", body[:branch])
        self.assertLess(branch, body.index("chan_trig_music.play()"))
        self.assertLess(body.index("chan_trig_music.play()"), body.index("chan_trig_music.pause()"))


class MixerTableTests(unittest.TestCase):
    def test_mixer_id_tables_are_not_fixed_caps(self):
        source = OMAHA.read_text()
        self.assertNotIn("OM_MAX_VOICES", source)
        self.assertNotIn("OM_MAX_HANDLES", source)
        self.assertNotIn("OM_MAX_SOUNDS", source)
        self.assertIn("static qboolean Om_GrowVoices(void)", source)
        self.assertIn("static int Om_TakeVoice(void)", source)
        self.assertIn("c.slot = Om_TakeVoice();", source)


if __name__ == "__main__":
    unittest.main()
