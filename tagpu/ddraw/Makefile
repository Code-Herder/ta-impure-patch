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
LIBS      = -lgdi32 -lwinmm -lole32 -lmsimg32 -lavifil32 -luuid

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
# ddraw-stocklimits.dll, which raises no engine limit, for the comparisons that prove
# the raised build equal to stock below the stock caps. Its objects carry their own
# suffix, so neither build can ever link the other's -- the two differ in a macro,
# not in a file, and make would otherwise call a stale object up to date.
ifeq ($(LIMITS),stock)
TARGET   := ddraw-stocklimits.dll
CPPFLAGS += -DTAGPU_LIMITS_STOCK
OBJEXT   := .stock.o
else
OBJEXT   := .o
endif

SRCS     := $(wildcard src/*.c) $(wildcard src/*/*.c) res.rc
OBJS     := $(addsuffix $(OBJEXT), $(basename $(SRCS)))
DEPS     := $(filter %.d, $(OBJS:.o=.d))
# `clean` removes both builds' objects, whichever LIMITS it is run with.
ALLOBJS  := $(foreach e, .o .stock.o, $(addsuffix $(e), $(basename $(SRCS))))

.PHONY: clean all thread-split spirv log-check logtest
all: $(TARGET)

%.o: %.rc
	$(WINDRES) -J rc $< $@ || windres -J rc $< $@

%.stock.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.stock.o: %.rc
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

$(TARGET): $(OBJS) | thread-split spirv log-check
	$(CC) $(LDFLAGS) -o $@ $(OBJS) exports.def $(LIBS)

# THE SINK'S STRESS TEST (tests/logtest.c, driven by tests/logtest.py under Wine): the
# real tagpu_log.c with caps small enough that rotation and eviction happen in seconds.
# Not part of `all` or of CI -- it needs Wine to run.
logtest: tests/logtest.exe

tests/logtest.exe: tests/logtest.c src/tagpu_log.c inc/tagpu_log.h
	$(CC) -Iinc -O2 -Wall -std=c99 -DTLOG_FILE_CAP=65536 -DTLOG_TOTAL_CAP=524288 -DTLOG_KEEP=10 -DTLOG_SELFTEST \
	    -static -o $@ tests/logtest.c src/tagpu_log.c

clean:
	$(RM) $(TARGET) $(ALLOBJS) $(ALLOBJS:.o=.d) tests/logtest.exe || del $(TARGET) $(subst /,\\,$(ALLOBJS))

-include $(DEPS)
