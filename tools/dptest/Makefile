# dptest -- a DirectPlay TCP/IP host/join probe, built as a 32-bit PE so it
# exercises exactly the dplayx + dpwsockx pair that Total Annihilation uses.
CC = i686-w64-mingw32-gcc
all: dptest.exe
dptest.exe: dptest.c
	$(CC) -O1 -o $@ $< -lole32 -lws2_32
clean:
	rm -f dptest.exe
	rm -rf out-*
.PHONY: all clean
