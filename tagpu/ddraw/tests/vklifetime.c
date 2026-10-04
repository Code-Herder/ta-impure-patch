/* Tests the production observer and predicate without a Vulkan device. The
   exchange wrapper deterministically schedules B's binding after A's ownership
   check but before A's destruction publication. Unused GPU code is link-discarded. */
#include <windows.h>
static LONG scheduled_exchange(volatile LONG *target, LONG value);
#undef InterlockedExchange
#define InterlockedExchange scheduled_exchange
#include "../src/tagpu_vk.c"
#undef InterlockedExchange
#define InterlockedExchange _InterlockedExchange

static int rebind;
static LONG departing;

void tagpu_logf(const char *fmt, ...) { (void)fmt; }
LONG tagpu_window_generation(HWND hwnd) { (void)hwnd; return departing; }

static LONG scheduled_exchange(volatile LONG *target, LONG value)
{
    if (target == &s_ownGone && rebind) {
        rebind = 0;
        InterlockedExchange(&s_ownerGeneration, 22);
    }
    return InterlockedExchange(target, value);
}

int main(void)
{
    s_ownWin = 1;
    s_ownerGeneration = departing = 11;
    rebind = 1;
    if (!tagpu_vk_wndproc((HWND)1, WM_DESTROY, 0, 0) || rebind ||
        s_ownGone != 11 || owner_gone()) {
        puts("FAIL: delayed A destruction claimed replacement B");
        return 1;
    }
    departing = 22;
    if (!tagpu_vk_wndproc((HWND)2, WM_DESTROY, 0, 0) || !owner_gone()) {
        puts("FAIL: B destruction did not claim B");
        return 1;
    }
    s_ownerGeneration = departing = 33;
    if (owner_gone()) {
        puts("FAIL: destruction marker survived as an unscoped veto");
        return 1;
    }
    departing = 0;
    if (tagpu_vk_wndproc((HWND)3, WM_DESTROY, 0, 0)) {
        puts("FAIL: unattached window claimed presentation");
        return 1;
    }
    puts("ok: delayed destruction names its own lifetime; new bindings remain live");
    return 0;
}
