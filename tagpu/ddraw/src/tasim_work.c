/* tasim_work.c -- the shared simulation core's work: a unit appearing (its script's create), the INBUILDSTANCE gate,
   a builder's credit on a structure, a feature's reclaim, and the tick's advance, which runs every unit's first order
   -- the walk, the build, the reclaim, the guard -- and the engine's script calls of each unit's turn.
   research/notes/impai/shared-core.md, tasim_work. Contract: tasim.h.

   Units are visited in creation order and every rule reads the WIKISIM its caller owns; no globals, no clock. */
#include <stdio.h>
#include <string.h>
#include "tasim.h"

/* INBUILDSTANCE, the gate a builder's work waits on (0x438700); open for a unit that runs no script */
static int stance(const WIKISIM* s, const WIKIUNIT* u)
{
    return !s->cob || !u->cob || tagpu_wikicob_value(s->cob, u->cob, WIKICOB_INBUILDSTANCE) != 0;
}

/* A UNIT APPEARS (a scene's spawn, a nanoframe placed): the create's script starts in the engine's
   order, Create run-now (0x485DE6), an extractor's SetSpeed run-later (0x437902), and for a unit
   made whole whose FBI says ActivateWhenBuilt the state setter's Activate (0x48612F). Its handle
   is the next in spawn order, the host's own numbering, so the facts carry its row while Create
   runs. */
void tasim_spawn(WIKISIM* s, WIKIUNIT* u)
{
    int n;
    if (!s->cob || s->ncob >= WIKICOB_UNITS) return;
    u->cob = ++s->ncob;
    n = tasim_facts(s);
    if (tagpu_wikicob_spawn(s->cob, u->type, s->fact, n) != u->cob) {
        u->cob = 0;
        s->ncob--;
        s->oom = 1;
        return;
    }
    if (tasim_def(s, u->type)->extracts > 0) {
        const int32_t a = tasim_extractor_speed(s, u);
        tasim_cob_call(s, u, "SetSpeed", WIKICOB_ARGS, 1, &a);
    }
    if (u->built >= 1 && tasim_def(s, u->type)->activate_when_built) tagpu_wikicob_activate(s->cob, u->cob, 1);
    tasim_notice_active(s, u);
}

/* ---- one tick. Units walk at MaxVelocity (world units a tick) as MOVEMENT below says, and stop
   once within BuildDistance of the footprint; a building fills at its builders' summed WorkerTime
   over its BuildTime (worker time a second), times the entry's pace, with no cost; a reclaim takes
   as long as building it would. Guards work on whatever their guarded unit works on this tick, so
   they are resolved after everyone else. ---- */

/* a builder in range works on st, and turns at once to face its centre (unchanged when it stands
   on the centre); its work waits on its stance (0x438700), at the work's start only (MobileBuild's
   work state 0x403E0C and the step 0x41BA60 never read +0x10F): no progress on a work until its
   script has set INBUILDSTANCE once, though the job is its own from now, which its StartBuilding
   goes for */
static void credit(WIKISIM* s, WIKIUNIT* u, WIKIUNIT* st, int how)
{
    const double dx = st->x - u->x, dz = st->z - u->z, d = sqrt(dx * dx + dz * dz);
    const double w = tasim_def(s, u->type)->worker_time / tasim_def(s, st->type)->build_time / TPS * s->PACE;
    if (d > 0) {
        u->hx = dx / d;
        u->hz = dz / d;
    }
    u->job = st;
    u->job_how = how;
    snprintf(u->job_id, sizeof u->job_id, "%s", st->id);
    if (!(u->stanced && !u->work_feat && !strcmp(u->work, st->id))) {
        if (!stance(s, u)) return;
        u->stanced = 1;
    }
    /* a reclaim says working once an order, when its work first goes on (ReclaimUnit's state after
       the stance wait, 0x4048BE); credit runs only for a unit with a first order */
    if (how == HOW_RECLAIM && u->norder && u->order[0].serial != u->said) {
        u->said = u->order[0].serial;
        tasim_notice(s, u, WIKI_SND_WORKING);
    }
    /* in a game, the build's request (tasim_econ_request): its increment's part of the structure still
       unbuilt after this tick's builders before it, as the engine's step updates +0x104 call by
       call, times the built type's build costs (0x41BB36..0x41BB5C); refused, no progress and no
       spray. The increment is worker_time / 30 / build_time where the engine's step takes
       floor(WorkerTime / 30), but the increments of a building sum to 1 in both, so a building
       costs its build costs */
    if (s->game && how == HOW_BUILD) {
        const double left = 1 - st->built - st->work_build;
        if (left > 0) {
            const double dd = w < left ? w : left;
            const WIKIDEF* b = tasim_def(s, st->type);
            if (!tasim_econ_request(u, dd * b->build_cost_energy, dd * b->build_cost_metal)) return;
        }
    }
    if (how == HOW_BUILD) st->work_build += w;
    else st->work_reclaim += w;
    st->worked = 1;
    /* the step made progress: the unit's turn sprays at the structure as it stands now */
    u->spray = how + 1;
    snprintf(u->spray_id, sizeof u->spray_id, "%s", st->id);
    u->spray_type = st->type;
    u->spray_x = st->x;
    u->spray_z = st->z;
}

/* the engine's arrival for a move: its goal test 0x44D310 passes when the unit's footprint cell is
   within ((order+0x36 + 4) >> 4) cells of the goal's (0x44CF60's radius, from Move_Ground
   0x403232..0x40323F); a click's move carries +0x36 = 0 (0x4991E2 pushes type 0 through 0x48CF30
   and 0x43AFC0, which 0x43A116 stores there), so the radius is 0: the goal's own cell */
static int in_goal_cell(const WIKISIM* s, const WIKIUNIT* u, double x, double z)
{
    return tasim_foot_cell(u->x, tasim_fpx(s, u->type)) == tasim_foot_cell(x, tasim_fpx(s, u->type)) &&
           tasim_foot_cell(u->z, tasim_fpz(s, u->type)) == tasim_foot_cell(z, tasim_fpz(s, u->type));
}

/* MobileBuild's reach when the builder's move gave up (event 0x40, 0x403B63..0x403C39): the
   whole world units between it and the site's centre, less trunc(8 x its footprint's diagonal)
   and trunc(8 x the site's), beyond its BuildDistance (def+0x212) */
static int out_of_reach(const WIKISIM* s, const WIKIUNIT* u, int t, int ox, int oz)
{
    const XZ c = tasim_centre(s, t, ox, oz);
    const double dx = u->x - c.x, dz = u->z - c.z;
    const double uf = sqrt((double)tasim_fpx(s, u->type) * tasim_fpx(s, u->type) + (double)tasim_fpz(s, u->type) * tasim_fpz(s, u->type));
    const double sf = sqrt((double)tasim_fpx(s, t) * tasim_fpx(s, t) + (double)tasim_fpz(s, t) * tasim_fpz(s, t));
    const double d = floor(sqrt(dx * dx + dz * dz)) - trunc(8.0 * uf) + trunc(-8.0 * sf);
    return d > tasim_def(s, u->type)->build_distance;
}

/* the countdown a Reclaim order sets on a feature (0x404BBA..0x404BDC): trunc(15 + (energy + metal)
   / 2) of the FeatureDef's floats (+0xEC, +0xF0), as tagpu_sweepgeo.c's sweep_reclaim_ticks reads
   them; the loader holds both below WIKI_FEAT_AMOUNT, so it fits an int */
static int reclaim_countdown(double energy, double metal)
{
    const double e = (float)energy, m = (float)metal;
    return (int)floor(15 + ((e > 0 ? e : 0) + (m > 0 ? m : 0)) / 2);
}

/* THE RECLAIM HANDLER ON A FEATURE, 0x404AD0, DISASSEMBLED (research/notes/exe-reverse-engineering.md,
   *The reclaim sweep's engine side*): every call looks the feature up at the order's point, and
   with none says "Reclamation failed" (kind 7, 0x404AF8) and ends. The walk until the footprints
   touch; a walk that gives up ends the order silent (result 8). Arrived, the countdown and
   StartBuilding; the stance wait (0x404C5A); the working reply once (0x404C6E); then a working
   call every 2 ticks (0x439E80(2) at 0x404C77) takes 2 off it, and above 15 sprays (0x404CB5).
   At 0 or below that call ends its state (result 1, 0x404C8B) with the next call armed 2 ticks
   on, and that call pays out and the feature goes (state 5, 0x404D60: 0x4237D0, FeatureDie) --
   after the entry's lookup, so a feature gone in between fails as above. The countdown, the next
   call's tick, the payout due and whether it paid are the unit's (rc_*). `o` is u's first order. */
static void feat_work(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* o)
{
    WIKIFEAT* f = tasim_feat_by_id(s, o->target);
    const int t = s->tick + 1;
    double dx, dz, d;
    if (!f) {
        tasim_shift_order(u);
        u->rc_serial = 0;
        tasim_notice(s, u, WIKI_SND_CANT);
        return;
    }
    if (u->rc_serial != o->serial) {
        const TARGET g = tasim_to_feat(o, f);
        const int r = tasim_go(s, u, &g);
        if (r == GO_STUCK) {
            tasim_shift_order(u);
            return;
        }
        if (r != GO_ARRIVED) return;
        u->rc_serial = o->serial;
        u->rc_c = reclaim_countdown(f->energy, f->metal);
        u->rc_next = t;
        u->rc_due = 0;
        u->rc_paid = 0;
    }
    dx = f->x - u->x;
    dz = f->z - u->z;
    d = sqrt(dx * dx + dz * dz);
    if (d > 0) {
        u->hx = dx / d;
        u->hz = dz / d;
    }
    u->job = NULL;
    u->job_how = HOW_FEATURE;
    snprintf(u->job_id, sizeof u->job_id, "%s", f->id);
    if (!(u->stanced && u->work_feat && !strcmp(u->work, f->id))) {
        if (!stance(s, u)) return;
        u->stanced = 1;
    }
    if (o->serial != u->said) {
        u->said = o->serial;
        tasim_notice(s, u, WIKI_SND_WORKING);
    }
    if (t < u->rc_next) return;
    u->rc_next = t + 2;
    if (u->rc_due) {
        u->rc_paid = 1;
        tasim_feat_remove(s, f);
        return;
    }
    u->rc_c -= 2;
    if (u->rc_c <= 0)
        u->rc_due = 1;
    else if (u->rc_c > 15) {
        u->spray = HOW_FEATURE + 1;
        u->spray_box[0] = f->ox;
        u->spray_box[1] = f->oz;
        u->spray_box[2] = f->fx;
        u->spray_box[3] = f->fz;
        u->spray_box[4] = f->height;
    }
}

void tasim_advance(WIKISIM* s)
{
    int i;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        u->job = NULL;
        u->job_id[0] = 0;
        u->worked = 0;
        /* went: it stepped on the last tick, so this tick's straight walks pass it as walking */
        u->went = u->stepped;
        u->stepped = 0;
        u->evacuated = 0;
        u->spray = 0;
        u->completed = 0;
        u->work_build = u->work_reclaim = 0;
    }
    s->nemit = 0;
    /* a structure placed here joins the list and is visited too, with no orders */
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        WIKIUNIT* st;
        WIKIORDER* o;
        int r;
        if (!tasim_mobile(s, u->type)) continue;
        o = u->norder ? &u->order[0] : NULL;
        /* the order's handler runs wherever the unit stands, so its ok comes before any step off
           a claim */
        if (o) tasim_begin(s, u, o);
        if (tasim_evacuate(s, u)) {
            u->evacuated = 1;
            continue;
        }
        if (!o || o->k == WIKI_ORDER_GUARD) continue;
        if (o->k == WIKI_ORDER_MOVE) {
            const TARGET t = tasim_to_point(o, o->x, o->z);
            int arrived;
            r = tasim_go(s, u, &t);
            if (r != GO_ARRIVED && r != GO_STUCK) continue;
            /* Move_Ground says arrived on its goal test's event 0x20 alone (0x4031EB..0x4031FB); a
               give-up (0x40) ends the order silent. A stuck move ends where the engine's mover
               would give up, unless it already stands in the goal's cell */
            arrived = r == GO_ARRIVED || in_goal_cell(s, u, o->x, o->z);
            tasim_shift_order(u);
            if (arrived) tasim_notice(s, u, WIKI_SND_ARRIVED);
            continue;
        }
        if (o->k == WIKI_ORDER_RECLAIM && o->feat) {
            feat_work(s, u, o);
            continue;
        }
        if (o->k == WIKI_ORDER_RECLAIM) {
            if (!(st = tasim_by_id(s, o->target))) { tasim_shift_order(u); continue; }
            if (tasim_reach(s, o, u, st->type, st->ox, st->oz)) credit(s, u, st, HOW_RECLAIM);
            continue;
        }
        st = tasim_structure_at(s, o->type, o->ox, o->oz);
        if (st && st->built >= 1) { tasim_shift_order(u); continue; }
        {
            const TARGET g = tasim_to_site(s, o, u, tasim_rect_of(s, o->type, o->ox, o->oz));
            r = tasim_go(s, u, &g);
        }
        if (r == GO_STUCK && out_of_reach(s, u, o->type, o->ox, o->oz)) {
            /* "I can't reach the construction site" (0x403C45) ends the order; a give-up within
               reach goes on to the site as an arrival does (0x403C59) */
            tasim_shift_order(u);
            tasim_notice(s, u, WIKI_SND_CANT);
            continue;
        }
        if (r != GO_ARRIVED && r != GO_STUCK) continue;
        if (!st) {
            const WIKIRECT rect = tasim_rect_of(s, o->type, o->ox, o->oz);
            int k, taken = 0;
            if (!tasim_site_ok(s, o->type, o->ox, o->oz, NULL)) {
                tasim_shift_order(u);
                tasim_notice(s, u, WIKI_SND_CANT);
                continue;
            }
            /* the site waits until no unit stands on it; those that do step off it, its claim
               (tasim_evacuate). MobileBuild's site test (0x47DB70 at 0x403CC7) fails on an occupied
               cell, and its first failure an order says cant ("Waiting for target area to
               clear", 0x403CDF) */
            for (k = 0; k < s->nunit && !taken; k++)
                taken = tasim_mobile(s, s->unit[k]->type) && tasim_hits(tasim_body(s, s->unit[k]), rect);
            if (taken) {
                if (u->cant != o->serial) {
                    u->cant = o->serial;
                    tasim_notice(s, u, WIKI_SND_CANT);
                }
                continue;
            }
            if (!(st = tasim_new_unit(s, o->type, u->owner, 0, NULL, NULL))) continue;
            tasim_seat_unit(s, st, o->ox, o->oz);
            tasim_notice(s, u, WIKI_SND_BUILD);
        }
        credit(s, u, st, HOW_BUILD);
    }
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        WIKIUNIT* g;
        WIKIORDER* o = u->norder ? &u->order[0] : NULL;
        if (!o || o->k != WIKI_ORDER_GUARD || u->evacuated) continue;
        if (!(g = tasim_by_id(s, o->target))) { tasim_shift_order(u); continue; }
        /* a guarded unit reclaiming a feature is followed: the wiki's guard helps only a
           structure's work (its job is NULL then) */
        if (g->job) {
            WIKIUNIT* j = g->job;
            if (tasim_reach(s, o, u, j->type, j->ox, j->oz)) credit(s, u, j, g->job_how);
        } else {
            const TARGET t = tasim_to_unit(s, o, g);
            tasim_go(s, u, &t);
        }
    }
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* st = s->unit[i];
        if (!st->worked) continue;
        if (st->work_build && st->built < 1) {
            st->built += st->work_build;
            if (st->built > 1) st->built = 1;
            st->completed = st->built >= 1;
            if (st->built >= 1 && st->owner == 0) s->nbuilt++;
            /* 0x41B8D0: a structure finished whose FBI says ActivateWhenBuilt is switched on (0x41B9A6) */
            if (st->built >= 1 && s->cob && st->cob && tasim_def(s, st->type)->activate_when_built)
                tagpu_wikicob_activate(s->cob, st->cob, 1);
        }
        if (st->work_reclaim) {
            st->reclaimed += st->work_reclaim;
            if (st->reclaimed > 1) st->reclaimed = 1;
            if (st->reclaimed >= 1) {
                if (s->cob && st->cob) tagpu_wikicob_remove(s->cob, st->cob);
                tasim_remove_unit(s, st);
                i--;
            }
        }
    }
    /* THE NOTICES OF THE WORK, in creation order whatever order the work was credited in: a change
       of ACTIVATION the completion made, then each builder whose build finished this tick, as each
       builder's handler finds the build complete (0x403F39, 0x40422F) */
    for (i = 0; i < s->nunit; i++) tasim_notice_active(s, s->unit[i]);
    for (i = 0; s->snd_on && i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        const WIKIUNIT* t = u->job_id[0] && u->job_how == HOW_BUILD ? tasim_by_id(s, u->job_id) : NULL;
        if (t && t->completed) tasim_notice(s, u, WIKI_SND_COMPLETE);
    }
    /* orders whose work is over end now, so the next one starts on the next tick */
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        const WIKIORDER* o = u->norder ? &u->order[0] : NULL;
        WIKIUNIT* st;
        if (!o) continue;
        if (o->k == WIKI_ORDER_BUILD) {
            if ((st = tasim_structure_at(s, o->type, o->ox, o->oz)) && st->built >= 1) tasim_shift_order(u);
        } else if (o->k == WIKI_ORDER_RECLAIM && o->feat) {
            /* a feature's reclaim ends with its payout (result 5); another reclaimer of it finds
               it gone on its next call */
            if (u->rc_serial == o->serial && u->rc_paid) {
                tasim_shift_order(u);
                u->rc_serial = 0;
            }
        } else if ((o->k == WIKI_ORDER_RECLAIM || o->k == WIKI_ORDER_GUARD) && !tasim_by_id(s, o->target))
            tasim_shift_order(u);
    }
    /* THE ENGINE'S CALLS OF THE UNIT'S TURN, sim.js's, unit by unit in creation order: its build
       calls, then its walk class. A builder's work is the structure it worked on this tick,
       waiting on its stance or not; when that changes StopBuilding goes for the old one (0x43A251,
       run-later, no argument: the order's destructor, in the same call as the build it finished)
       and StartBuilding for the new one (0x4385C7, run-later, one word: the target's angle from the
       builder's facing, 0 here since the builder faces it at once). A structure finished or
       reclaimed this tick is no one's work any more. Then the walk class (0x43DA70): a mobile unit
       that stepped this tick and stood the last starts StartMoving and MoveRate1 (no FBI the wiki
       reads sets moverate1, whose default 2 x MaxVelocity keeps its class 1), one that stood this
       tick and stepped the last StopMoving, both run-now. Jobs are found by id: a reclaim freed
       its target above. */
    for (i = 0; s->cob && i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        /* a feature's work holds while the feature stands */
        const int feat = u->job_id[0] && u->job_how == HOW_FEATURE;
        const WIKIUNIT* t = u->job_id[0] && !feat ? tasim_by_id(s, u->job_id) : NULL;
        const WIKIFEAT* tf = feat ? tasim_feat_by_id(s, u->job_id) : NULL;
        const char* tid;
        char ended[WIKI_ID_MAX] = "";
        if (t && u->job_how == HOW_BUILD && t->built >= 1) t = NULL;
        tid = t ? t->id : tf ? tf->id : NULL;
        if (u->work[0] && (!tid || u->work_feat != (tf != NULL) || strcmp(u->work, tid))) {
            tasim_cob_call(s, u, "StopBuilding", WIKICOB_LATER, 0, NULL);
            snprintf(ended, sizeof ended, "%s", u->work);
            u->work[0] = 0;
            u->work_serial = 0;
            u->work_feat = 0;
            u->stanced = 0;
        }
        if (tid && !u->work[0]) {
            const int32_t heading = 0;
            tasim_cob_call(s, u, "StartBuilding", WIKICOB_ARGS, 1, &heading);
            snprintf(u->work, sizeof u->work, "%s", tid);
            u->work_feat = tf != NULL;
            u->work_serial = u->norder ? u->order[0].serial : 0;
        }
        /* a work that ended this tick sprays its last step by its own cadence, which is forgotten
           only then -- unless this tick's spray was a new work's */
        if (u->spray) tasim_spray(s, u, s->tick + 1);
        if (ended[0] && !strcmp(u->spray_on, ended)) u->spray_at = 0;
        if (!tasim_mobile(s, u->type)) continue;
        if (u->stepped && !u->moving) {
            tasim_cob_call(s, u, "StartMoving", WIKICOB_NOW, 0, NULL);
            tasim_cob_call(s, u, "MoveRate1", WIKICOB_NOW, 0, NULL);
        } else if (!u->stepped && u->moving)
            tasim_cob_call(s, u, "StopMoving", WIKICOB_NOW, 0, NULL);
        u->moving = u->stepped;
    }
    /* a run-now start or a query in the turns can set ACTIVATION too */
    for (i = 0; i < s->nunit; i++) tasim_notice_active(s, s->unit[i]);
    /* no pointer to a reclaimed unit outlives the tick */
    for (i = 0; i < s->nunit; i++) s->unit[i]->job = NULL;
}

/* AN ORDER THE PLAYER TAKES AWAY ends its work at once: the order's destructor starts StopBuilding
   when the input is handled (0x43A251), before the next tick's slice */
void tasim_drop_work(WIKISIM* s)
{
    int i;
    for (i = 0; s->cob && i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        if (!u->work[0] || (u->norder ? u->order[0].serial : 0) == u->work_serial) continue;
        tasim_cob_call(s, u, "StopBuilding", WIKICOB_LATER, 0, NULL);
        u->work[0] = 0;
        u->work_serial = 0;
        u->work_feat = 0;
        u->stanced = 0;
        u->spray_at = 0;
    }
}
