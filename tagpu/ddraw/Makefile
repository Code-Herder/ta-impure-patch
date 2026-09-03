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
# This is not hypothetical. G13d (0e6b6b7) inserted four ints (evpL/evpT/evw/evh)
# into the MIDDLE of TAGPU_FXVIEW; tagpu_sfx.o had been built hours earlier and
# was never rebuilt, so tagpu_sfx_gather went on reading fogGrid 16 bytes early —
# where evpT now lives. At zoom < 1 that field is a small negative int, the
# `!v->fogGrid` test passes because it is non-zero, and the render thread
# dereferences it: a hard crash that froze the game with the process still up.
# It survived for a day because the mis-read gate (fogMode <- evpL) is only ODD
# at some zoom levels and is EVEN at 1x, so it looked intermittent and
# zoom-dependent rather than like a build fault.
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

SRCS     := $(wildcard src/*.c) $(wildcard src/*/*.c) res.rc
OBJS     := $(addsuffix .o, $(basename $(SRCS)))
DEPS     := $(filter %.d, $(OBJS:.o=.d))

.PHONY: clean all
all: $(TARGET)

%.o: %.rc
	$(WINDRES) -J rc $< $@ || windres -J rc $< $@

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ exports.def $(LIBS)

clean:
	$(RM) $(TARGET) $(OBJS) $(DEPS) || del $(TARGET) $(subst /,\\,$(OBJS))

-include $(DEPS)
