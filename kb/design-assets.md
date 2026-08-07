# Assets on the disc — evidence

`data/` is upstream, untouched, fetched and sha256-verified by
`tools/fetch-data.sh`. `data-dc/` is what actually gets burned, produced by
`tools/stage-data.sh`. Neither is committed.

Today the only transform is the sound effects. Everything else — courses,
textures, fonts, music, the `.tcl` scripts — is copied byte for byte.

## The AICA sample limit, and why it forced a data change

The Dreamcast's AICA plays at most **65534 samples** per voice. KOS does not
enforce it: `snd_sfx_load` warns (`snd_sfxmgr.c:409`) and `snd_sfx_play_ex`
silently **truncates** (`snd_sfxmgr.c:766`), so an oversized effect is not an
error, it is a click.

Every WAV upstream ships is 44100 Hz stereo 16-bit. At that rate the limit is
1.486 s, and **three of the seven are over it** — precisely the three that
loop continuously under the player:

| File | Upstream | | Staged |
|---|---|---|---|
| `tux_on_snow1.wav` | 2.12 s | → | 46,673 samples |
| `tux_on_ice1.wav` | 2.40 s | → | 53,009 samples |
| `tux_on_rock1.wav` | 4.31 s | → | 63,962 samples |
| `tux_hit_tree1.wav` | 0.60 s | → | 13,265 samples |
| `fish_pickup{1,2,3}.wav` | short | → | 559–719 samples |

Staging is mono, 22050 Hz, trimmed to 2.90 s. Measured result:

```
sfx total: 358,052 B of the 2,097,152 B AICA pool   (was 1,678,784 B)
```

A 4.7× reduction, and `stage-data.sh` hard-fails if any output still exceeds
65534 samples — so this cannot silently regress when the rate or the trim is
changed.

**Why the data and not the code.** The limit is a property of the asset. A
runtime resampler would burn CPU on every load to produce exactly these bytes,
and a runtime *check* would only turn a silent click into a log line. These
are wind and scrape noise with no melodic content, so a shorter loop and a
lower rate are not audible as a change through the Dreamcast's audio path.

**Unverified:** that claim is a judgement about noise assets, not a listening
test. Nobody has heard this build yet.

## Music is not touched

The soundtrack is five Impulse Tracker modules, 424 KB total, decoded by
`libmodplug` into a `snd_stream` rather than uploaded to sound RAM, so the
AICA limit does not apply to them. See `kb/design-audio.md`.

## Size

151 files. 12 MB upstream, 10 MB staged, on a ~700 MB disc. There is no
packing pressure and no reason to compress or repack anything.
