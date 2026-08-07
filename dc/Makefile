# =============================================================================
# dc/Makefile — the Dreamcast build. Runs INSIDE the tuxracer-dc:sdk container.
# =============================================================================
# The image's dc-env entrypoint has already exported KOS_BASE, KOS_PORTS,
# KOS_CFLAGS, KOS_LDFLAGS and a PATH carrying kos-cc / kos-c++, so this file
# does no toolchain setup of its own. Never invoke it from the host.
#
#   make objs     compile every TU, do not link      <- the milestone-1 signal
#   make all      objs + link -> dc/build/tuxracer.elf
#   make clean    rm -rf dc/build
#   make count    print TU counts
#   make sources  dump the computed source list (debugging the exclusions)
#
# Knobs:
#   V=1            echo full compiler command lines
#   TR_OPT=-O0     optimisation for the game TUs (default -O2)
#   TR_DC_OPT=-O2  optimisation for dc/src (default -O2)
#   TR_DEFS=...    extra -D flags for every TU
# =============================================================================

DC_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT    := $(abspath $(DC_DIR)/..)
BUILD   := $(DC_DIR)/build
OBJDIR  := $(BUILD)/obj
TARGET  := $(BUILD)/tuxracer.elf
MAPFILE := $(BUILD)/tuxracer.map

ifeq ($(V),1)
  Q :=
else
  Q := @
endif

KCC  := kos-cc
KCXX := kos-c++

# --- Source selection --------------------------------------------------------
# src/ is upstream Tux Racer 0.61, verbatim. Two TUs are REPLACED rather than
# ported in place, because both are pure SDL 1.2 platform code with no game
# logic worth keeping:
#
#   winsys.c   -> dc/src/dc_winsys.c    (KOS maple + GLdc, no SDL)
#   joystick.c -> dc/src/dc_joystick.c  (the DC pad IS the joystick)
#
# Everything else in src/ compiles as-is against the shims in dc/include.
# Keep this list SHORT: every entry here is upstream code we have taken
# ownership of and must maintain forever.
SRC_EXCLUDE := winsys.c joystick.c

GAME_C   := $(filter-out $(addprefix $(ROOT)/src/,$(SRC_EXCLUDE)), \
                         $(wildcard $(ROOT)/src/*.c))
GAME_CXX := $(wildcard $(ROOT)/src/*.cpp)
DC_C     := $(wildcard $(DC_DIR)/src/*.c)

OBJS := $(patsubst $(ROOT)/%.c,$(OBJDIR)/%.c.o,$(GAME_C)) \
        $(patsubst $(ROOT)/%.cpp,$(OBJDIR)/%.cpp.o,$(GAME_CXX)) \
        $(patsubst $(ROOT)/%.c,$(OBJDIR)/%.c.o,$(DC_C))

DEPS := $(OBJS:.o=.d)

# --- Flags -------------------------------------------------------------------
# Include order is load-bearing. dc/include MUST come before the KOS and
# kos-ports include paths so that our shims win:
#
#   dc/include/tcl.h        shadows nothing (there is no Tcl in the SDK) but
#                           is what tuxracer.h's `#include TCL_HEADER` finds
#   dc/include/SDL_mixer.h  is what src/audio.c gets instead of real SDL_mixer
#   dc/include/sys/pwd.h &  fill gaps in KOS's newlib
#   dc/include/config.h     replaces the autoconf-generated one
#
# $KOS_CFLAGS already carries the target ABI (-m4-single, -ffunction-sections,
# -fno-builtin, the KOS include paths...), so it goes first and our flags
# override it where they disagree.
INCLUDES := -I$(DC_DIR)/include -I$(ROOT)/src -I$(KOS_PORTS)/include

TR_OPT    ?= -O2
TR_DC_OPT ?= -O2

# -DHAVE_CONFIG_H makes tuxracer.h pick up dc/include/config.h, which is where
# every HAVE_* answer for this platform lives.
#
# -fno-strict-aliasing: 2001-vintage C. The code punts float/int through unions
# and casts freely (src/alglib.c, src/image.c). GCC 15 at -O2 is entitled to
# assume that never happens. This is not a style preference, it is the
# difference between a build that works and one that miscompiles silently.
#
# -Wno-* : upstream is 25 years old and warns constantly under a modern GCC.
# Silencing the noise classes keeps real diagnostics visible. Do not add
# -w — that would hide the ones that matter.
# TR_HARNESS=1 (the default) compiles in the Flycast test protocol: the
# BEGIN/END brackets, MARK:BOOT_OK and the 1.5 Mbps SCIF baud
# (dc/src/dc_harness.c). It is on by default because every gate in
# harness/dc/ depends on it and a silent guest is untestable. TR_HARNESS=0
# for a build destined for real hardware — a coder's cable cannot sync at
# 1.5 Mbps.
TR_HARNESS ?= 1
ifeq ($(TR_HARNESS),1)
  HARNESS_DEFS := -DTR_HARNESS
else
  HARNESS_DEFS :=
endif

# -include tr_glcompat.h: the GL shim (dc/include/tr_glcompat.h, dc/src/gl_compat.c)
# must be in front of EVERY TU, not just the ones that obviously touch GL.
# It does two jobs that both require this: it declares the ~20 entry points
# GLdc does not have (glNewList, glTexGeni, glMultMatrixd, gluScaleImage...),
# and it macro-redirects a short list of entry points GLdc *does* have so the
# shim can observe them (texgen needs to see glDrawElements; the display list
# recorder needs to see glBegin/glVertex3f). A TU that misses the -include
# would still compile but would call GLdc directly and bypass the shim, which
# is exactly the kind of half-applied state that is miserable to debug.
# See the header's comment block for the full intercept list and kb/design-gl.md
# for the measured API gap table.
COMMON := $(KOS_CFLAGS) $(INCLUDES) -DHAVE_CONFIG_H -g $(HARNESS_DEFS) \
          -include tr_glcompat.h \
          -fno-strict-aliasing -fno-omit-frame-pointer \
          -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
          -Wno-unused-function -Wno-parentheses -Wno-format-truncation \
          -Wno-misleading-indentation -Wno-implicit-fallthrough \
          $(TR_DEFS)

# -fno-omit-frame-pointer is NOT optional even though it costs a register:
# without it KOS's crash handler prints "frame pointers not enabled!" and the
# stack trace in harness/dc/crash.sh is empty. A crash you cannot symbolise
# costs far more than the register (harness/dc/README.md).

# gnu89 for the game: upstream declares variables mid-block in a few places but
# also relies on pre-C99 `inline` semantics and K&R-isms elsewhere. gnu89 is
# what the autoconf build used.
CFLAGS_GAME := -std=gnu89 $(TR_OPT)
CFLAGS_DC   := -std=gnu99 $(TR_DC_OPT)
CXXFLAGS    := $(TR_OPT) -fno-exceptions -fno-rtti

# --- Flag stamp --------------------------------------------------------------
# make compares TIMESTAMPS, not compiler flags. Change TR_DEFS or TR_OPT and
# nothing rebuilds, so you get a binary assembled from objects compiled under
# DIFFERENT flag sets -- half the tree with a diagnostic switch on and half
# with it off. That is not a theoretical hazard: it cost a long debugging
# session where a build appeared to hang and the "fix" was whichever TU
# happened to be stale.
#
# So: hash the flags into a stamp file, rewrite it only when the hash changes,
# and make every object depend on it.
FLAGSIG := $(COMMON)|$(CFLAGS_GAME)|$(CFLAGS_DC)|$(CXXFLAGS)
FLAGSTAMP := $(BUILD)/flags.stamp

$(shell mkdir -p $(BUILD); \
        printf '%s' '$(FLAGSIG)' > $(BUILD)/flags.new; \
        cmp -s $(BUILD)/flags.new $(FLAGSTAMP) \
          || mv $(BUILD)/flags.new $(FLAGSTAMP); \
        rm -f $(BUILD)/flags.new)

# --- Rules -------------------------------------------------------------------
.PHONY: all objs clean count sources

all: $(TARGET)

objs: $(OBJS)
	@echo "-- objs: $(words $(OBJS)) TUs compiled, no link."

$(OBJDIR)/src/%.c.o: $(ROOT)/src/%.c $(FLAGSTAMP)
	@echo "  CC   src/$*.c"
	$(Q)mkdir -p $(@D) && $(KCC) $(COMMON) $(CFLAGS_GAME) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@

$(OBJDIR)/dc/src/%.c.o: $(DC_DIR)/src/%.c $(FLAGSTAMP)
	@echo "  CC   dc/src/$*.c"
	$(Q)mkdir -p $(@D) && $(KCC) $(COMMON) $(CFLAGS_DC) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@

$(OBJDIR)/src/%.cpp.o: $(ROOT)/src/%.cpp $(FLAGSTAMP)
	@echo "  CXX  src/$*.cpp"
	$(Q)mkdir -p $(@D) && $(KCXX) $(COMMON) $(CXXFLAGS) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@

# Link with kos-c++ (not kos-cc): three TUs are C++ and the link needs
# -lstdc++, which kos-c++ appends along with the ldscript, -nostdlib and
# --start-group -lkallisti -lm -lc -lgcc --end-group.
#
# Library order matters: -lGLU calls into -lGL, and libmodplug is C++ so it
# must precede the implicit -lstdc++ that kos-c++ adds at the end.
$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	@echo "  LD   $@"
	$(Q)$(KCXX) -o $@ $(OBJS) -lGLU -lGL -lmodplug -ljim -lz -lm \
	    -Wl,-Map,$(MAPFILE)
	@sh-elf-size $@ || true

clean:
	rm -rf $(BUILD)

count:
	@echo "game .c   : $(words $(GAME_C))  (upstream src/, minus $(words $(SRC_EXCLUDE)) replaced)"
	@echo "game .cpp : $(words $(GAME_CXX))"
	@echo "dc/src .c : $(words $(DC_C))"
	@echo "TOTAL TUs : $(words $(OBJS))"

sources:
	@printf '%s\n' $(GAME_C) $(GAME_CXX) $(DC_C)

-include $(DEPS)
