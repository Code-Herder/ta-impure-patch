-include config.mk

TARGET   ?= ddraw.dll

LDFLAGS  ?= -Wl,--enable-stdcall-fixup -s -static -shared
CFLAGS   ?= -Iinc -O2 -Wall -std=c99

# HEADER DEPENDENCY TRACKING — not an optimisation, a correctness requirement.
#
# Without it `make` rebuilds only the .c files that changed, so a header change
# leaves every object that includes it compiled against the OLD declarations —
# and objects that disagree about a struct's layout link without a murmur.
#
# A field inserted into the MIDDLE of a shared struct shifts every field after
# it, and a stale object keeps reading the old offsets: a pointer read 16 bytes
# early lands on a small int, passes a non-NULL test, and the render thread
# dereferences it — a hard crash with the process still up. The mis-read values
# are wrong only at some settings, so the fault looks intermittent and
# setting-dependent rather than like a build fault.
#
# -MMD writes a .d per object listing the headers it used; -MP adds phony targets
# so a DELETED header does not wedge the build. Both are appended to CPPFLAGS,
# which the built-in %.o: %.c rule passes through, and CPPFLAGS is used rather
# than CFLAGS because CFLAGS is `?=` and a caller override would silently drop it.
CPPFLAGS += -MMD -MP
LIBS      = -lgdi32 -lwinmm -lole32 -lmsimg32 -lavifil32 -luuid -lcomctl32

COMMIT   := $(shell git describe --match=NeVeRmAtCh --always --dirty || echo UNKNOWN)
BRANCH   := $(shell git rev-parse --abbrev-ref HEAD || echo UNKNOWN)

HASH     := \#
ECHOTEST := $(shell echo \"\")
ifeq ($(ECHOTEST),\"\")
	# Windows
	ECOMMIT  := $(shell echo $(HASH)define GIT_COMMIT "$(COMMIT)" > inc/git.h)
	EBRANCH  := $(shell echo $(HASH)define GIT_BRANCH "$(BRANCH)" >> inc/git.h)
else
	# Either *nix or Windows with BusyBox (e.g. w64devkit)
	ECOMMIT  := $(shell echo "$(HASH)define GIT_COMMIT" \"$(COMMIT)\" > inc/git.h)
	EBRANCH  := $(shell echo "$(HASH)define GIT_BRANCH" \"$(BRANCH)\" >> inc/git.h)
endif

ifdef DEBUG
	CFLAGS   += -D _DEBUG -D _DEBUG_X
endif

ifdef _WIN32_WINNT
	CFLAGS   += -march=i486 -D _WIN32_WINNT=$(_WIN32_WINNT)
endif

CC        = i686-w64-mingw32-gcc
WINDRES  ?= i686-w64-mingw32-windres

# THE STOCK-LIMITS BUILD (inc/tagpu_limits.h): `make LIMITS=stock` builds
# ddraw-stocklimits.dll, which raises no engine limit, for comparisons with stock. It
# changes only the engine's limits: the renderer's own budgets are the raised build's in
# both, which draw more than stock ever asks for. Its objects carry their own
# suffix, so neither build can ever link the other's -- the two differ in a macro,
# not in a file, and make would otherwise call a stale object up to date.
#
# THE COVERAGE BUILD (inc/tagpu_cover.h): `make COVER=1` builds ddraw-cover.dll for the replay
# harness. The simulation files of thread-split.allow and the demo recorder's own are compiled
# with -finstrument-functions-once, each function in a section of its own: a function calls the
# hook the first time it runs and tests one byte after that, which keeps the games at speed
# (with every call and return hooked, the forest fixtures' live games missed their deadlines).
# The DLL is linked unstripped with a map, ddraw-cover.map, which turns an address that ran into
# its function. No instrumented file holds a naked function or inline assembly, which the hook's
# call on entry would break. Its objects carry their own suffix, as the stock-limits build's do, and
# `make LIMITS=stock COVER=1` is the stock-limits build's coverage twin.
ifeq ($(LIMITS),stock)
TARGET   := ddraw-stocklimits.dll
CPPFLAGS += -DTAGPU_LIMITS_STOCK
OBJEXT   := .stock.o
else
OBJEXT   := .o
endif
ifeq ($(COVER),1)
TARGET   := $(basename $(TARGET))-cover.dll
CPPFLAGS += -DTAGPU_COVER
OBJEXT   := $(basename $(OBJEXT)).cover.o
LDFLAGS  := -Wl,--enable-stdcall-fixup -static -shared -Wl,-Map=$(basename $(TARGET)).map
COVERED  := $(shell awk '$$1 !~ /^\#/ && $$3 == "sim" {print $$1}' thread-split.allow) \
            src/tagpu_recorder.c src/tagpu_recwriter.c src/tagpu_recfile.c src/tagpu_recreader.c src/tagpu_replay.c \
            src/tagpu_reccodec.c src/tagpu_reclossy.c $(wildcard src/tagpu_recframe_*.c)
$(addsuffix $(OBJEXT), $(basename $(COVERED))): CFLAGS += -finstrument-functions -finstrument-functions-once -ffunction-sections
endif

SRCS     := $(wildcard src/*.c) $(wildcard src/*/*.c) res.rc
OBJS     := $(addsuffix $(OBJEXT), $(basename $(SRCS)))

# THE WIKI'S SCENE (src/tagpu_scene.h): the seven files that draw one world scene are compiled a
# second time, with -DTAGPU_SCENE_WIKI, into the in-game wiki's stage -- src/<name>.wiki.o beside
# the game's src/<name>.o, each in every build's suffix -- and both copies are linked. The two
# share no static, which C's linkage guarantees; the link fails on a symbol tagpu_scene.h did not
# rename, and scene-check below on a call from a wiki copy into a module only the game has.
SCENE_SRCS := src/tagpu_vk_world.c src/tagpu_vk_terr.c src/tagpu_vk_feat.c src/tagpu_vk_unit.c \
              src/tagpu_posedraw.c src/tagpu_posebake.c src/tagpu_render3do.c
SCENE_OBJS := $(addsuffix .wiki$(OBJEXT), $(basename $(SCENE_SRCS)))
OBJS     += $(SCENE_OBJS)

DEPS     := $(filter %.d, $(OBJS:.o=.d))
# Every build's object suffix: a rule that names one object names it in all four builds.
# `clean` removes every build's objects, whichever LIMITS or COVER it is run with.
ALLEXTS  := .o .stock.o .cover.o .stock.cover.o
ALLOBJS  := $(foreach e, $(ALLEXTS), $(addsuffix $(e), $(basename $(SRCS))) \
                                     $(addsuffix .wiki$(e), $(basename $(SCENE_SRCS))))

.PHONY: clean all thread-split spirv log-check replay-layout scene-check logtest arttest windowtest vklifetime codectest
all: $(TARGET)

%.o: %.rc
	$(WINDRES) -J rc $< $@ || windres -J rc $< $@

%.stock.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.stock.o: %.rc
	$(WINDRES) -J rc $< $@ || windres -J rc $< $@

%.cover.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.cover.o: %.rc
	$(WINDRES) -J rc $< $@ || windres -J rc $< $@

%.stock.cover.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.stock.cover.o: %.rc
	$(WINDRES) -J rc $< $@ || windres -J rc $< $@

$(SCENE_OBJS): src/%.wiki$(OBJEXT): src/%.c
	$(CC) $(CPPFLAGS) -DTAGPU_SCENE_WIKI $(CFLAGS) -c -o $@ $<

# THE THREAD-SPLIT RULE (frame-packet-exchange §10): a source file that is
# not on thread-split.allow may not name an engine address, include tagpu_engine.h,
# probe with IsBad*Ptr or add an offset to the main pointer. Checked before the link
# so `make` here and the CI job (.github/workflows/build.yml runs exactly this
# target) both fail on an offender; the upstream build.cmd / vcxproj do not run it.
# The list is default-deny and only shrinks — see the file's header.
thread-split:
	../../tools/thread-split-check.sh .

# THE SHADER RULE (roadmap.md Phase G): inc/spirv/ is GENERATED from the GLSL string
# literals in src/*.c by tools/spirv-gen.py and committed as text, so no build
# needs a GLSL compiler. Generated code that is committed rots, so the build
# re-derives each shader's hash -- the C preprocessor and python3, never glslang
# -- and fails if a shader has been edited without regenerating. Same placement
# and same reasoning as the thread-split rule above; the upstream build.cmd /
# vcxproj run neither.
#
# IT NEEDS python3 (and the cross compiler, which the build has by definition).
# That makes python3 a build dependency of this Makefile, which is why the CI
# job names it explicitly instead of relying on the runner image having it.
spirv:
	../../tools/spirv-check.sh .

# THE LOG RULE: only tagpu_log.c opens a log file, because its caps hold only while it
# is the one writer (inc/tagpu_log.h). Same placement as the two rules above.
log-check:
	../../tools/log-check.sh .

# THE SCENE RULE (src/tagpu_scene.h): no wiki copy of a world pass may call into a module only the
# game's world has. tools/scene-check.sh reads every wiki object's undefined symbols and fails on
# one that another object of ours defines and its allow list does not name. It reads objects, so
# it depends on every object of the link and the link waits for it: it cannot race the compiler.
scene-check: $(OBJS)
	../../tools/scene-check.sh $(SCENE_OBJS) -- $(filter-out $(SCENE_OBJS), $(OBJS))

# THE REPLAY LAYOUT, GLOBALS, LANES AND HOLD TABLE (tools/replay-layout-gen.py,
# tools/replay-globals-gen.py, tools/replay-lanes-gen.py, tools/replay-hold-gen.py):
# replay-layout.tsv lists the bytes of the engine records the demo recorder does not record as
# values, replay-globals.tsv the fields of the main block and .data it records, replay-lanes.tsv
# how the unit codec cuts a unit image into lanes, replay-hold.tsv the bytes a skirmish stream
# holds between refreshes, and inc/tagpu_replay_layout.h, inc/tagpu_replay_globals.h,
# inc/tagpu_reclanes.h and inc/tagpu_rechold.h are generated from them and committed. The build
# re-derives each header and fails when it and its file disagree, so a loosening is always a
# visible diff of a file. Same placement as the rules above.
replay-layout:
	../../tools/replay-layout-check.sh .

# THE ART RULE (tools/artbake.py; src/tagpu_art.h): Impure's own UI art is committed as PNGs
# under art/ui/ and baked into palette indices here, into inc/tagpu_art_gen.h, which is
# generated and never committed -- so the art in the DLL cannot disagree with the art in
# git. The bake needs only the bare python3 the build already names. The folders are
# prerequisites as well as the files, because a deleted PNG changes only its folder's time.
ART_DIR  := ../../art/ui
ART_SRCS := $(shell find $(ART_DIR) -type d -o -name '*.png' 2>/dev/null)
inc/tagpu_art_gen.h: ../../tools/artbake.py ../../unditherer/ta-palette.json $(ART_SRCS)
	python3 ../../tools/artbake.py $(ART_DIR) ../../unditherer/ta-palette.json $@

$(addprefix src/tagpu_art, $(ALLEXTS)) tests/arttest.exe: inc/tagpu_art_gen.h

# THE HUD ARCHIVE (tools/hudpak.py; src/tagpu_hudart.h): the replay HUD's files under art/hud/,
# packed as they are -- JSON, PNG, TrueType -- into inc/tagpu_hudpak.bin, which res.rc embeds as
# the RCDATA resource HUDPAK. Generated and never committed, like the art above, and for the same
# reason: the HUD in the DLL cannot disagree with the HUD in git, or with the mockup that reads it.
HUD_DIR  := ../../art/hud
HUD_SRCS := $(shell find $(HUD_DIR) -type d -o -type f 2>/dev/null)
inc/tagpu_hudpak.bin: ../../tools/hudpak.py $(HUD_SRCS)
	python3 ../../tools/hudpak.py $(HUD_DIR) $@

$(addprefix res, $(ALLEXTS)): inc/tagpu_hudpak.bin

# THE WIKI ARCHIVE (tools/hudpak.py --set wiki; src/tagpu_wikidata.h, src/tagpu_hudart.h): the
# in-game wiki's tree, entries and fitted window kit under art/wiki/, packed by the same tool in the
# same format into inc/tagpu_wikipak.bin, which res.rc embeds as the RCDATA resource WIKIPAK.
# Generated and never committed, as the HUD's.
WIKI_DIR  := ../../art/wiki
WIKI_SRCS := $(shell find $(WIKI_DIR) -type d -o -type f 2>/dev/null)
inc/tagpu_wikipak.bin: ../../tools/hudpak.py $(WIKI_SRCS)
	python3 ../../tools/hudpak.py --set wiki $(WIKI_DIR) $@

$(addprefix res, $(ALLEXTS)): inc/tagpu_wikipak.bin

# THE WIKI'S SIMULATION (src/tagpu_wikisim.h) agrees with the mockup's sim.js bit for bit, which
# needs every double rounded once per operation, as JavaScript rounds it. The x87 unit this build
# otherwise computes on keeps 80-bit intermediates (MEASURED 2026-10-25: built for x87, 1196 of the
# 9298 frames of the demos differ from sim.js in a last bit; built so, none), so the simulation,
# and the loader that normalises the scenes' facings for it (src/tagpu_wikidata.h), compute on
# SSE2 instead (MEASURED 2026-10-26, the loader's x / sqrt(x*x + z*z) over 3720 facings: built for
# x87, 1804 differ from sim.js in a bit; built so, none). The reclaim sweep's split
# (src/tagpu_sweepgeo.h), which the wiki's sim calls as the game's sweep does, sums its loads in
# double, so it computes on SSE2 with them. The units' scripts' host
# (src/tagpu_wikicob.h) answers GET's angles and distances in double, and the mockup asks the same C
# built natively (tools/wiki/serve.py), so it computes on SSE2 too. The game's threads align the
# stack to 4 bytes only, so they realign where SSE needs more. tools/wiki/simparity.mjs --wine
# checks the build under Wine.
$(addprefix src/tagpu_wikisim, $(ALLEXTS)) $(addprefix src/tagpu_wikidata, $(ALLEXTS)) \
$(addprefix src/tagpu_wikicob, $(ALLEXTS)) $(addprefix src/tagpu_sweepgeo, $(ALLEXTS)): \
    CFLAGS += -msse2 -mfpmath=sse -mincoming-stack-boundary=2

# THE WIKI'S STAGE (src/tagpu_wikistage.h) computes its pixels in double by the mockup's stage.js
# arithmetic, and floors and snaps some of them, so it computes on SSE2 for the same reason: an
# 80-bit intermediate can carry a value across a pixel edge stage.js does not cross.
# tools/wiki/stagecheck.mjs checks a native build of it.
$(addprefix src/tagpu_wikistage, $(ALLEXTS)): CFLAGS += -msse2 -mfpmath=sse -mincoming-stack-boundary=2

# THE WIKI'S SCENE (src/tagpu_wikiscene.h) stages a nanoframe by truncating a single-precision
# product (tagpu_nanostage.h, `nano * 255.0f`) as the mockup's scene.js truncates Math.fround's,
# so it computes on SSE2 too: an 80-bit product can land on the other side of a whole number.
# tools/wiki/scenecheck.mjs checks a native build of it.
$(addprefix src/tagpu_wikiscene, $(ALLEXTS)): CFLAGS += -msse2 -mfpmath=sse -mincoming-stack-boundary=2

# THE RESTORER'S MODELS (src/tagpu_restore_core.c load_weights): unditherer/models/<name>.w32.bin,
# compressed by zstd into inc/tagpu_rmodel_<name>.zst, which res.rc embeds as RMODEL_<NAME>.
# Generated and never committed, like the HUD archive. The frame records its content size, which
# the loader bounds before it decompresses.
RMODELS := full tiny
inc/tagpu_rmodel_%.zst: ../../unditherer/models/%.w32.bin
	zstd -q -19 -f $< -o $@

$(addprefix res, $(ALLEXTS)): $(RMODELS:%=inc/tagpu_rmodel_%.zst)

# THE SIMULATION-STATE RULE (inc/tagpu_simreg.h): every writable static of a file marked `sim`
# in thread-split.allow is declared, and no `sim` object allocates, takes a TLS index, changes a
# page or names a lever itself. It reads the objects this link uses, so it runs in the link's own
# recipe, after every object is built: as an order-only prerequisite it would race the compiler.
# A failure stops the build before the link, so the DLL on disk is still the last one that
# passed; the allow list and the script are prerequisites so that a change to either runs it
# again.
#
# THE SIMULATION FEATURES (inc/tagpu_simreg.h, SIM_FEATURE): every line of a `sim` file lies in a
# feature that names its replay fixtures, so a landing knows which fixtures to run. The check
# reads the sources, not the objects, and runs beside the state rule for the same reasons; the
# fixture list and the parser are prerequisites so that a change to either runs it again.
$(TARGET): $(OBJS) thread-split.allow ../../tools/sim-state-check.sh ../../tools/replay/fixtures.json ../../tools/replay/tafeat.py | thread-split spirv log-check replay-layout scene-check
	../../tools/sim-state-check.sh . $(OBJEXT)
	python3 ../../tools/replay/tareplay.py features --check .
	$(CC) $(LDFLAGS) -o $@ $(OBJS) exports.def $(LIBS)

# THE SINK'S STRESS TEST (tests/logtest.c, driven by tests/logtest.py under Wine): the
# real tagpu_log.c with caps small enough that rotation and eviction happen in seconds.
# Not part of `all` or of CI -- it needs Wine to run.
logtest: tests/logtest.exe

tests/logtest.exe: tests/logtest.c src/tagpu_log.c inc/tagpu_log.h
	$(CC) -Iinc -O2 -Wall -std=c99 -DTLOG_FILE_CAP=65536 -DTLOG_TOTAL_CAP=524288 -DTLOG_KEEP=10 -DTLOG_SELFTEST \
	    -static -o $@ tests/logtest.c src/tagpu_log.c

# THE ART'S SELF-TEST (tests/arttest.c, driven by tests/arttest.py under Wine): the real
# tagpu_art.c inflates the baked art and prints every frame, which arttest.py compares with
# the PNGs in art/ui/. Not part of `all` or of CI -- it needs Wine to run.
arttest: tests/arttest.exe

tests/arttest.exe: tests/arttest.c src/tagpu_art.c src/lodepng.c src/tagpu_log.c src/tagpu_art.h
	$(CC) -Iinc -Isrc -O2 -Wall -std=c99 -static -o $@ tests/arttest.c src/tagpu_art.c src/lodepng.c src/tagpu_log.c

# The real window subclass on Win32/Wine; run on an isolated display.
windowtest: tests/windowtest.exe

tests/windowtest.exe: tests/windowtest.c src/tagpu_window.c src/tagpu_window.h
	$(CC) -Iinc -Isrc -O2 -Wall -std=c99 -static -o $@ tests/windowtest.c src/tagpu_window.c -lcomctl32

vklifetime: tests/vklifetime.exe

tests/vklifetime.exe: tests/vklifetime.c src/tagpu_vk.c src/tagpu_vk.h
	$(CC) -Iinc -Isrc -O2 -Wall -std=c99 -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -Wl,--gc-sections -static -o $@ tests/vklifetime.c

# THE STREAM CODEC'S SELF-TEST (tests/codectest.c, driven by tests/codectest.py under Wine, or
# built natively with the sanitizers by `codectest.py --native`): the real tagpu_reccodec.c and its
# record frames transcode and decode recordings, the lossy filter (src/tagpu_reclossy.c) runs over
# one, its outcomes and a fuzz, against tools/replay/reccodec.py. codectest.c includes the codec's
# source, to copy a decoder's state the interface keeps opaque; the frames, whose states their own
# `copy` copies, and the filter are linked; so is the DLL's reader and what it reads a file with
# (RECREAD), for `codectest sprays` and the outcomes that seek through it, which this Windows
# build alone has: the host build (codectest.py --native) leaves them out, the reader being Win32
# file code. Only these builds define RECF_TEST_COPY, which compiles the frames' copies
# (inc/tagpu_recframe.h). Not part of `all` or of CI -- it needs Wine or a host compiler, and
# recordings.
codectest: tests/codectest.exe

RECFRAMES := $(wildcard src/tagpu_recframe_*.c)
RECREAD   := src/tagpu_recreader.c src/tagpu_recfile.c src/tagpu_recmp_explored.c \
             src/tagpu_recmp_file_state.c src/tagpu_recmp_history.c src/tagpu_recmp_named.c \
             src/tagpu_recmp_orders.c src/tagpu_recmp_players.c src/tagpu_recmp_pov.c \
             src/tagpu_recmp_roots.c src/tagpu_recmp_stream.c src/tagpu_recmp_text.c \
             src/tagpu_recmp_units.c src/crc32.c
tests/codectest.exe: tests/codectest.c src/tagpu_reccodec.c $(RECFRAMES) src/tagpu_reclossy.c $(RECREAD) inc/tagpu_reccodec.h inc/tagpu_recframe.h inc/tagpu_recfmt.h inc/tagpu_reclanes.h inc/tagpu_reclossy.h inc/tagpu_rechold.h inc/tagpu_recreader.h src/zstd.c
	$(CC) -Iinc -Isrc -DRECF_TEST_COPY -O2 -Wall -std=c99 -static -o $@ tests/codectest.c $(RECFRAMES) src/tagpu_reclossy.c $(RECREAD) src/zstd.c

clean:
	$(RM) ddraw.dll ddraw-stocklimits.dll ddraw-cover.dll ddraw-cover.map ddraw-stocklimits-cover.dll ddraw-stocklimits-cover.map $(ALLOBJS) $(ALLOBJS:.o=.d) tests/logtest.exe tests/arttest.exe tests/windowtest.exe tests/vklifetime.exe tests/codectest.exe tests/codectest-asan.exe inc/tagpu_art_gen.h $(RMODELS:%=inc/tagpu_rmodel_%.zst) || del ddraw.dll ddraw-stocklimits.dll $(subst /,\\,$(ALLOBJS))

-include $(DEPS)
