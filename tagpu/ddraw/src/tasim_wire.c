/* tasim_wire.c -- the scenario wire's parser (tasim_wire.h). The format and why it is private to the CLI and
   the DLL: research/notes/scenario-format.md, *Transport*.
   Fixed vocabulary, positional columns, `-` for unset. No user hand-writes the file and nothing promises it is
   stable; it changes whenever the DLL does. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_wire.h"

void tasim_wire_errs_reset(TASIM_WIRE_ERRS* errs)
{
    errs->n = errs->dropped = 0;
}

/* Errors are the product here: a scenario that creates nothing must say which line was wrong. Keep the first
   few verbatim and count the rest. */
void tasim_wire_err(TASIM_WIRE_ERRS* errs, const char* fmt, ...)
{
    va_list ap;

    if (errs->n >= TASIM_WIRE_MAX_ERRORS)
    {
        errs->dropped++;
        return;
    }

    va_start(ap, fmt);
    vsnprintf(errs->msg[errs->n], TASIM_WIRE_ERRLEN, fmt, ap);
    va_end(ap);
    errs->n++;
}

void tasim_wire_reset(TASIM_WIRE* w)
{
    w->nunits = w->nfeats = w->norders = 0;
    w->seed = 0;
    w->clear = 1;
    w->abort = 1;
    w->limit = TASIM_WIRE_UNSET;
    w->map[0] = 0;
    w->sw_set = w->sw_clr = 0;
    w->npl = 0;
    w->clearfeat = 0;
    w->objective = -1;
    w->cam_kind = TASIM_TGT_NONE;
    w->cam_a = w->cam_b = 0;
    w->cam_pin = 0;
}

char* tasim_wire_tok(char** p)
{
    char* s = *p;
    char* b;

    while (*s == ' ' || *s == '\t')
        s++;

    if (!*s)
    {
        *p = s;
        return 0;
    }

    b = s;

    while (*s && *s != ' ' && *s != '\t')
        s++;

    if (*s)
        *s++ = 0;

    *p = s;
    return b;
}

/* One integer column. `-` means "unset — use the engine's own default". */
static int num(const char* t, int* out)
{
    char* end;
    long v;

    if (!t)
        return 0;

    if (t[0] == '-' && !t[1])
    {
        *out = TASIM_WIRE_UNSET;
        return 1;
    }

    v = strtol(t, &end, 10);

    if (end == t || *end)
        return 0;

    /* strtol saturates at LONG_MIN and LONG_MAX (C99 7.20.1.4), and the DLL's long is 32 bits: a wider long is
       saturated at the DLL's bounds here, so every build reads an out-of-range column as the DLL reads it. */
    if (v > 2147483647L)
        v = 2147483647L;
    else if (v < -2147483647L - 1)
        v = -2147483647L - 1;

    *out = (int)v;
    return 1;
}

static int req(const char* t, int* out)
{
    return num(t, out) && *out != TASIM_WIRE_UNSET;
}

/* One storage column: a finite float, 0 or more. The engine's storages are floats, so the
   value is stored as the float the seat setup would store.
   The bound is below 3e38: the double nearest 3e38 lies above it (0x1.c363cbf21f28ap+127), and the DLL's
   applier, built on the x87 under C99's excess precision, held a double against the constant's long double,
   which refused that double and nothing below it. `>=` on doubles draws the same line in every build. */
static int storage_value(const char* t, float* out)
{
    char*  end;
    double v;

    if (!t || !*t)
        return 0;

    v = strtod(t, &end);

    if (end == t || *end || !(v >= 0.0) || v >= 3.0e38)
        return 0;

    *out = (float)v;
    return 1;
}

/* `after=<n>`, the ticks an order waits: 0..255 */
static int after_value(const char* t, int* out)
{
    int v;

    if (!t || strncmp(t, "after=", 6) || !req(t + 6, &v) || v < 0 || v > 255)
        return 0;

    *out = v;
    return 1;
}

/* `name=value` in the v1 / sw / player / cam lines. */
int tasim_wire_kv(char* t, char** key, int* value)
{
    char* eq;

    if (!t)
        return 0;

    eq = strchr(t, '=');

    if (!eq)
        return 0;

    *eq = 0;
    *key = t;
    return num(eq + 1, value);
}

const TASIM_WIRE_SWITCH tasim_wire_switches[] = {
    { "drop", 0x1 }, { "cheats", 0x2 }, { "selboxes", 0x4 }, { "invulnfeatures", 0x8 },
    { "noshake", 0x10 }, { "clock", 0x40 }, { "doubleshot", 0x80 }, { "halfshot", 0x100 },
    { "radar", 0x200 }, { "shootall", 0x400 }, { 0, 0 }
};

/* The command types of TA's order mapper Type2Index 0x43F0E0, which jumps on type - 1 through
   the table 0x4401EC (0x43F141..0x43F14D) [DISASSEMBLED]; the numbering is TADR's
   [TA_MemoryStructures.pas:6-21] except stop's. There is no attack-move; the CLI rejects the word and offers
   `attack` at a coordinate instead.
   `stop` is type 10, although TADR names type 1 BUTTON_ORDER_STOP. Type 1 (0x43F9E9) is the
   right click's contextual default command: with no target and the position (0,0,0) it gives a
   ground unit Move_Ground (action 26) to the map's corner (MEASURED 2026-10-27, an ARMCK, 3 of
   3). Type 10 goes to 0x43F82C, which looks up the script action "STOP" (0x502714) through
   0x438760 and tests nothing of the unit, its def or a target [DISASSEMBLED]. That is the action
   the sidebar's STOP button gives each selected unit: 0x419CE5 looks up the same string, and
   0x48CF30 hands it to ORDERS_NewMainOrder2Unit 0x43AFC0 with no target and a NULL position
   (0x48D1D9..0x48D1F4). The zero position the applier passes (tagpu_scenario.c) makes the same
   node: without Shift 0x43AFC0 hands the position only through 0x43ADC0 to the node constructor
   0x43A0C0, which
   stores (0,0,0) for a NULL one (0x43A13A..0x43A164) and clears the node's +0x42 bit 0x400
   (0x43A1A7..0x43A1B3), a bit Stop's descriptor flags (0, the static record 0x4FC490, +0x11)
   never set [DISASSEMBLED]. This is the order alone: the button's own writes stay the button's,
   which disarms an armed build (main+0x2CC3 = 1, 0x419CC2), clears main+0x2CC6 bit 3
   (0x419CCE..0x419CD8) and plays `immediateorders` through 0x47F1A0 (0x41A0EA)
   (exe-reverse-engineering.md, *STOP and ATTACK*).
   `teleport` (11) is the engine's own Teleport, which stock play never issues: Type2Index
   0x43F0E0 jumps on the type (0x43F14D) to 0x43F813, which names the script action "TELEPORT"
   for any unit with no test of the def (DISASSEMBLED), and the action's handler 0x406AA0 moves
   every other unit inside the box the ordering unit's def gives about it (+0x15E..+0x172) to
   the order's position, offset kept (scenario-format.md, *Order vocabulary*). */
static const struct { const char* name; int id; } ORDERTYPES[] = {
    { "stop", 10 }, { "move", 2 }, { "attack", 3 }, { "blast", 4 }, { "unload", 5 },
    { "load", 6 }, { "defend", 7 }, { "repair", 8 }, { "patrol", 9 }, { "teleport", 11 },
    { "reclaim", 12 }, { "capture", 13 }, { "mobilebuild", 14 }, { "buildweapon", TASIM_CMD_BUILDWEAPON },
    { 0, 0 }
};

/* The stance column is a token, never a raw mask: (UnitStateMask & 0xC0000) >> 18
   is 0 hold / 1 manoeuvre / 2 roam [VERIFIED, TADR dialog.cpp:409-417]. */
static const char* STANCES[] = { "hold", "manoeuvre", "roam", 0 };

/* The fire column, also a token: (UnitStateMask & 0x300000) >> 20, the field Standing_FireOrder 0x403100 stores
   (0x403123), is 0 hold fire / 1 return fire / 2 fire at will, the three the UI's buttons give (0x41A614,
   0x41A64D, 0x41A686) [DISASSEMBLED, engine/units.md]. No stock writer but the def keys makes 3. */
static const char* FIRES[] = { "hold", "return", "at_will", 0 };

static int token_of(const char* t, const char* const* names, int* out)
{
    int i;

    if (!t)
        return 0;

    if (t[0] == '-' && !t[1])
    {
        *out = TASIM_WIRE_UNSET;
        return 1;
    }

    for (i = 0; names[i]; i++)
        if (!strcmp(t, names[i]))
        {
            *out = i;
            return 1;
        }

    return 0;
}

/* `-` or a whole number 0..TASIM_WIRE_STOCK_MAX */
static int stock_value(const char* t, int* out)
{
    return num(t, out) && (*out == TASIM_WIRE_UNSET || (*out >= 0 && *out <= TASIM_WIRE_STOCK_MAX));
}

static void copy_name(char* dst, const char* src)
{
    int i;

    for (i = 0; i < TASIM_WIRE_NAMELEN - 1 && src[i]; i++)
        dst[i] = src[i];

    dst[i] = 0;
}

/* `@<index>` or `@<index>:<TYPE>` — a live unit and the caller's claim about
   what should be in that slot. Returns 0 on anything malformed; `expect` is
   left empty when no type was named. */
static int live_ref(const char* t, int* idx, char* expect)
{
    char  digits[16];
    int   i = 0;
    const char* p;

    expect[0] = 0;

    if (!t || t[0] != '@')
        return 0;

    for (p = t + 1; *p && *p != ':'; p++)
    {
        if (*p < '0' || *p > '9' || i >= (int)sizeof digits - 1)
            return 0;
        digits[i++] = *p;
    }

    if (!i)
        return 0;

    digits[i] = 0;

    if (!req(digits, idx))
        return 0;

    if (*p == ':')
    {
        if (!p[1])
            return 0;
        copy_name(expect, p + 1);
    }

    return 1;
}

/* The first column of an `order` line: an ordinal, `sel`, or a live reference. */
static int order_subject(char* t, TASIM_WIRE_ORDER* o, int line, TASIM_WIRE_ERRS* errs)
{
    if (!t)
    {
        tasim_wire_err(errs, "line %d: order wants a subject (an ordinal, sel, or @index)", line);
        return 0;
    }

    if (!strcmp(t, "sel"))
    {
        o->subj = TASIM_SUBJ_SEL;
        return 1;
    }

    if (t[0] == '@')
    {
        if (!live_ref(t, &o->uord, o->sexp))
        {
            tasim_wire_err(errs, "line %d: order subject `%.24s` is not @<index> or "
                                 "@<index>:<TYPE>", line, t);
            return 0;
        }
        o->subj = TASIM_SUBJ_LIVE;
        return 1;
    }

    if (!req(t, &o->uord))
    {
        tasim_wire_err(errs, "line %d: order wants a unit ordinal", line);
        return 0;
    }

    o->subj = TASIM_SUBJ_ORD;
    return 1;
}

/* A `queue` order joins the chain its subject's earlier lines make: the chain's head is the nearest earlier line
   of the same subject without `queue`, and every line between them is the same subject's.

   THE BOUND THAT KEEPS SHIFT FROM CANCELLING: 0x43AFC0 with Shift removes a node of the same action and the same
   target and adds nothing (0x43AFE0..0x43B087, DISASSEMBLED), so a queued order must name a unit no earlier order
   of its chain names; and an order issued before its predecessor would be wiped by the predecessor's non-Shift
   issue, so its `after` is never below the previous one's.

   The rest keeps the bound checkable here: an ordinal subject and an ordinal unit target are the only identities
   the wire can compare (a live `@` reference names a slot, and two references can name one unit), and a position
   or a feature target would need the toggle's own test, within 16 world units on x and on z of a position the
   chain stores (0x43B004..0x43B029). Buildweapon goes to the queue root 0x43B0B0, which takes no Shift. The walk
   back is bounded by the orders already parsed. */
static int queue_joins_chain(const TASIM_WIRE* w, const TASIM_WIRE_ORDER* o, int line, TASIM_WIRE_ERRS* errs)
{
    const TASIM_WIRE_ORDER* prev = w->norders > 0 ? &w->orders[w->norders - 1] : 0;
    int                     i;

    if (o->subj != TASIM_SUBJ_ORD || o->kind != TASIM_TGT_UNIT || o->tlive)
    {
        tasim_wire_err(errs, "line %d: order %d: queue wants an ordinal subject and a `unit <ordinal>` target",
                             line, o->uord);
        return 0;
    }
    if (!prev || prev->subj != TASIM_SUBJ_ORD || prev->uord != o->uord)
    {
        tasim_wire_err(errs, "line %d: order %d: queue joins the line before it, which must be an order of "
                             "the same unit", line, o->uord);
        return 0;
    }
    if (o->after < prev->after)
    {
        tasim_wire_err(errs, "line %d: order %d: queue's after=%d is below the line before it (%d)", line,
                             o->uord, o->after, prev->after);
        return 0;
    }
    for (i = w->norders - 1; i >= 0 && w->orders[i].subj == TASIM_SUBJ_ORD && w->orders[i].uord == o->uord; i--)
    {
        const TASIM_WIRE_ORDER* c = &w->orders[i];
        if (c->kind == TASIM_TGT_UNIT && !c->tlive && c->a == o->a)
        {
            tasim_wire_err(errs, "line %d: order %d: queue repeats target unit %d of its chain, which Shift "
                                 "would cancel", line, o->uord, o->a);
            return 0;
        }
        if (!c->queue)
            break;
    }
    return 1;
}

/* A unit line's optional keys after its last column: `at=<n>`, then `slot=<p>`, then `cloak=1`, each at most once
   and in that order; `cloak=1` only on a unit made at the apply (tasim_wire.h). Two units of one owner never ask
   for one slot: the applier creates each request in the slot it names or not at all, so a repeat could only be
   refused later, at its create. The walk back is bounded by the units already parsed. */
static int unit_keys(const TASIM_WIRE* w, TASIM_WIRE_UNIT* u, char* cur, int line, int ord, TASIM_WIRE_ERRS* errs)
{
    char* t;
    int   i;

    u->at = 0;
    u->slot = TASIM_WIRE_UNSET;
    u->cloak = 0;
    t = tasim_wire_tok(&cur);
    if (t && !strncmp(t, "at=", 3))
    {
        if (!req(t + 3, &u->at) || u->at < 0 || u->at > TASIM_WIRE_AT_MAX)
        {
            tasim_wire_err(errs, "line %d: unit %d `%.24s` wants at=0..%d", line, ord, t, TASIM_WIRE_AT_MAX);
            return 0;
        }
        t = tasim_wire_tok(&cur);
    }
    if (t && !strncmp(t, "slot=", 5))
    {
        if (!req(t + 5, &u->slot) || u->slot < 0 || u->slot > TASIM_WIRE_SLOT_MAX)
        {
            tasim_wire_err(errs, "line %d: unit %d `%.24s` wants slot=0..%d", line, ord, t, TASIM_WIRE_SLOT_MAX);
            return 0;
        }
        t = tasim_wire_tok(&cur);
    }
    if (t && (!strncmp(t, "at=", 3) || !strncmp(t, "slot=", 5)))
    {
        tasim_wire_err(errs, "line %d: unit %d: `%.24s` is repeated or out of order (at= comes before slot=)",
                             line, ord, t);
        return 0;
    }
    if (t && !strncmp(t, "cloak=", 6))
    {
        if (strcmp(t + 6, "1"))
        {
            tasim_wire_err(errs, "line %d: unit %d `%.24s` wants cloak=1", line, ord, t);
            return 0;
        }
        if (u->at)
        {
            tasim_wire_err(errs, "line %d: unit %d: cloak=1 is the apply's, and the unit is made at=%d",
                                 line, ord, u->at);
            return 0;
        }
        u->cloak = 1;
        t = tasim_wire_tok(&cur);
    }
    if (t && (!strncmp(t, "at=", 3) || !strncmp(t, "slot=", 5) || !strncmp(t, "cloak=", 6)))
    {
        tasim_wire_err(errs, "line %d: unit %d: `%.24s` is repeated or out of order (cloak= comes last)",
                             line, ord, t);
        return 0;
    }
    if (t)
    {
        tasim_wire_err(errs, "line %d: unit %d has a malformed column", line, ord);
        return 0;
    }
    if (u->slot == TASIM_WIRE_UNSET)
        return 1;
    for (i = 0; i < w->nunits; i++)
        if (w->units[i].slot == u->slot && w->units[i].owner == u->owner)
        {
            tasim_wire_err(errs, "line %d: unit %d asks for slot %d of player %d, which unit %d asks for", line,
                                 ord, u->slot, u->owner, i);
            return 0;
        }
    return 1;
}

/* An ordinal subject and its ordinal unit target are created on one tick: an order is issued when its subject is
   made, and its target must be there then and must not be a unit of another create tick. A live or selected
   subject is there at the apply, so its ordinal target must be too. Ordinals outside the wire are the applier's
   to refuse (resolve_all), so only those inside it are compared here. */
static int orders_share_at(const TASIM_WIRE* w, TASIM_WIRE_ERRS* errs)
{
    int i;

    for (i = 0; i < w->norders; i++)
    {
        const TASIM_WIRE_ORDER* o = &w->orders[i];
        int sat, tat;

        if (o->kind != TASIM_TGT_UNIT || o->tlive || o->a < 0 || o->a >= w->nunits)
            continue;
        if (o->subj == TASIM_SUBJ_ORD && (o->uord < 0 || o->uord >= w->nunits))
            continue;
        sat = o->subj == TASIM_SUBJ_ORD ? w->units[o->uord].at : 0;
        tat = w->units[o->a].at;
        if (sat != tat)
        {
            tasim_wire_err(errs, "order %d: its subject is created at=%d and its target unit %d at=%d; an order's "
                                 "subject and unit target are created on one tick", i, sat, o->a, tat);
            return 0;
        }
    }
    return 1;
}

int tasim_wire_parse(TASIM_WIRE* w, char* text, TASIM_WIRE_ERRS* errs)
{
    char* p = text;
    int   sawv = 0, sawend = 0;
    int   want_units = -1, want_feats = -1;
    int   line = 0;

    tasim_wire_reset(w);

    while (*p)
    {
        char* eol = strchr(p, '\n');
        char* cur = p;
        char* verb;

        if (eol)
        {
            *eol = 0;
            p = eol + 1;
        }
        else
        {
            p = cur + strlen(cur);
        }

        line++;

        {   /* strip a trailing CR from a file that crossed a Windows share */
            size_t n = strlen(cur);
            if (n && cur[n - 1] == '\r')
                cur[n - 1] = 0;
        }

        verb = tasim_wire_tok(&cur);

        if (!verb || verb[0] == '#')
            continue;

        if (sawend)
        {
            tasim_wire_err(errs, "line %d: %.32s after the `end` marker", line, verb);
            return 0;
        }

        if (!sawv && strcmp(verb, "v1") != 0)
        {
            tasim_wire_err(errs, "line %d: expected the version line `v1 ...`, got %.32s", line, verb);
            return 0;
        }

        if (!strcmp(verb, "v1"))
        {
            char* t;
            sawv = 1;
            while ((t = tasim_wire_tok(&cur)) != 0)
            {
                char* k;
                int   v;
                if (!strncmp(t, "onerror=", 8))
                {
                    w->abort = strcmp(t + 8, "skip") != 0;
                    continue;
                }
                /* the seed is tacli's 32-bit value, 0..0xFFFFFFFF, which the signed `num` would
                   saturate at the DLL's LONG_MAX; a negative one is its 32-bit pattern */
                if (!strncmp(t, "seed=", 5))
                {
                    char*     end;
                    long long v64 = strtoll(t + 5, &end, 10);
                    if (end == t + 5 || *end || v64 < -2147483648LL || v64 > 4294967295LL)
                    {
                        tasim_wire_err(errs, "line %d: `seed` is not name=value", line);
                        return 0;
                    }
                    w->seed = (unsigned int)v64;
                    continue;
                }
                if (!tasim_wire_kv(t, &k, &v))
                {
                    tasim_wire_err(errs, "line %d: `%.32s` is not name=value", line, t);
                    return 0;
                }
                if (!strcmp(k, "clear")) w->clear = v != 0;
                else if (!strcmp(k, "units")) want_units = v;
                else if (!strcmp(k, "feats")) want_feats = v;
                else
                {
                    tasim_wire_err(errs, "line %d: unknown v1 key `%.32s`", line, k);
                    return 0;
                }
            }
            continue;
        }

        if (!strcmp(verb, "map"))
        {
            while (*cur == ' ' || *cur == '\t')
                cur++;
            snprintf(w->map, sizeof w->map, "%s", cur);
            continue;
        }

        if (!strcmp(verb, "limit"))
        {
            if (!req(tasim_wire_tok(&cur), &w->limit))
            {
                tasim_wire_err(errs, "line %d: limit wants one number", line);
                return 0;
            }
            continue;
        }

        if (!strcmp(verb, "sw"))
        {
            char* t;
            while ((t = tasim_wire_tok(&cur)) != 0)
            {
                char* k;
                int   v, i, found = 0;
                if (!tasim_wire_kv(t, &k, &v))
                {
                    tasim_wire_err(errs, "line %d: `%.32s` is not name=0|1", line, t);
                    return 0;
                }
                for (i = 0; tasim_wire_switches[i].name; i++)
                    if (!strcmp(k, tasim_wire_switches[i].name))
                    {
                        if (v) w->sw_set |= tasim_wire_switches[i].bit;
                        else   w->sw_clr |= tasim_wire_switches[i].bit;
                        found = 1;
                        break;
                    }
                if (!found)
                {
                    tasim_wire_err(errs, "line %d: no engine switch named `%.24s`", line, k);
                    return 0;
                }
            }
            continue;
        }

        if (!strcmp(verb, "player"))
        {
            char* t;
            int   slot;
            if (!req(tasim_wire_tok(&cur), &slot) || slot < 0 || slot > 9)
            {
                tasim_wire_err(errs, "line %d: player wants a slot 0..9", line);
                return 0;
            }
            if (w->npl >= 10)
            {
                tasim_wire_err(errs, "line %d: more than ten player lines", line);
                return 0;
            }
            w->pl[w->npl].slot = slot;
            w->pl[w->npl].metal = TASIM_WIRE_UNSET;
            w->pl[w->npl].energy = TASIM_WIRE_UNSET;
            w->pl[w->npl].share[0] = w->pl[w->npl].share[1] = TASIM_WIRE_UNSET;
            w->pl[w->npl].aioff = 0;
            w->pl[w->npl].hasstore = 0;
            w->pl[w->npl].mstore = w->pl[w->npl].estore = 0.0f;
            while ((t = tasim_wire_tok(&cur)) != 0)
            {
                char* k;
                int   v;
                if (!strcmp(t, "ai=off"))
                {
                    w->pl[w->npl].aioff = 1;
                    continue;
                }
                if (!strncmp(t, "mstore=", 7) || !strncmp(t, "estore=", 7))
                {
                    float* dst = t[0] == 'm' ? &w->pl[w->npl].mstore : &w->pl[w->npl].estore;
                    if (!storage_value(t + 7, dst))
                    {
                        tasim_wire_err(errs, "line %d: `%.32s` wants a finite storage of 0 or more",
                                             line, t);
                        return 0;
                    }
                    w->pl[w->npl].hasstore |= t[0] == 'm' ? 1 : 2;
                    continue;
                }
                if (!tasim_wire_kv(t, &k, &v))
                {
                    tasim_wire_err(errs, "line %d: `%.32s` is not name=value", line, t);
                    return 0;
                }
                if (!strcmp(k, "metal"))       w->pl[w->npl].metal = v;
                else if (!strcmp(k, "energy")) w->pl[w->npl].energy = v;
                else if (!strcmp(k, "share_metal") || !strcmp(k, "share_energy"))
                    w->pl[w->npl].share[k[6] == 'e'] = v == TASIM_WIRE_UNSET ? v : v < 0 ? -1 : v;
                else
                {
                    tasim_wire_err(errs, "line %d: unknown player key `%.24s`", line, k);
                    return 0;
                }
            }
            /* The seat setup writes both base storages together (0x496F64..0x496F97), and a
               half-given pair would leave the other at whatever the last level set. */
            if (w->pl[w->npl].hasstore && w->pl[w->npl].hasstore != 3)
            {
                tasim_wire_err(errs, "line %d: a player's storage wants both mstore= and estore=", line);
                return 0;
            }
            w->npl++;
            continue;
        }

        if (!strcmp(verb, "clearfeat"))
        {
            int v;
            if (!req(tasim_wire_tok(&cur), &v) || (v != 0 && v != 1) || tasim_wire_tok(&cur))
            {
                tasim_wire_err(errs, "line %d: clearfeat wants 0 or 1", line);
                return 0;
            }
            w->clearfeat = v;
            continue;
        }

        if (!strcmp(verb, "objective"))
        {
            char* t = tasim_wire_tok(&cur);
            if (!t || (strcmp(t, "off") && strcmp(t, "on")) || tasim_wire_tok(&cur))
            {
                tasim_wire_err(errs, "line %d: objective wants off or on", line);
                return 0;
            }
            w->objective = t[1] == 'n';
            continue;
        }

        if (!strcmp(verb, "unit"))
        {
            TASIM_WIRE_UNIT* u;
            char*            name;
            int              ord;
            if (w->nunits >= w->units_cap)
            {
                tasim_wire_err(errs, "more than %d units -- the fork's own ceiling", w->units_cap);
                return 0;
            }
            if (!req(tasim_wire_tok(&cur), &ord) || ord != w->nunits)
            {
                tasim_wire_err(errs, "line %d: unit ordinals must run 0,1,2... (expected %d)",
                                     line, w->nunits);
                return 0;
            }
            u = &w->units[w->nunits];
            memset(u, 0, sizeof *u);
            name = tasim_wire_tok(&cur);
            if (!name)
            {
                tasim_wire_err(errs, "line %d: unit %d has no type", line, ord);
                return 0;
            }
            copy_name(u->type, name);
            if (!req(tasim_wire_tok(&cur), &u->owner) ||
                !req(tasim_wire_tok(&cur), &u->x) || !req(tasim_wire_tok(&cur), &u->y) ||
                !num(tasim_wire_tok(&cur), &u->height) || !num(tasim_wire_tok(&cur), &u->facing) ||
                !num(tasim_wire_tok(&cur), &u->health) || !token_of(tasim_wire_tok(&cur), STANCES, &u->stance) ||
                !num(tasim_wire_tok(&cur), &u->nano) || !num(tasim_wire_tok(&cur), &u->kills) ||
                !token_of(tasim_wire_tok(&cur), FIRES, &u->fire) ||
                !stock_value(tasim_wire_tok(&cur), &u->stock))
            {
                tasim_wire_err(errs, "line %d: unit %d has a malformed column", line, ord);
                return 0;
            }
            if (!unit_keys(w, u, cur, line, ord, errs))
                return 0;
            w->nunits++;
            continue;
        }

        if (!strcmp(verb, "feat"))
        {
            TASIM_WIRE_FEAT* fe;
            char*            name;
            int              ord;
            if (w->nfeats >= w->feats_cap)
            {
                tasim_wire_err(errs, "more than %d features -- the fork's own ceiling", w->feats_cap);
                return 0;
            }
            if (!req(tasim_wire_tok(&cur), &ord) || ord != w->nfeats)
            {
                tasim_wire_err(errs, "line %d: feature ordinals must run 0,1,2... (expected %d)",
                                     line, w->nfeats);
                return 0;
            }
            fe = &w->feats[w->nfeats];
            memset(fe, 0, sizeof *fe);
            name = tasim_wire_tok(&cur);
            if (!name)
            {
                tasim_wire_err(errs, "line %d: feature %d has no type", line, ord);
                return 0;
            }
            copy_name(fe->type, name);
            if (!req(tasim_wire_tok(&cur), &fe->x) || !req(tasim_wire_tok(&cur), &fe->y) ||
                !num(tasim_wire_tok(&cur), &fe->height) || !num(tasim_wire_tok(&cur), &fe->facing))
            {
                tasim_wire_err(errs, "line %d: feature %d has a malformed column", line, ord);
                return 0;
            }
            w->nfeats++;
            continue;
        }

        if (!strcmp(verb, "order"))
        {
            TASIM_WIRE_ORDER* o;
            char*             cmd;
            char*             kind;
            char*             tail;
            int               i, found = 0;
            if (w->norders >= w->orders_cap)
            {
                tasim_wire_err(errs, "more than %d orders -- the fork's own ceiling", w->orders_cap);
                return 0;
            }
            o = &w->orders[w->norders];
            memset(o, 0, sizeof *o);
            if (!order_subject(tasim_wire_tok(&cur), o, line, errs))
                return 0;
            cmd = tasim_wire_tok(&cur);
            if (!cmd)
            {
                tasim_wire_err(errs, "line %d: order %d has no command", line, o->uord);
                return 0;
            }
            for (i = 0; ORDERTYPES[i].name; i++)
                if (!strcmp(cmd, ORDERTYPES[i].name))
                {
                    o->cmd = ORDERTYPES[i].id;
                    found = 1;
                    break;
                }
            if (!found)
            {
                tasim_wire_err(errs, "line %d: no engine order named `%.24s`", line, cmd);
                return 0;
            }
            kind = tasim_wire_tok(&cur);
            tail = 0;
            if (!kind || !strncmp(kind, "after=", 6) || !strcmp(kind, "queue"))
            {
                o->kind = TASIM_TGT_NONE;
                tail = kind;
            }
            else if (!strcmp(kind, "pos"))
            {
                o->kind = TASIM_TGT_POS;
                if (!req(tasim_wire_tok(&cur), &o->a) || !req(tasim_wire_tok(&cur), &o->b))
                {
                    tasim_wire_err(errs, "line %d: order %d pos wants x and y", line, o->uord);
                    return 0;
                }
            }
            else if (!strcmp(kind, "count"))
            {
                o->kind = TASIM_TGT_COUNT;
                if (!req(tasim_wire_tok(&cur), &o->a) || o->a < 1 || o->a > TASIM_WIRE_STOCK_MAX)
                {
                    tasim_wire_err(errs, "line %d: order %d count wants 1..%d", line, o->uord,
                                         TASIM_WIRE_STOCK_MAX);
                    return 0;
                }
            }
            else if (!strcmp(kind, "unit") || !strcmp(kind, "feat"))
            {
                char* t = tasim_wire_tok(&cur);
                o->kind = kind[0] == 'u' ? TASIM_TGT_UNIT : TASIM_TGT_FEAT;
                if (o->kind == TASIM_TGT_UNIT && t && t[0] == '@')
                {
                    if (!live_ref(t, &o->a, o->texp))
                    {
                        tasim_wire_err(errs, "line %d: order %d target `%.24s` is not "
                                             "@<index> or @<index>:<TYPE>", line, o->uord, t);
                        return 0;
                    }
                    o->tlive = 1;
                }
                else if (!req(t, &o->a))
                {
                    tasim_wire_err(errs, "line %d: order %d %s wants an ordinal", line, o->uord, kind);
                    return 0;
                }
            }
            else
            {
                tasim_wire_err(errs, "line %d: order target must be pos/unit/feat/count, got `%.16s`",
                                     line, kind);
                return 0;
            }
            if ((o->cmd == TASIM_CMD_BUILDWEAPON) != (o->kind == TASIM_TGT_COUNT))
            {
                tasim_wire_err(errs, "line %d: order %d: buildweapon takes a count and nothing else, and "
                                     "only buildweapon takes one", line, o->uord);
                return 0;
            }
            if (o->kind != TASIM_TGT_NONE)
                tail = tasim_wire_tok(&cur);
            if (tail && strcmp(tail, "queue"))
            {
                if (!after_value(tail, &o->after))
                {
                    tasim_wire_err(errs, "line %d: order %d `%.24s` wants after=0..255", line, o->uord, tail);
                    return 0;
                }
                tail = tasim_wire_tok(&cur);
            }
            if (tail && !strcmp(tail, "queue"))
            {
                o->queue = 1;
                tail = tasim_wire_tok(&cur);
            }
            if (tail)
            {
                tasim_wire_err(errs, "line %d: order %d has a column past its end", line, o->uord);
                return 0;
            }
            if (o->queue && !queue_joins_chain(w, o, line, errs))
                return 0;
            w->norders++;
            continue;
        }

        if (!strcmp(verb, "cam"))
        {
            char* kind = tasim_wire_tok(&cur);
            char* t;
            if (!kind)
            {
                tasim_wire_err(errs, "line %d: cam wants pos/unit/feat", line);
                return 0;
            }
            if (!strcmp(kind, "pos"))
            {
                w->cam_kind = TASIM_TGT_POS;
                if (!req(tasim_wire_tok(&cur), &w->cam_a) || !req(tasim_wire_tok(&cur), &w->cam_b))
                {
                    tasim_wire_err(errs, "line %d: cam pos wants x and y", line);
                    return 0;
                }
            }
            else if (!strcmp(kind, "unit") || !strcmp(kind, "feat"))
            {
                w->cam_kind = kind[0] == 'u' ? TASIM_TGT_UNIT : TASIM_TGT_FEAT;
                if (!req(tasim_wire_tok(&cur), &w->cam_a))
                {
                    tasim_wire_err(errs, "line %d: cam %s wants an ordinal", line, kind);
                    return 0;
                }
            }
            else
            {
                tasim_wire_err(errs, "line %d: cam target must be pos/unit/feat", line);
                return 0;
            }
            while ((t = tasim_wire_tok(&cur)) != 0)
            {
                char* k;
                int   v;
                if (tasim_wire_kv(t, &k, &v) && !strcmp(k, "pin"))
                    w->cam_pin = v != 0;
            }
            continue;
        }

        if (!strcmp(verb, "end"))
        {
            sawend = 1;
            continue;
        }

        tasim_wire_err(errs, "line %d: unknown wire verb `%.24s`", line, verb);
        return 0;
    }

    if (!sawend)
    {
        tasim_wire_err(errs, "the file has no `end` marker -- it is truncated, nothing was applied");
        return 0;
    }

    if (want_units >= 0 && want_units != w->nunits)
    {
        tasim_wire_err(errs, "the header promised %d units and the file carries %d",
                             want_units, w->nunits);
        return 0;
    }

    if (want_feats >= 0 && want_feats != w->nfeats)
    {
        tasim_wire_err(errs, "the header promised %d features and the file carries %d",
                             want_feats, w->nfeats);
        return 0;
    }

    return orders_share_at(w, errs);
}
