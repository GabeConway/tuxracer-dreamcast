# design-audio — SDL_mixer on KallistiOS

`src/audio.c` (854 lines) and `src/audio_data.c` are built verbatim and call
SDL_mixer. SDL_mixer has no KOS port. `dc/src/dc_mixer.c` is the replacement;
`dc/include/SDL_mixer.h`, `dc/include/SDL.h`, `dc/include/SDL_types.h` declare it.

## 1. The surface that actually has to exist

```
grep -rhno 'Mix_[A-Za-z_]*' src/ | sort -u
```
18 symbols: `Mix_Chunk Mix_Music Mix_OpenAudio Mix_CloseAudio Mix_QuerySpec
Mix_LoadWAV Mix_FreeChunk Mix_PlayChannel Mix_HaltChannel Mix_Volume
Mix_VolumeChunk Mix_LoadMUS Mix_FreeMusic Mix_PlayMusic Mix_HaltMusic
Mix_PlayingMusic Mix_VolumeMusic Mix_GetError`. Nothing else is declared.

SDL side, from `src/audio.c src/audio_data.c src/loop.c`: `SDL_Init`,
`SDL_INIT_AUDIO`, `SDL_GetError`, `SDL_GetTicks`. `SDL_LockAudio`,
`SDL_UnlockAudio` and `SDL_Delay` come from `src/winsys.c:333,397,410` and are
declared for whoever replaces it.

`src/` never dereferences a `Mix_Chunk` or `Mix_Music` — `src/audio_data.c:139`
casts the pointer to `char*` purely for a NULL test — so both are opaque here.

**Verified**: `src/audio.c` syntax-checks clean (0 errors, 0 warnings, gnu89)
against these headers with only Tcl stubbed. `src/audio_data.c` produces no
SDL/Mix diagnostics; its only errors are `Tcl_Obj` at lines 613/641, which
belongs to the Tcl agent. `dc/src/dc_mixer.c` builds clean under
`kos-cc -std=gnu99 -Wall -Wextra` (5025 text / 38 data / 608 bss).

## 2. Mapping

| SDL_mixer | KOS backing | Notes |
|---|---|---|
| `Mix_OpenAudio` | `snd_stream_init_ex(2, 16384)` | implies `snd_init()`, so sfx comes up too |
| `Mix_CloseAudio` | `snd_stream_destroy` + `snd_stream_shutdown` | joins the music thread first |
| `Mix_QuerySpec` | returns cached spec | reports actual (always 16-bit), not requested |
| `Mix_LoadWAV` | `snd_sfx_load` | wraps `sfxhnd_t` + a 0..128 volume |
| `Mix_FreeChunk` | `snd_sfx_unload` | halts owning voices first — upstream doesn't (`src/audio_data.c:477`) |
| `Mix_PlayChannel(-1,…)` | `snd_sfx_play_ex(.chn=-1)` | returns the voice, matching SDL |
| `Mix_HaltChannel` | `snd_sfx_stop` | `-1` sweeps all 64 |
| `Mix_Volume(-1,v)` | master scalar + `AICA_CH_CMD_UPDATE\|SET_VOL` | pushed to sounding voices |
| `Mix_VolumeChunk` | per-chunk scalar + same AICA update | required: `src/racing.c:334-361` re-sets terrain volume every frame |
| `Mix_LoadMUS` | `fopen` probe + `strdup(path)` | **does not decode** — see §4 |
| `Mix_PlayMusic` | `ModPlug_Load` + `snd_stream_start` | decode thread at 20 ms |
| `Mix_HaltMusic` / `Mix_FreeMusic` | `snd_stream_stop` + `ModPlug_Unload` | |
| `Mix_PlayingMusic` | flag cleared when `ModPlug_Read` returns 0 | |
| `Mix_VolumeMusic` | `snd_stream_volume` | 0..128 → 0..255 |
| `Mix_GetError` | `#define Mix_GetError SDL_GetError` | same aliasing real SDL_mixer uses |

Volumes: SDL 0..128 (`MIX_MAX_VOLUME`, clamped at `src/audio.c:645,662`), AICA
0..255. Folded as `chunk_vol * master * 255 / (128*128)`.

**Nothing is a no-op.** Music is fully implemented, not stubbed.

## 3. AICA sound RAM budget — MEASURED from `data/sounds/`

Every shipped WAV is **44100 Hz / stereo / 16-bit**:

| file | frames | seconds | AICA bytes |
|---|---:|---:|---:|
| fish_pickup1/2/3.wav | 1404 / 1336 / 1084 | 0.03 ea | 15,296 |
| tux_hit_tree1.wav | 26,496 | 0.60 | 105,984 |
| tux_on_snow1.wav | 93,312 | 2.12 | 373,248 |
| tux_on_ice1.wav | 105,984 | 2.40 | 423,936 |
| tux_on_rock1.wav | 190,080 | 4.31 | 760,320 |
| **total** | | | **1,678,784 (1.60 MiB)** |

Plus the music stream ring: `DC_MUS_BUFSIZE` 16 KiB × 2 channels = 32,768 B.
**Total 1,711,552 B against a 2,097,152 B pool** (`snd_mem.c:101`, minus the
AICA driver reserve). It fits, with roughly 375 KB of headroom before the
reserve. `dc_mixer.c` logs the real `snd_mem_available()` delta per load, so a
boot log supersedes this table.

### RISK: three effects exceed the hardware sample limit

`snd_sfx_load` accepts anything but only **warns** past 65534 samples
(`snd_sfxmgr.c:409-411`); `snd_sfx_play_ex` then **truncates**
(`snd_sfxmgr.c:766` — `if(size >= 65535) size = 65534;`). At 44100 Hz that cap
is 1.486 s. `tux_on_snow1`, `tux_on_ice1` and `tux_on_rock1` are 2.12 / 2.40 /
4.31 s — the three continuously-looping terrain sounds that carry the entire
in-game soundscape. Audible result: each loops at an arbitrary cut point, i.e.
a periodic click, and it burns full sound RAM for audio it can never reach.

This is an **asset problem, not a code problem**. Converting the three to mono /
22050 Hz / ≤ 65534 samples fixes the truncation and drops the whole SFX budget
from 1.60 MiB to roughly 160 KB. That belongs in the data pipeline
(`tools/fetch-data.sh` is the only tool present); it is not done.

## 4. Music: lazy load, on purpose

`data/music/` is 5 Impulse Tracker modules, 25–127 KB on disc
(`start1-jt.it` 127,397 B is the largest). `data/tuxracer_init.tcl:31-50` loads
all five at startup and binds them with loop `-1` (except `game_over`, loop 1).

`Mix_LoadMUS` records the path; `Mix_PlayMusic` does the `ModPlug_Load`. A
decoded module holds its unpacked patterns and samples in **main RAM**, and 16 MB
does not have room for five of those alongside the game. Cost: a GD-ROM read +
parse hitch at each track change. This also happens to be why looping works —
`mLoopCount` only takes effect on the next `ModPlug_Load` (`modplug.h:84-86`),
which is exactly the call we make.

Resampling is set to `MODPLUG_RESAMPLE_LINEAR` with no effect flags. **ASSUMED,
not measured**: that a 200 MHz SH-4 can decode a stereo 22050 Hz .it inside the
frame budget while running the game. Nobody has run this on hardware. If it
starves, the knobs in order are: `mResamplingMode` → `NEAREST`, `mChannels` → 1,
`mFrequency` → 11025. If it still starves, `Mix_PlayMusic` returning -1 leaves
the game correct and silent.

Decode runs on a `thd_create` thread that polls every 20 ms; 16 KiB per channel
is ~186 ms of 22050 Hz stereo, so the margin is ~9x.

## 5. Locking

`SDL_LockAudio`/`SDL_UnlockAudio` map to a **recursive `mutex_t`**, not a no-op.
It is the same lock the music thread takes around `snd_stream_poll`, so it
genuinely blocks the decode callback — which is what SDL_LockAudio means.
Recursive because a holder may call a `Mix_*` entry point that locks again.

The one caller in upstream (`src/winsys.c:333,397`) locks and unlocks back to
back, an SDL-era scheduling hack; `src/winsys.c` is not built here. If a
`dc_winsys.c` ever holds the lock across a long operation, music starves — the
poll thread cannot get in.

## 6. Known gaps

- **`loops > 0`** (finite repeats) plays as loop-forever. The AICA loop flag is
  a boolean (`snd_sfxmgr.c:775`). Tux Racer only passes 0 or -1
  (`src/racing.c`, `src/phys_sim.c:767`), so this is unreachable today.
- **Per-channel volumes** are not tracked; one master scalar. `src/audio.c:652`
  only ever calls `Mix_Volume(-1, v)`.
- **`Mix_QuerySpec` reports 16-bit always**, even if `AUDIO_U8` was requested
  (`src/audio.c:96`). `is_audio_open()` is the only caller and ignores the values.
- **Not run on hardware or Flycast.** Every claim above is from source reading,
  header inspection and a syntax-check build. No `.cdi` has been booted.

---

# Silence, and how far it has been chased (2026-08-07)

The port produces no audible sound. Two things are established and one is not.

## Fixed: `Mix_LoadMUS` stored a relative path

`Mix_LoadMUS` deliberately does not decode — it records the path and
`Mix_PlayMusic` does the `ModPlug_Load`, because five decoded modules do not
fit in 16 MB. The path it recorded was the caller's, and the caller's is
relative: `data/tuxracer_init.tcl:31` passes `music/start1-jt.it`.

Between the two calls the game changes directory — `src/course_load.c:345`
and `src/file_util.c` walk in and out of course directories, and
`tuxracer_init.tcl` has its own `tux_goto_data_dir`. By the time the splash
screen asks for music, the working directory is elsewhere and the `fopen`
misses.

MEASURED, with `-DTR_AUDIO_TRACE`:

```
before:  PlayMusic 0x8c3d6b28 loops=-1
         polls=98 cb=0 playing=0 stream=0 mpf=0x0     <- never started
after:   LoadMUS resolved -> /cd/music/start1-jt.it
         polls=98 cb=4 playing=1 stream=0 mpf=0x8c4956f8
```

`Mix_PlayMusic` returns -1 on that path and **the game has no error handler for
it**, so this failed in complete silence and looked exactly like an AICA or a
libmodplug problem. The path is now made absolute in `Mix_LoadMUS`.

## Not fixed: the AICA does not drain the stream

With the module loaded and the stream started, the callback runs **four times
and then stops forever**:

```
dc_mixer: polls=98 cb=4 playing=1 stream=0 mpf=0x8c4956f8
dc_mixer: polls=98 cb=4 playing=1 stream=0 mpf=0x8c4956f8   (unchanged, every 2 s)
```

`polls` is the music thread looping ~50 times a second and calling
`snd_stream_poll`; `cb` counts entries to `mus_get_data`. Four callbacks is the
prefill `snd_stream_start` performs. After that `snd_stream_poll` never asks
for another byte, which means KOS believes the ring is still full, which means
**the AICA's read pointer is not advancing**.

So the guest is not producing samples — this is not a host playback problem,
and there is no point looking at Flycast's audio backend until the read pointer
moves.

What is already ruled out:

- `snd_stream_init_ex` / `snd_stream_alloc` / `snd_stream_start` all succeed
  (`mus_stream` is a valid handle, no warning logged).
- `mus_rate` is 22050 and `spec_channels` is 2 — not a zero-frequency channel.
- `snd_init()` works: every effect uploads into AICA RAM successfully
  (357,536 B used of 2 MB), which goes through the same driver.
- The music thread is alive and holds/releases the lock correctly.

## Next step

Find out whether the AICA ARM core is executing KOS's driver at all under
Flycast + reios. The cheap probe is a standalone KOS program in
`harness/dc/` — no game, no shim, in the shape of `harness/dc/gltest/` — that
calls `snd_stream_start` on a generated tone and prints the callback count once
a second. If that also freezes at the prefill, the problem is KOS-vs-Flycast
and not this file; if it streams correctly, the difference is something this
mixer does, and the first suspect is the SFX voice reservation
(`snd_sfx_chn_alloc`) colliding with the two voices the stream needs.

Do not "fix" this by changing volumes or the resampling mode. Neither can move
a read pointer.
