#include "tagpu_recmp.h"
#include <string.h>

#define ORDER_POOL_NODES 131072u
#define ORDER_SOURCES 10u

typedef struct {
    unsigned next;
    unsigned char image[RECMP_ORDER_BYTES];
} OrderNode;

typedef struct {
    RecMpOrderState state;
    unsigned head, valid;
} OrderList;

typedef struct {
    unsigned origin, sequence, tick, active;
    GUID stream;
    RecMpOrders chunk;
    unsigned char image[RECMP_ORDER_MAX * RECMP_ORDER_BYTES];
} OrderStage;

struct RecMpOrderCache {
    unsigned slots, free, available;
    OrderStage source[ORDER_SOURCES];
    OrderNode node[ORDER_POOL_NODES];
    OrderList list[];
};

RecMpOrderCache* tagpu_recmp_orders_begin(unsigned slots)
{
    RecMpOrderCache* c;
    unsigned i;
    if (slots < 2 || slots > 65536u) return NULL;
    c = VirtualAlloc(NULL, sizeof *c + (size_t)slots * 2 * sizeof(OrderList),
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!c) return NULL;
    c->slots = slots;
    c->free = 1;
    c->available = ORDER_POOL_NODES;
    for (i = 0; i < ORDER_POOL_NODES; i++)
        c->node[i].next = i + 1 < ORDER_POOL_NODES ? i + 2 : 0;
    return c;
}

void tagpu_recmp_orders_end(RecMpOrderCache* c)
{
    if (c) VirtualFree(c, 0, MEM_RELEASE);
}

int tagpu_recmp_orders_idle(const RecMpOrderCache* c, unsigned origin)
{
    unsigned i;
    if (!c || !origin) return 0;
    for (i = 0; i < ORDER_SOURCES; i++)
        if (c->source[i].origin == origin && c->source[i].active) return 0;
    return 1;
}

static OrderList* order_list(const RecMpOrderCache* c, unsigned slot, unsigned list)
{
    if (!c || !slot || slot >= c->slots || list > 1) return NULL;
    return (OrderList*)&c->list[slot * 2 + list];
}

static void list_free(RecMpOrderCache* c, OrderList* l)
{
    unsigned id = l->head;
    while (id) {
        OrderNode* node = &c->node[id - 1];
        unsigned next = node->next;
        node->next = c->free;
        c->free = id;
        c->available++;
        id = next;
    }
    memset(l, 0, sizeof *l);
}

void tagpu_recmp_orders_drop(RecMpOrderCache* c, unsigned slot)
{
    OrderList* l = order_list(c, slot, 0);
    if (!l) return;
    list_free(c, l);
    list_free(c, l + 1);
}

int tagpu_recmp_orders_same(const RecMpOrderCache* c, unsigned slot, unsigned list,
                            unsigned origin, unsigned birth, const void* image, unsigned count)
{
    const OrderList* l = order_list(c, slot, list);
    const unsigned char* bytes = image;
    unsigned id, i;
    if (!l || !l->valid || count > RECMP_ORDER_MAX || (count && !image) ||
        l->state.origin != origin || l->state.birth != birth || l->state.count != count) return 0;
    id = l->head;
    for (i = 0; i < count; i++) {
        const OrderNode* node = &c->node[id - 1];
        if (memcmp(node->image, bytes + (size_t)i * RECMP_ORDER_BYTES, RECMP_ORDER_BYTES)) return 0;
        id = node->next;
    }
    return 1;
}

int tagpu_recmp_orders_get(const RecMpOrderCache* c, unsigned slot, unsigned list,
                           RecMpOrderState* state, void* image, unsigned capacity)
{
    const OrderList* l = order_list(c, slot, list);
    unsigned id, i;
    if (!l || !l->valid || !state || l->state.count > capacity || (l->state.count && !image)) return 0;
    *state = l->state;
    id = l->head;
    for (i = 0; i < l->state.count; i++) {
        const OrderNode* node = &c->node[id - 1];
        memcpy((unsigned char*)image + (size_t)i * RECMP_ORDER_BYTES, node->image, RECMP_ORDER_BYTES);
        id = node->next;
    }
    return 1;
}

int tagpu_recmp_orders_put(RecMpOrderCache* c, const RecMpHeader* h,
                           const void* body, unsigned bytes, unsigned applied_tick)
{
    RecMpOrders chunk;
    OrderStage* stage = NULL;
    OrderList* l;
    unsigned i, previous = 0;
    if (!c || !h || !body || bytes < sizeof chunk || !h->origin || !h->sequence) return -1;
    memcpy(&chunk, body, sizeof chunk);
    l = order_list(c, chunk.slot, chunk.list);
    if (!l || chunk.reserved || chunk.reserved2 || chunk.total > RECMP_ORDER_MAX ||
        chunk.count > RECMP_ORDER_CHUNK || chunk.first > chunk.total ||
        chunk.count > chunk.total - chunk.first ||
        bytes != sizeof chunk + chunk.count * RECMP_ORDER_BYTES ||
        (!chunk.count && (chunk.total || chunk.first))) return -1;
    for (i = 0; i < ORDER_SOURCES; i++) {
        if (c->source[i].origin == h->origin) { stage = &c->source[i]; break; }
        if (!stage && !c->source[i].origin) stage = &c->source[i];
    }
    if (!stage) return -1;
    if (stage->origin && (memcmp(&stage->stream, &h->stream, sizeof h->stream) ||
                          h->sequence <= stage->sequence || h->tick < stage->tick)) return -1;
    if (!chunk.first) {
        if (stage->active) return -1;
        stage->origin = h->origin;
        stage->stream = h->stream;
        stage->tick = h->tick;
        stage->chunk = chunk;
        stage->chunk.first = 0;
        stage->active = 1;
    } else if (!stage->active || memcmp(&stage->stream, &h->stream, sizeof h->stream) ||
               stage->tick != h->tick || stage->sequence + 1 != h->sequence ||
               stage->chunk.slot != chunk.slot || stage->chunk.list != chunk.list ||
               stage->chunk.birth != chunk.birth || stage->chunk.total != chunk.total ||
               stage->chunk.first != chunk.first) return -1;
    memcpy(stage->image + (size_t)chunk.first * RECMP_ORDER_BYTES,
           (const unsigned char*)body + sizeof chunk, chunk.count * RECMP_ORDER_BYTES);
    stage->sequence = h->sequence;
    stage->chunk.first += chunk.count;
    if (stage->chunk.first != chunk.total) return 0;
    /* Enough room is proved before releasing the old list. No allocation or failure
       lies between that release and publication of the complete replacement. */
    if (chunk.total > c->available + l->state.count) return -1;
    list_free(c, l);
    for (i = 0; i < chunk.total; i++) {
        unsigned id = c->free;
        OrderNode* node = &c->node[id - 1];
        c->free = node->next;
        c->available--;
        node->next = 0;
        memcpy(node->image, stage->image + (size_t)i * RECMP_ORDER_BYTES, RECMP_ORDER_BYTES);
        if (previous) c->node[previous - 1].next = id;
        else l->head = id;
        previous = id;
    }
    l->state.origin = h->origin;
    l->state.birth = chunk.birth;
    /* Source clocks need not equal this recorder's clock. A complete transaction
       becomes available at its final chunk's application tick; partial chunks leave
       both the old image and its provenance intact. */
    l->state.source_tick = h->tick;
    l->state.sequence = h->sequence;
    l->state.applied_tick = applied_tick;
    l->state.count = chunk.total;
    l->valid = 1;
    stage->active = 0;
    return 1;
}
