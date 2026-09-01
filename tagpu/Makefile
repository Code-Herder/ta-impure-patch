# tagpu.dll — GPU overlay/renderer companion (32-bit, mingw i686)
CC      = i686-w64-mingw32-gcc
CFLAGS  = -Iddraw/inc -O2 -Wall -std=c99 -march=i486
LDFLAGS = -shared -static -s
LIBS    = -lopengl32 -lgdi32

tagpu.dll: src/tagpu.c ddraw/inc/tagpu.h
	$(CC) $(CFLAGS) -o $@ src/tagpu.c $(LDFLAGS) $(LIBS)

clean:
	rm -f tagpu.dll
