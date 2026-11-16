/* tagpu_boardwire.c -- the drawing board's operations and their bytes (tagpu_boardwire.h). */
#include <string.h>
#include "tagpu_boardwire.h"

static int coord_ok(int32_t v) { return v >= -TAGPU_BW_COORD && v <= TAGPU_BW_COORD; }

static int text_ok(const char* t, unsigned len)
{
    unsigned k;
    if (len > TAGPU_BW_TEXT || t[len]) return 0;
    for (k = 0; k < len; k++)
        if ((unsigned char)t[k] < 0x20 || (unsigned char)t[k] > 0x7E) return 0;
    return 1;
}

static int id_ok(uint32_t id) { return tagpu_bw_id_seat(id) < TAGPU_BW_SEATS && (id & 0xFFFFFFu); }

static int has_may(unsigned code)
{
    return code == TAGPU_BW_EDIT || code == TAGPU_BW_MOVE || code == TAGPU_BW_DELETE ||
           code == TAGPU_BW_ERASE;
}

int tagpu_boardwire_ok(const TagpuBoardOp* op, unsigned seat)
{
    int k;
    if (seat >= TAGPU_BW_SEATS || (has_may(op->op) && op->may > TAGPU_BW_MAY_ALL)) return 0;
    switch (op->op) {
    case TAGPU_BW_POINT:
        if (!id_ok(op->id) || tagpu_bw_id_seat(op->id) != seat || !text_ok(op->text, op->len)) return 0;
        break;
    case TAGPU_BW_EDIT:
        return id_ok(op->id) && text_ok(op->text, op->len);
    case TAGPU_BW_MOVE:
        if (!id_ok(op->id)) return 0;
        break;
    case TAGPU_BW_DELETE:
        return id_ok(op->id);
    case TAGPU_BW_LINE:
        for (k = 0; k < 3; k++) if (!coord_ok(op->b[k])) return 0;
        break;
    case TAGPU_BW_ERASE:
        break;
    default:
        return 0;
    }
    for (k = 0; k < 3; k++) if (!coord_ok(op->a[k])) return 0;
    return 1;
}

static unsigned char* put32(unsigned char* o, uint32_t v)
{
    o[0] = (unsigned char)v; o[1] = (unsigned char)(v >> 8);
    o[2] = (unsigned char)(v >> 16); o[3] = (unsigned char)(v >> 24);
    return o + 4;
}

static uint32_t get32(const unsigned char* b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* an operation's size from its code and, for the two with text, its length byte; 0 unknown */
static unsigned op_size(unsigned code, unsigned len)
{
    switch (code) {
    case TAGPU_BW_POINT:  return 1 + 4 + 12 + 1 + len;
    case TAGPU_BW_EDIT:   return 1 + 4 + 2 + 1 + len;
    case TAGPU_BW_MOVE:   return 1 + 4 + 2 + 12;
    case TAGPU_BW_DELETE: return 1 + 4 + 2;
    case TAGPU_BW_LINE:   return 1 + 24;
    case TAGPU_BW_ERASE:  return 1 + 2 + 12;
    }
    return 0;
}

unsigned tagpu_boardwire_put(unsigned char* out, unsigned cap, const TagpuBoardOp* op, unsigned seat)
{
    unsigned n = op_size(op->op, op->len), k;
    unsigned char* o = out;
    if (!n || n > cap || !tagpu_boardwire_ok(op, seat)) return 0;
    *o++ = (unsigned char)op->op;
    if (op->op != TAGPU_BW_LINE && op->op != TAGPU_BW_ERASE) o = put32(o, op->id);
    if (has_may(op->op)) { o[0] = (unsigned char)op->may; o[1] = (unsigned char)(op->may >> 8); o += 2; }
    if (op->op == TAGPU_BW_POINT || op->op == TAGPU_BW_MOVE || op->op == TAGPU_BW_LINE ||
        op->op == TAGPU_BW_ERASE)
        for (k = 0; k < 3; k++) o = put32(o, (uint32_t)op->a[k]);
    if (op->op == TAGPU_BW_LINE)
        for (k = 0; k < 3; k++) o = put32(o, (uint32_t)op->b[k]);
    if (op->op == TAGPU_BW_POINT || op->op == TAGPU_BW_EDIT) {
        *o++ = (unsigned char)op->len;
        memcpy(o, op->text, op->len);
        o += op->len;
    }
    return (unsigned)(o - out);
}

/* the operation at body[at], read and checked; the next offset, 0 when it is not whole and well
   formed */
static unsigned read_op(const unsigned char* body, unsigned bytes, unsigned at, unsigned seat,
                        TagpuBoardOp* op)
{
    const unsigned char* p;
    unsigned code, len = 0, n, k;
    if (at >= bytes) return 0;
    code = body[at];
    if (code == TAGPU_BW_POINT && bytes - at >= 18) len = body[at + 17];
    if (code == TAGPU_BW_EDIT && bytes - at >= 8) len = body[at + 7];
    n = op_size(code, len);
    if (!n || len > TAGPU_BW_TEXT || n > bytes - at) return 0;
    memset(op, 0, sizeof *op);
    op->op = code;
    p = body + at + 1;
    if (code != TAGPU_BW_LINE && code != TAGPU_BW_ERASE) { op->id = get32(p); p += 4; }
    if (has_may(code)) { op->may = (unsigned)p[0] | (unsigned)p[1] << 8; p += 2; }
    if (code == TAGPU_BW_POINT || code == TAGPU_BW_MOVE || code == TAGPU_BW_LINE || code == TAGPU_BW_ERASE)
        for (k = 0; k < 3; k++, p += 4) op->a[k] = (int32_t)get32(p);
    if (code == TAGPU_BW_LINE)
        for (k = 0; k < 3; k++, p += 4) op->b[k] = (int32_t)get32(p);
    if (code == TAGPU_BW_POINT || code == TAGPU_BW_EDIT) {
        op->len = len;
        memcpy(op->text, p + 1, len);
        op->text[len] = 0;
    }
    return tagpu_boardwire_ok(op, seat) ? at + n : 0;
}

int tagpu_boardwire_check(const unsigned char* body, unsigned bytes, unsigned* seat)
{
    TagpuBoardOp op;
    unsigned at = TAGPU_BW_HEAD;
    if (!body || bytes <= TAGPU_BW_HEAD || body[0] != TAGPU_BW_VERSION || body[1] >= TAGPU_BW_SEATS)
        return 0;
    while (at < bytes)
        if (!(at = read_op(body, bytes, at, body[1], &op))) return 0;
    if (seat) *seat = body[1];
    return 1;
}

unsigned tagpu_boardwire_reseat(const unsigned char* body, unsigned bytes,
                                const unsigned char map[TAGPU_BW_SEATS], unsigned char* out,
                                unsigned cap)
{
    TagpuBoardOp op;
    unsigned seat, to, at = TAGPU_BW_HEAD, next, n = TAGPU_BW_HEAD;
    if (!tagpu_boardwire_check(body, bytes, &seat) || cap < bytes || (to = map[seat]) >= TAGPU_BW_SEATS)
        return 0;
    out[0] = TAGPU_BW_VERSION;
    out[1] = (unsigned char)to;
    while (at < bytes && (next = tagpu_boardwire_next(body, bytes, at, &op)) != 0) {
        unsigned k;
        if (op.op != TAGPU_BW_LINE && op.op != TAGPU_BW_ERASE) {
            unsigned owner = tagpu_bw_id_seat(op.id);
            if (map[owner] >= TAGPU_BW_SEATS) return 0;
            op.id = (uint32_t)map[owner] << 24 | (op.id & 0xFFFFFFu);
        }
        if (has_may(op.op)) {
            unsigned s, may = 0;
            for (s = 0; s < TAGPU_BW_SEATS; s++)
                if (op.may >> s & 1u) {
                    if (map[s] >= TAGPU_BW_SEATS) return 0;
                    may |= 1u << map[s];
                }
            op.may = may;
        }
        if (!(k = tagpu_boardwire_put(out + n, cap - n, &op, to))) return 0;
        n += k;
        at = next;
    }
    return n == bytes ? n : 0;
}

unsigned tagpu_boardwire_next(const unsigned char* body, unsigned bytes, unsigned at,
                              TagpuBoardOp* op)
{
    if (at >= bytes) return 0;
    return read_op(body, bytes, at, body[1], op);
}
