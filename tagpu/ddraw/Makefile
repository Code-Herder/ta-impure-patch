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
            src/tagpu_recorder.c src/tagpu_recwriter.c src/tagpu_recfile.c src/tagpu_recreader.c src/tagpu_replay.c
$(addsuffix $(OBJEXT), $(basename $(COVERED))): CFLAGS += -finstrument-functions -finstrument-functions-once -ffunction-sections
endif

SRCS     := $(wildcard src/*.c) $(wildcard src/*/*.c) res.rc
OBJS     := $(addsuffix $(OBJEXT), $(basename $(SRCS)))
DEPS     := $(filter %.d, $(OBJS:.o=.d))
# `clean` removes every build's objects, whichever LIMITS or COVER it is run with.
ALLOBJS  := $(foreach e, .o .stock.o .cover.o .stock.cover.o, $(addsuffix $(e), $(basename $(SRCS))))

.PHONY: clean all thread-split spirv log-check replay-layout logtest arttest windowtest
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

# THE REPLAY LAYOUT AND GLOBALS (tools/replay-layout-gen.py, tools/replay-globals-gen.py):
# replay-layout.tsv lists the bytes of the engine records the demo recorder does not record as
# values, replay-globals.tsv the fields of the main block and .data it records, and
# inc/tagpu_replay_layout.h and inc/tagpu_replay_globals.h are generated from them and committed.
# The build re-derives each header and fails when it and its file disagree, so a loosening is
# always a visible diff of a file. Same placement as the rules above.
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

src/tagpu_art.o src/tagpu_art.stock.o tests/arttest.exe: inc/tagpu_art_gen.h

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
$(TARGET): $(OBJS) thread-split.allow ../../tools/sim-state-check.sh ../../tools/replay/fixtures.json ../../tools/replay/tafeat.py | thread-split spirv log-check replay-layout
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

clean:
	$(RM) ddraw.dll ddraw-stocklimits.dll ddraw-cover.dll ddraw-cover.map ddraw-stocklimits-cover.dll ddraw-stocklimits-cover.map $(ALLOBJS) $(ALLOBJS:.o=.d) tests/logtest.exe tests/arttest.exe tests/windowtest.exe inc/tagpu_art_gen.h || del ddraw.dll ddraw-stocklimits.dll $(subst /,\\,$(ALLOBJS))

-include $(DEPS)
