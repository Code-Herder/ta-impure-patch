#ifndef TAGPU_SEATCONTACT_H
#define TAGPU_SEATCONTACT_H
/* Every seat targets with its own contacts and its allies' (research/notes/modules/allied-sharing.md,
   *Per-seat contacts*). */

/* The acquisition's two calls, its radar read and UnitInPlayerLOS's sonar gate, byte-matched and
   all or none, in DllMain's sequence after tagpu_allysight_init, before the engine runs. */
void tagpu_seatcontact_init(void);

/* 1 once every site is written */
int tagpu_seatcontact_installed(void);

#define TAGPU_SEAT_CONTACT 0x01   /* the acquisition's radar list takes it: radar not jammed, or sight */
#define TAGPU_SEAT_SONAR   0x02   /* a sonar contact, not sonar-jammed                                */
#define TAGPU_SEAT_SEES    0x04   /* UnitInPlayerLOS(seat, unit), the sonar gate answered per seat    */

/* For a skirmish AI: what seat (a record 0..9) knows of every unit, as the acquisition computes it
   -- its own contacts and those of every owner whose contacts it receives
   (tagpu_allysight_contacts), never the viewed player's. out[slot] gets the TAGPU_SEAT_* bits of
   unit slot, 0 for an empty or dead slot, for slots 0..n-1 of the unit array; returns the slots
   written (n bounded by the array), 0 when not installed, seat is not 0..9 or an acquisition is
   running. One radar pass's cost per call, plus one UnitInPlayerLOS per live unit. The game thread
   only, outside the engine's unit step's acquisition. */
unsigned tagpu_seatcontact_scan(unsigned seat, unsigned char* out, unsigned n);

#endif
