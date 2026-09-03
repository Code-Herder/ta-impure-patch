# tagpu.dll — GPU overlay/renderer companion (32-bit, mingw i686)
CC      = i686-w64-mingw32-gcc
CFLAGS  = -Iddraw/inc -O2 -Wall -std=c99 -march=i486
LDFLAGS = -shared -static -s
LIBS    = -lopengl32 -lgdi32

# Header deps, for the same reason ddraw/Makefile carries them: the two headers
# named by hand here are not the only ones src/tagpu.c includes, so a change to
# any other one would leave this DLL built against stale declarations.
CPPFLAGS += -MMD -MP

tagpu.dll: src/tagpu.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/tagpu.c $(LDFLAGS) $(LIBS)

clean:
	rm -f tagpu.dll tagpu.d

-include tagpu.d
