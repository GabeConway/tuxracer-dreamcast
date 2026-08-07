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

CORRECTED 2026-08-07: the KOS stream and sfxmgr *playback* calls in this table
were replaced by `dc/src/dc_aica.c`, which writes the AICA channel registers
from the SH-4. KOS's ARM firmware never runs under Flycast — the evidence is at
the bottom of this file. Loading still uses KOS.

| SDL_mixer | Backing | Notes |
|---|---|---|
| `Mix_OpenAudio` | `dc_aica_init()` (which calls `snd_init()`) + a sound-RAM music ring | one call brings up both halves |
| `Mix_CloseAudio` | `dc_aica_stop_all()` + `snd_mem_free` | joins the music thread first |
| `Mix_QuerySpec` | returns cached spec | reports actual (always 16-bit), not requested |
| `Mix_LoadWAV` | `snd_sfx_load` | KOS parses the WAV and DMAs it into sound RAM; the handle is read as a `dc_snd_effect_t` |
| `Mix_FreeChunk` | `snd_sfx_unload` | halts owning voices first — upstream doesn't (`src/audio_data.c:477`) |
| `Mix_PlayChannel(-1,…)` | `alloc_voices()` + `dc_aica_play_delayed` ×2 + `dc_aica_key_on` | stereo = two adjacent voices keyed on together |
| `Mix_HaltChannel` | `dc_aica_stop` over the voice group | `-1` sweeps every effect voice |
| `Mix_Volume(-1,v)` | master scalar + `dc_aica_set_vol` | pushed to sounding voices |
| `Mix_VolumeChunk` | per-chunk scalar + `dc_aica_set_vol` | required: `src/racing.c:334-361` re-sets terrain volume every frame |
| `Mix_LoadMUS` | `fopen` probe + absolute-path `strdup` | **does not decode** — see §4 |
| `Mix_PlayMusic` | `ModPlug_Load` + two ring voices keyed on together | refill thread at 20 ms |
| `Mix_HaltMusic` / `Mix_FreeMusic` | `dc_aica_stop` ×2 + `ModPlug_Unload` | |
| `Mix_PlayingMusic` | flag cleared when `ModPlug_Read` returns 0 | |
| `Mix_VolumeMusic` | `dc_aica_set_vol` on both ring voices | 0..128 → 0..255 |
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

The AICA's loop registers are 16 bit, so no voice can address more than 65534
samples; `dc_aica.c` clamps to that, as KOS's `snd_sfx_play_ex` did
(`snd_sfxmgr.c:766`). At 44100 Hz the cap is 1.486 s. `tux_on_snow1`, `tux_on_ice1` and `tux_on_rock1` are 2.12 / 2.40 /
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
It is the same lock the music thread takes around its refill pass, so it
genuinely blocks the decode — which is what SDL_LockAudio means.
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
- **Not run on real hardware.** CORRECTED 2026-08-07: this used to say "not run
  on hardware or Flycast". It runs on Flycast — music streams and effects play
  through a full race (see the bottom of this file). Silicon is still untested.

---

# Silence: found, and fixed by leaving the ARM out (2026-08-07)

The port produced no audible sound at all. Two causes, one behind the other.

## 1. `Mix_LoadMUS` stored a relative path

`Mix_LoadMUS` deliberately does not decode -- it records the path and
`Mix_PlayMusic` does the `ModPlug_Load`, because five decoded modules do not
fit in 16 MB. The path it recorded was the caller's, and the caller's is
relative: `data/tuxracer_init.tcl:31` passes `music/start1-jt.it`.

Between the two calls the game changes directory (`src/course_load.c:345`,
`src/file_util.c`, and `tuxracer_init.tcl`'s own `tux_goto_data_dir`), so by
the time the splash screen asks for music the `fopen` misses. `Mix_PlayMusic`
returns -1 and **the game has no error handler for it**, so this failed in
complete silence. Fixed: the path is made absolute in `Mix_LoadMUS`.

## 2. KOS's ARM sound firmware never runs under Flycast

With the path fixed, the stream started and then the callback ran exactly four
times -- the prefill -- and stopped forever. `snd_stream_poll()` decides how
much to ask for by reading a play position out of SPU RAM at
`AICA_MEM_CHANNELS`, and **that structure is written by the ARM**, not by
hardware. A frozen callback count therefore means the ARM stopped.

`harness/dc/audiotest` is the program that took this apart. It has no game, no
GL and no shim in it, and it establishes, in order (all MEASURED on
Flycast v2.6 + reios, KOS 2.3.0):

| Question | Answer |
|---|---|
| Did the firmware upload land? | Yes. SPU RAM word 0 reads `ea00002d`, the ARM reset branch. |
| Is the ARM out of reset? | Yes. `0xa0702c00` reads `0x00000300`, bit 0 clear. |
| Does the ARM execute at all? | **Yes** -- a hand-assembled probe loop ran 478k iterations in 500 ms. |
| Does timer A run? | Yes. TIMA counts, SCIPD bit 6 (timer A) is pending, SCIEB is `0x40`. |
| Does the timer FIQ reach the ARM? | **No.** `AICA_MEM_CLOCK` never moves and the command queue tail never advances. |

The reason is one instruction. `harness/dc/fiqprobe.s` sets timer A up exactly
as `aica_init()` does and enables FIQ the way KOS's `arm_fiq_enable()` does:

```
mrs r0, cpsr / orr r0,r0,#0x80 / bic r0,r0,#0x40 / msr CPSR_c, r0
```

Under Flycast the CPSR reads back **unchanged** (`0x53`, F still set) after
that `msr`, and no FIQ is ever taken. Replacing the last instruction with the
full-field form -- `msr CPSR, r0` -- takes the FIQ 80,856 times in 500 ms with
`INTREQ == 2`. **Flycast's ARM7 ignores the byte-field `msr CPSR_c`.**

That matters because `arm_main()` ends every loop iteration in `timer_wait()`
(`sound/arm/main.c:224`), which spins until `AICA_MEM_CLOCK` moves, and only
the FIQ handler moves it. No FIQ, and the firmware hangs on its very first
iteration -- before it has processed a single SH-4 command. Every `Mix_*` call
the game made was queued to a processor that was never going to read the queue.
That is why **effects were silent too**, not just music.

### Patching the firmware was tried, and does not work

The firmware is shipped prebuilt (`stream.drv.prebuilt`, 3344 bytes; there is
no ARM compiler in the SDK image), so the obvious fix is to patch those two
words in SPU RAM between `snd_init()` and letting the ARM out of reset. It was
tried, in both the in-place (`g2_write_32`) and block-reload
(`spu_memread`/patch/`spu_memload`) forms, and with `timer_wait()` and
`aica_init()` stubbed out on top. MEASURED: the patched words read back
correctly, the ARM restarts, and the main loop still never runs -- a heartbeat
counter spliced into `timer_wait()` stayed at 0 while a whole-program upload of
`fiqprobe` to the same address in the same run ran fine. Not diagnosed further,
because by then there was a better answer.

## 3. The fix: drive the AICA from the SH-4

The ARM is only a proxy. The AICA's channel registers are in the G2 register
file at `0xa0700000`, and the SH-4 can write them itself.
`dc/src/dc_aica.c` does exactly that, with the same register sequence
`sound/arm/aica.c` would have executed, and `dc/src/dc_mixer.c` is built on it.
KOS is still used for what is pure SH-4 code: `snd_mem_malloc` and
`snd_sfx_load`.

| Layer | Before | Now |
|---|---|---|
| Effects | `snd_sfx_play_ex` -> ARM queue | `dc_aica_play_delayed` + `dc_aica_key_on` per voice |
| Music | `snd_stream` + `snd_stream_poll` | two voices looping over a 16 KB-per-channel ring in sound RAM, refilled ahead of the play position |
| Position feedback | `chans[ch].pos` in SPU RAM (ARM-written) | the AICA's own register: select in `0x280d`, read `0x2814` |
| Voice allocation | `snd_sfx_chn_alloc` round-robin | `alloc_voices()` in dc_mixer.c; voices 0/1 reserved for music |

MEASURED, playtest run `smoke-TuxRacer-20260807-191004`: music streams
continuously (`reads` climbing, ring write cursor tracking the play position
through wrap after wrap), ten `Mix_PlayChannel` calls during the race, and the
run reaches `MARK:RACING` and exits clean.

**Cost**: the racing view went from 12-15 fps to 9-11. That is libmodplug
decoding 22050 Hz stereo on the SH-4 alongside the game, which was always going
to be the price; it is per-sample, so batching the refills does not recover it
(2048-sample fills measured *worse*, 4-8 fps -- see `DC_MUS_MIN_FILL`). The
lever, if it is needed, is the decode itself: `mResamplingMode` to `NEAREST`,
then `mChannels` to 1, then `mFrequency` to 11025.

## 4. What is still wrong

- **Three effects are longer than the hardware can address.** The AICA's loop
  registers are 16-bit, so 65534 samples is the ceiling, and
  `tux_on_snow1` / `tux_on_ice1` / `tux_on_rock1` are 93k / 106k / 190k frames.
  They now loop over their first 65534 samples instead of being truncated at an
  arbitrary point by KOS, but the real fix is still an asset one: convert them
  to mono 22050 Hz, which also drops the SFX budget from 1.60 MiB to ~160 KB.
- **Not run on real hardware.** Everything above is Flycast. The register
  sequence is KOS's own, so it should behave, but nobody has heard it on a
  Dreamcast.
