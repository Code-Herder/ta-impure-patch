#ifndef TAGPU_GUI_RESET_H
#define TAGPU_GUI_RESET_H

/* Render-thread owned. A reset is owed until a non-lost record containing it
   is handed to the consumer; recording or publishing alone is not delivery. */
typedef struct TagpuGuiReset {
    int owed, recorded;
} TagpuGuiReset;

static inline void tagpu_gui_reset_arm(TagpuGuiReset* gate)
{
    gate->owed = 1;
    gate->recorded = 0;
}

static inline int tagpu_gui_reset_begin(TagpuGuiReset* gate)
{
    int retry = gate->owed && gate->recorded;
    gate->recorded = 0;
    return retry;
}

static inline int tagpu_gui_reset_ready(const TagpuGuiReset* gate)
{
    return !gate->owed || gate->recorded;
}

static inline void tagpu_gui_reset_record(TagpuGuiReset* gate)
{
    gate->recorded = 1;
}

static inline void tagpu_gui_reset_deliver(TagpuGuiReset* gate, int lost)
{
    if (!lost && gate->recorded) gate->owed = 0;
}

#endif
