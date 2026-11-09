#ifndef TAGPU_RESOURCE_H
#define TAGPU_RESOURCE_H
#include "tagpu_packet.h"

/* Game thread only. Mouse captures the repaired raw world point; preview
   publication changes the packet, never the engine's command mode. */
void tagpu_resource_init(void);
int tagpu_resource_live(void);
/* 1 when Quick Build's right-command gate on 0x499100 would take a right click on the record
   being handled (Interface Type 1, mode 1): the formation move leaves that press to it. */
int tagpu_resource_quick_right(void);
void tagpu_resource_mouse(void);
void tagpu_resource_level_end(void);
void tagpu_resource_packet(TAGPU_PACKET* packet);

#endif
