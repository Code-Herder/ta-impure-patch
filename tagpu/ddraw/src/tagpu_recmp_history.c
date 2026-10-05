#include "tagpu_recmp_history.h"
#include <string.h>

struct RecMpHistory {
    RecMpRoster roster;
    RecMpFileState ledger;
    RecMpPlayers *source_player, *applied_player;
    RecMpSeatMaps *source_seats, *applied_seats;
    RecMpUnits *source_units, *applied_units;
    RecMpNamed *source_named, *applied_named;
    RecMpRoots *source_roots, *applied_roots;
    RecMpOrderCache *source_orders, *applied_orders;
    RecMpExplored *source_explored, *applied_explored;
    RecMpText *source_text, *applied_text;
    unsigned failed;
};

RecMpHistory* tagpu_recmp_history_begin(const RecMpRecordHeader* header, const RecMpRoster* roster,
                                            unsigned width, unsigned height)
{
    RecMpHistory* h;
    if (!tagpu_recmp_file_roster_valid(header,roster)) return NULL;
    h=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *h);
    if (!h) return NULL;
    h->roster=*roster;
    tagpu_recmp_file_state(&h->ledger,header);
    h->source_player=tagpu_recmp_players_begin(roster);
    h->applied_player=tagpu_recmp_players_begin(roster);
    h->source_seats=tagpu_recmp_seats_begin(roster);
    h->applied_seats=tagpu_recmp_seats_begin(roster);
    h->source_units=tagpu_recmp_units_begin(roster);
    h->applied_units=tagpu_recmp_units_begin(roster);
    h->source_named=tagpu_recmp_named_begin(roster);
    h->applied_named=tagpu_recmp_named_begin(roster);
    h->source_roots=tagpu_recmp_roots_begin(roster);
    h->applied_roots=tagpu_recmp_roots_begin(roster);
    h->source_explored=tagpu_recmp_explored_begin(roster,width,height);
    h->applied_explored=tagpu_recmp_explored_begin(roster,width,height);
    h->source_text=tagpu_recmp_text_begin(roster);
    h->applied_text=tagpu_recmp_text_begin(roster);
    h->source_orders=tagpu_recmp_orders_begin(roster->slots);
    h->applied_orders=tagpu_recmp_orders_begin(roster->slots);
    if (!h->source_player || !h->applied_player || !h->source_seats || !h->applied_seats ||
        !h->source_units || !h->applied_units || !h->source_named || !h->applied_named || !h->source_roots || !h->applied_roots ||
        !h->source_orders || !h->applied_orders || !h->source_explored || !h->applied_explored ||
        !h->source_text || !h->applied_text) {
        tagpu_recmp_history_end(h); return NULL;
    }
    return h;
}

void tagpu_recmp_history_end(RecMpHistory* h)
{
    if (!h) return;
    tagpu_recmp_players_end(h->source_player);
    tagpu_recmp_players_end(h->applied_player);
    tagpu_recmp_seats_end(h->source_seats);
    tagpu_recmp_seats_end(h->applied_seats);
    tagpu_recmp_units_end(h->source_units);
    tagpu_recmp_units_end(h->applied_units);
    tagpu_recmp_named_end(h->source_named);
    tagpu_recmp_named_end(h->applied_named);
    tagpu_recmp_roots_end(h->source_roots);
    tagpu_recmp_roots_end(h->applied_roots);
    tagpu_recmp_orders_end(h->source_orders);
    tagpu_recmp_orders_end(h->applied_orders);
    tagpu_recmp_explored_end(h->source_explored);
    tagpu_recmp_explored_end(h->applied_explored);
    tagpu_recmp_text_end(h->source_text);
    tagpu_recmp_text_end(h->applied_text);
    HeapFree(GetProcessHeap(),0,h);
}

static int semantic(RecMpHistory* h, const RecMpRecord* where, const RecMpHeader* packet,
                      const void* body, unsigned bytes)
{
    unsigned proof=where->phase==RECMP_RECORD_PROOF;
    unsigned tick=proof ? h->ledger.tick : where->tick;
    if (packet->kind==RECMP_SEATS) {
        if (!tagpu_recmp_seats_put(h->source_seats,packet,body,bytes)) return 0;
        return proof || tagpu_recmp_seats_put(h->applied_seats,packet,body,bytes);
    }
    if (packet->kind==RECMP_PLAYER) {
        RecMpPlayerState state;
        unsigned image[REC_PLAYER_DWORDS];
        if (!tagpu_recmp_seats_get(h->source_seats,packet->origin,NULL,NULL) ||
            !tagpu_recmp_players_put(h->source_player,packet,body,bytes,tick) ||
            !tagpu_recmp_players_get(h->source_player,packet->origin,&state) ||
            !tagpu_recmp_seats_image(h->source_seats,packet->origin,&state.stream,
                                     state.image,sizeof state.image,image,sizeof image)) return 0;
        if (!proof && !tagpu_recmp_seats_get(h->applied_seats,packet->origin,NULL,NULL)) return 0;
        return proof || tagpu_recmp_players_put(h->applied_player,packet,body,bytes,tick);
    }
    if (packet->kind==RECMP_EXPLORED) {
        if (!tagpu_recmp_explored_put(h->source_explored,h->source_seats,packet,body,bytes,tick)) return 0;
        return proof || tagpu_recmp_explored_put(h->applied_explored,h->applied_seats,packet,body,bytes,tick);
    }
    if (packet->kind==RECMP_TEXT) {
        if (!tagpu_recmp_text_put(h->source_text,h->source_seats,packet,body,bytes,tick)) return 0;
        return proof || tagpu_recmp_text_put(h->applied_text,h->applied_seats,packet,body,bytes,tick);
    }
    if (packet->kind==RECMP_UNITS) {
        if (!tagpu_recmp_units_put(h->source_units,h->source_seats,packet,body,bytes,tick)) return 0;
        return proof || tagpu_recmp_units_put(h->applied_units,h->applied_seats,packet,body,bytes,tick);
    }
    if (packet->kind==RECMP_ROOTS) {
        if (!tagpu_recmp_roots_put(h->source_roots,h->source_seats,packet,body,bytes,tick)) return 0;
        return proof || tagpu_recmp_roots_put(h->applied_roots,h->applied_seats,packet,body,bytes,tick);
    }
    if (packet->kind==RECMP_NAMED) {
        if (!tagpu_recmp_named_put(h->source_named,h->source_seats,packet,body,bytes,tick)) return 0;
        return proof || tagpu_recmp_named_put(h->applied_named,h->applied_seats,packet,body,bytes,tick);
    }
    if (packet->kind==RECMP_ORDERS) {
        RecMpOrders chunk;
        unsigned i;
        if (!body || bytes<sizeof chunk ||
            !tagpu_recmp_seats_get(h->source_seats,packet->origin,NULL,NULL) ||
            (!proof && !tagpu_recmp_seats_get(h->applied_seats,packet->origin,NULL,NULL))) return 0;
        memcpy(&chunk,body,sizeof chunk);
        for (i=0;i<h->roster.count;i++) if (h->roster.actor[i].origin==packet->origin) break;
        if (i==h->roster.count || chunk.slot<h->roster.actor[i].first ||
            chunk.slot>h->roster.actor[i].last ||
            tagpu_recmp_orders_put(h->source_orders,packet,body,bytes,tick)<0) return 0;
        return proof || tagpu_recmp_orders_put(h->applied_orders,packet,body,bytes,tick)>=0;
    }
    if (packet->kind==RECMP_SEAL)
        return tagpu_recmp_orders_idle(h->source_orders,packet->origin) &&
               tagpu_recmp_explored_idle(h->source_explored,packet->origin);
    /* Legacy resource samples are opaque source-ledger records. They establish
       neither a complete player baseline nor order authority. Production uses PLAYER. */
    return packet->kind==RECMP_RESOURCES;
}

int tagpu_recmp_history_record(RecMpHistory* h, const RecMpRecord* where,
                               const RecMpHeader* packet, const void* body, unsigned bytes)
{
    RecMpFileState next;
    if (!h || h->failed) return 0;
    next=h->ledger;
    if (!tagpu_recmp_file_record(&next,where,packet,body,bytes) ||
        !semantic(h,where,packet,body,bytes)) { h->failed=1; return 0; }
    h->ledger=next;
    return 1;
}

int tagpu_recmp_history_block(RecMpHistory* h, const void* body, unsigned bytes)
{
    const unsigned char* b=body;
    unsigned at=0;
    if (!h || h->failed) return 0;
    if (!b || !bytes) { h->failed=1; return 0; }
    while (at<bytes) {
        RecMpRecord where;
        RecMpHeader packet;
        if (bytes-at<sizeof where+sizeof packet) break;
        memcpy(&where,b+at,sizeof where); at+=sizeof where;
        memcpy(&packet,b+at,sizeof packet);
        if (packet.bytes<sizeof packet || packet.bytes>RECMP_PACKET_MAX || packet.bytes>bytes-at ||
            !tagpu_recmp_history_record(h,&where,&packet,b+at+sizeof packet,packet.bytes-sizeof packet)) break;
        at+=packet.bytes;
    }
    if (at!=bytes) { h->failed=1; return 0; }
    return 1;
}

const RecMpFileState* tagpu_recmp_history_ledger(const RecMpHistory* h)
{
    return h && !h->failed ? &h->ledger : NULL;
}

int tagpu_recmp_history_footer(const RecMpHistory* h, const RecMpRecordEnd* end)
{
    return h && !h->failed && tagpu_recmp_text_complete(h->source_text) &&
           tagpu_recmp_file_footer(&h->ledger,end);
}

int tagpu_recmp_history_player(const RecMpHistory* h, unsigned origin, RecMpPlayerState* out)
{
    RecMpPlayerState state;
    if (!h || h->failed || !out || !tagpu_recmp_players_get(h->applied_player,origin,&state) ||
        !tagpu_recmp_seats_image(h->applied_seats,origin,&state.stream,state.image,sizeof state.image,
                                 state.image,sizeof state.image)) return 0;
    *out=state;
    return 1;
}

int tagpu_recmp_history_orders(const RecMpHistory* h, unsigned slot, unsigned list,
                               RecMpOrderState* out, void* image, unsigned capacity)
{
    return h && !h->failed && tagpu_recmp_orders_get(h->applied_orders,slot,list,out,image,capacity);
}

int tagpu_recmp_history_unit(const RecMpHistory* h, unsigned slot, RecMpUnitState* out)
{
    return h && !h->failed && tagpu_recmp_units_get(h->applied_units,h->applied_seats,slot,out);
}

int tagpu_recmp_history_named(const RecMpHistory* h, unsigned origin, RecMpNamedState* out)
{
    return h && !h->failed && tagpu_recmp_named_get(h->applied_named,origin,out);
}

int tagpu_recmp_history_roots(const RecMpHistory* h, unsigned origin, RecMpRootsState* out)
{
    return h && !h->failed && tagpu_recmp_roots_get(h->applied_roots,origin,out);
}

int tagpu_recmp_history_text(const RecMpHistory* h, unsigned origin, RecMpTextState* out)
{
    return h && !h->failed && tagpu_recmp_text_get(h->applied_text,origin,out);
}

int tagpu_recmp_history_explored(const RecMpHistory* h, unsigned origin,
                                  RecMpExploreState* out, void* image, unsigned capacity)
{
    return h && !h->failed && tagpu_recmp_explored_get(h->applied_explored,origin,out,image,capacity);
}
