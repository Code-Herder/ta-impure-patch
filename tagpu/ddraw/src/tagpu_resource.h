#ifndef TAGPU_RESOURCE_H
#define TAGPU_RESOURCE_H
#include "tagpu_packet.h"

/* Game thread only. Mouse captures the repaired raw world point; preview
   publication changes the packet, never the engine's command mode. */
void tagpu_resource_init(void);
int tagpu_resource_live(void);
void tagpu_resource_mouse(void);
void tagpu_resource_level_end(void);
void tagpu_resource_packet(TAGPU_PACKET* packet);

#endif
