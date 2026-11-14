#ifndef TAGPU_SKIPSTUB_H
#define TAGPU_SKIPSTUB_H
/* tagpu_skipstub.h -- the bytes of a skippable before-call stub, apart from the memory they go in,
   so that tools/replay/skipstub_test.c can hold them to the literal on the host.

   The stub is entered by a site's `call engine` whose callee is stdcall with `retn` bytes of
   arguments. It runs `fn` with every register and the flags kept, then tests the byte at `skip`:
   0 jumps to `engine`, which returns to the site as the call would; anything else returns past it
   with the callee's own `ret retn`, so the site goes on as after the callee's return (the
   registers the callee keeps are kept, the ones it clobbers hold what the site held).

     9C              pushfd
     60              pushad
     E8 <fn>         call fn
     61              popad
     9D              popfd
     80 3D <skip> 00 cmp byte [skip], 0
     75 05           jne +5            (to the ret)
     E9 <engine>     jmp engine
     C2 <retn>       ret retn

   TAGPU_SKIPSTUB_BYTES of them. Engine-free: the addresses are the caller's. */

#define TAGPU_SKIPSTUB_BYTES 26

/* write the stub into `p`, whose first byte will execute at the address `at`; returns the bytes
   written. A rel32 is its target less the address of the byte after it. */
static __inline unsigned tagpu_skipstub_emit(unsigned char* p, unsigned at, unsigned fn,
                                             unsigned skip, unsigned engine,
                                             unsigned short retn)
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
