/* tagpu_factoryq.c -- Ctrl+Shift+click on a build-menu button queues 100 (tagpu_factoryq.h).

   THE CLICK. The build menus' handler 0x41AA00 ends, for a button it has not armed and the
   shown unit selected, in the factory branch (DISASSEMBLED, research/notes/engine/ui.md, the
   build menu's click):
       0x41ABFA  68 F9 00 00 00   push 0xF9              ; Shift, 0x4C1B80's id
       0x41ABFF  E8 <rel32>       call 0x4C1B80          ; the key sampler
       0x41AC04  85 C0            test eax,eax
       0x41AC06  57               push edi
       0x41AC07  74 18            je 0x41AC21            ; no Shift
       0x41AC09  E8 <rel32>       call 0x4AB6B0          ; the button: 1 left
       0x41AC0E  83 F8 01         cmp eax,1
       0x41AC11  75 04            jne 0x41AC17
       0x41AC13  6A 05            push 5                 ; Shift + left
       0x41AC15  EB 1F            jmp 0x41AC36
       0x41AC17  6A FB            push -5                ; Shift + right
       ...                                               ; 0x41AC21: no Shift, push 1 or -1
       0x41AC3C  E8 <rel32>       call 0x419B00          ; (name, unit, amount), ret 0xC
   so the amount 0x419B00 receives from this one call is 5 or -5 exactly when the engine saw
   Shift down. The bytes 0x41AC04..0x41AC3B are matched with their two calls of 0x4AB6B0
   decoded, and the Shift sampler's push and call before them (0x41ABFA, 0x41ABFF), so the
   rule "+-5 is Shift" holds for the exe the wrapper is installed on; an exe with other
   amounts (TADR rewrites the two immediates at 0x41AC14 and 0x41AC18 while Ctrl is down)
   arms nothing.

   0x419B00's other callers are the AI's (0x40865E, 0x4087C4), which keep calling it directly.
   After the call the handler reads only EBP and EDI (0x41AC41..0x41AC74), which the wrapper
   preserves as the C ABI does, and nothing reads EAX, so a stdcall function of three dwords
   stands in for it exactly.

   The count it adds to is a dword, order +0x3A, and 0x43B0B0 adds to it with no bound; a take
   off larger than a node's count empties the node and carries the rest to the last remaining
   node of the type, toward the head (research/notes/engine/recorder.md, 0x43B0B0). The
   button's "+%d" goes to its text[128] at +0xB6 (0x4199B0), which holds any int. Nothing here is state: the queue the click makes is
   the engine's own, which the recorder already carries, and on a puppet host the call acts on
   no unit, as stock's does (the controlled block is empty). */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "tagpu_factoryq.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"

#define VA_QUEUE      0x00419B00u   /* the build-menu queue call (name, unit, amount)       */
#define VA_KEYDOWN    0x004C1B80u   /* the engine's key sampler, injection-aware            */
#define VA_BUTTON     0x004AB6B0u   /* the GUI's button of the click: 1 left               */
#define VA_SHIFT_CALL 0x0041ABFFu   /* call 0x4C1B80, with 0xF9 pushed at 0x41ABFA          */
#define VA_BRANCH     0x0041AC04u   /* the matched branch, to the queue call                */
#define VA_QUEUE_CALL 0x0041AC3Cu

enum { KEY_SHIFT = 0xF9, KEY_CONTROL = 0xFA };
enum { STOCK_SHIFT_AMOUNT = 5, HUNDRED = 100 };

typedef void (__stdcall* QueueCall)(const char* name, void* unit, int amount);
typedef int (__stdcall* KeyDown)(int id);

/* Ctrl held on a Shift click makes it 100. Ctrl goes through the same sampler the engine asked
   for Shift, which answers each key's state at the call (research/notes/engine/input.md,
   KeyboardHotkeySampler), so injected input counts as a real key does. */
static void __stdcall queue_click(const char* name, void* unit, int amount)
{
    if ((amount == STOCK_SHIFT_AMOUNT || amount == -STOCK_SHIFT_AMOUNT) &&
        ((KeyDown)VA_KEYDOWN)(KEY_CONTROL))
        amount = amount > 0 ? HUNDRED : -HUNDRED;
    ((QueueCall)VA_QUEUE)(name, unit, amount);
}

static int branch_matches(void)
{
    /* 0x41AC04..0x41AC3B, the two calls of 0x4AB6B0 (0x41AC09, 0x41AC21) as E8 and their
       rel32 decoded below */
    static const unsigned char want[0x38] = {
        0x85,0xC0, 0x57, 0x74,0x18,
        0xE8,0,0,0,0, 0x83,0xF8,0x01, 0x75,0x04, 0x6A,0x05, 0xEB,0x1F,
        0x6A,0xFB, 0x8D,0x54,0x24,0x48, 0x55, 0x52, 0xEB,0x1B,
        0xE8,0,0,0,0, 0x83,0xF8,0x01, 0x75,0x09, 0x50, 0x8D,0x44,0x24,0x48, 0x55, 0x50,
        0xEB,0x08, 0x6A,0xFF, 0x8D,0x4C,0x24,0x48, 0x55, 0x51 };
    static const unsigned char push_shift[5] = {0x68, KEY_SHIFT, 0, 0, 0};
    unsigned char have[sizeof want];
    memcpy(have, (const void*)(uintptr_t)VA_BRANCH, sizeof have);
    memset(have + 6, 0, 4);                     /* 0x41AC0A, the first button call's rel32  */
    memset(have + 30, 0, 4);                    /* 0x41AC22, the second's                   */
    return !memcmp((const void*)(uintptr_t)(VA_SHIFT_CALL - 5), push_shift, sizeof push_shift) &&
           tagpu_detour_redirect_ok(VA_SHIFT_CALL, VA_KEYDOWN) &&
           !memcmp(have, want, sizeof want) &&
           tagpu_detour_redirect_ok(VA_BRANCH + 5, VA_BUTTON) &&
           tagpu_detour_redirect_ok(VA_BRANCH + 29, VA_BUTTON) &&
           tagpu_detour_redirect_ok(VA_QUEUE_CALL, VA_QUEUE);
}

void tagpu_factoryq_init(void)
{
    if (!branch_matches()) {
        tagpu_logf("factoryq: the build menu's factory branch (0x41ABFA..0x41AC3C) holds other "
                   "bytes; Ctrl+Shift stays 5");
        return;
    }
    if (!tagpu_detour_redirect(VA_QUEUE_CALL, VA_QUEUE, (void*)queue_click)) {
        tagpu_logf("factoryq: the queue call (0x41AC3C) could not be written; Ctrl+Shift stays 5");
        return;
    }
    tagpu_logf("factoryq: armed (0x41AC3C): Ctrl+Shift+click queues %d", HUNDRED);
}
