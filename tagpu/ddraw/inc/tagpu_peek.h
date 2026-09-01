#ifndef TAGPU_PEEK_H
#define TAGPU_PEEK_H

/* tagpu_peek — read game memory on demand, from inside the process.

   The cheapest way to answer "did that command-line switch write the global I
   think it writes?": name the address, launch with and without the switch, read
   the value. No debugger, no guessing, and the answer is a line in tagpu.log.

   OFF BY DEFAULT and one-shot: nothing happens until `tagpu_peek.trigger` shows
   up next to the exe. It is read, evaluated and deleted, so a peek is a
   measurement rather than a running cost. Probed every 15th frame like the
   other triggers.

   Trigger file: one spec per line, blank lines and `#` comments ignored.

       <spec> := ['*'] <hex> ('+' <hex>)* [':' <size>]
       <size> := 1 | 2 | 4        (unsigned, logged decimal + hex; default 4)
               | s<N>             (N bytes as text, N <= 256)
               | x<N>             (N bytes as hex, N <= 256)

   A leading `*` dereferences the first term as a dword before the offsets are
   added, which is how every TAdynmemStruct field is reached:

       *0x511DE8+0x2C74:2     the battleroom rule word
       0x511DE0               the -f flag
       *0x511DE8+0x37F31      the -t timeout

   Every read is VirtualQuery-guarded; an unmapped address logs `<unreadable>`
   rather than taking the game down. */

void tagpu_peek_frame(unsigned int frame_counter);

#endif
