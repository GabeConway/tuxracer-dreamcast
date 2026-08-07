# The disc, and whether the game's paths survive it — evidence

Tux Racer's data is not a flat blob. The Tcl scripts reference each other by
relative path (`source courses/course_idx.tcl`), the loader `chdir()`s into a
course directory and opens `course.tcl` by bare name (`src/course_load.c:345`),
and course assets are `elev.rgb` / `terrain.rgb` beside it. So the port depends
on lowercase, long, mixed-case paths surviving ISO9660 — a real risk, because
plain ISO9660 level 1 is 8.3 uppercase and would turn `course_idx.tcl` into
`COURSE_I.TCL;1`.

## Measurement

Built a real image from the real data and dumped the data track:

```
mkdcdisc -q -N -e selftest.elf -D /work/data -I -o /tmp/t.cdi
# -> /tmp/t.iso, 12,445,696 B
```

Scanning the identifiers in that ISO finds **both** name forms present:

```
COURSE.TCL;1   COURSE_IDX.TCL;1   COURSEINIT.TCL;1   BUNNY_HILL   COURSES
course.tcl     course_idx.tcl     courseinit.tcl     bunny_hill   courses
```

Two things follow:

1. The primary descriptor is **not** level 1 — `COURSE_IDX.TCL;1` is an 11-char
   basename, so mkdcdisc is emitting relaxed/level-2 names. Nothing is
   truncated.
2. Lowercase, exact-case names exist alongside them, i.e. a Rock Ridge or
   Joliet extension is present. That is the form the game will ask for.

## What is still unverified

**Whether KOS's iso9660 driver returns the lowercase names.** Both forms are on
the disc; which one `fs_open("/cd/courses/course_idx.tcl")` resolves against
depends on the driver's extension support, and that has not been tested from a
booted guest. It cannot be, until the game links (M2).

If it turns out KOS only exposes the uppercase 8.3-ish form, the fix is *not*
to rename the data — the `.tcl` scripts reference each other by name and would
all have to be rewritten. It is to case-fold in the file-open path, in
`dc/src/`. Do not "fix" it by mangling `data/`.

## Size

151 files, 12 MB. A CD holds ~700 MB, so there is no packing pressure at all
and no reason to compress or repack anything. `-N` (unpadded) keeps the image
at a few MB for the Flycast loop; `DC_CDI_PAD=1` produces the 740 MB form only
when a burn or a read-speed-realistic timing run needs it.
