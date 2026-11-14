#ifndef TAGPU_SKIPSTUB_H
#define TAGPU_SKIPSTUB_H
/* tagpu_skipstub.h -- the bytes of a skippable before-call stub and of a skippable call, apart from
   the memory they go in, so that tools/replay/skipstub_test.c can hold them to the literal on the
   host.

   The stub is entered by a site's `call engine`. It runs `fn` with every register and the flags
   kept, then tests the byte at `skip`: 0 jumps to `engine`, anything else jumps to `alt`. Either
   way the site's return address and arguments are on the stack as the call left them, so `alt`
   takes the callee's place: it must take the same arguments, pop them as the callee does and keep
   the registers the callee keeps.

     9C              pushfd
     60              pushad
     E8 <fn>         call fn
     61              popad
     9D              popfd
     80 3D <skip> 00 cmp byte [skip], 0
     75 05           jne +5            (to the second jmp)
     E9 <engine>     jmp engine
     E9 <alt>        jmp alt

   TAGPU_SKIPSTUB_BYTES of them. Engine-free: the addresses are the caller's. */

#define TAGPU_SKIPSTUB_BYTES 28

/* write the stub into `p`, whose first byte will execute at the address `at`; returns the bytes
   written. A rel32 is its target less the address of the byte after it. */
static __inline unsigned tagpu_skipstub_emit(unsigned char* p, unsigned at, unsigned fn,
                                             unsigned skip, unsigned engine, unsigned alt)
{
    unsigned n = 0, rel;
    p[n++] = 0x9C;                                          /* pushfd */
    p[n++] = 0x60;                                          /* pushad */
    p[n++] = 0xE8;                                          /* call fn */
    rel = fn - (at + n + 4);
    p[n++] = (unsigned char)rel; p[n++] = (unsigned char)(rel >> 8);
    p[n++] = (unsigned char)(rel >> 16); p[n++] = (unsigned char)(rel >> 24);
    p[n++] = 0x61;                                          /* popad */
    p[n++] = 0x9D;                                          /* popfd */
    p[n++] = 0x80; p[n++] = 0x3D;                           /* cmp byte [skip], 0 */
    p[n++] = (unsigned char)skip; p[n++] = (unsigned char)(skip >> 8);
    p[n++] = (unsigned char)(skip >> 16); p[n++] = (unsigned char)(skip >> 24);
    p[n++] = 0x00;
    p[n++] = 0x75; p[n++] = 0x05;                           /* jne +5: past the first jmp */
    p[n++] = 0xE9;                                          /* jmp engine */
    rel = engine - (at + n + 4);
    p[n++] = (unsigned char)rel; p[n++] = (unsigned char)(rel >> 8);
    p[n++] = (unsigned char)(rel >> 16); p[n++] = (unsigned char)(rel >> 24);
    p[n++] = 0xE9;                                          /* jmp alt */
    rel = alt - (at + n + 4);
    p[n++] = (unsigned char)rel; p[n++] = (unsigned char)(rel >> 8);
    p[n++] = (unsigned char)(rel >> 16); p[n++] = (unsigned char)(rel >> 24);
    return n;
}

/* A skippable call: entered by a site's `call engine` whose callee is stdcall (or thiscall) with
   `retn` bytes of arguments, it jumps to `engine` while the byte at `skip` is 0 and otherwise
   returns past it with the callee's own `ret retn`, every register the callee keeps kept.

     80 3D <skip> 00 cmp byte [skip], 0
     75 05           jne +5            (to the ret)
     E9 <engine>     jmp engine
     C2 <retn>       ret retn

   TAGPU_SKIPCALL_BYTES of them. */

#define TAGPU_SKIPCALL_BYTES 17

static __inline unsigned tagpu_skipcall_emit(unsigned char* p, unsigned at, unsigned skip,
                                             unsigned engine, unsigned short retn)
{
    unsigned n = 0, rel;
    p[n++] = 0x80; p[n++] = 0x3D;                           /* cmp byte [skip], 0 */
    p[n++] = (unsigned char)skip; p[n++] = (unsigned char)(skip >> 8);
    p[n++] = (unsigned char)(skip >> 16); p[n++] = (unsigned char)(skip >> 24);
    p[n++] = 0x00;
    p[n++] = 0x75; p[n++] = 0x05;                           /* jne +5: past the jmp */
    p[n++] = 0xE9;                                          /* jmp engine */
    rel = engine - (at + n + 4);
    p[n++] = (unsigned char)rel; p[n++] = (unsigned char)(rel >> 8);
    p[n++] = (unsigned char)(rel >> 16); p[n++] = (unsigned char)(rel >> 24);
    p[n++] = 0xC2;                                          /* ret retn */
    p[n++] = (unsigned char)retn; p[n++] = (unsigned char)(retn >> 8);
    return n;
}

#endif
