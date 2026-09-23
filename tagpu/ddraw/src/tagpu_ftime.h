#ifndef TAGPU_FTIME_H
#define TAGPU_FTIME_H
/* tagpu_ftime.h -- WHAT A FRAME COSTS, in GPU time.

   WALL CLOCK CANNOT ANSWER "what does this change cost a frame". What is
   comparable is the GPU time the Vulkan lane spends on its own passes,
   measured on the device by the device, polled from render_vk.c.
   **What the numbers support is a CROSS-BUILD comparison**: this build's
   Vulkan GPU time against a previous build's.

   TIMESTAMPS, NOT SCOPED QUERIES: two `vkCmdWriteTimestamp` counters a frame.

   NOTHING EVER BLOCKS. The result is read behind the fence the seam ALREADY
   waits on before it re-records that slot, so it costs no wait at all. A frame
   whose result is not ready yet is carried, never waited for -- a harness that
   stalls the thread it is measuring measures the stall.

   THE LEVER IS `tagpu_ftime.on` and nothing here runs without it: two queries a
   frame is cheap but it is not free, and a cost harness that is always on is a
   cost. Read live, like every other lever in this tree.

   `REPORT_FRAMES` counts frames that actually STAMPED a pair, not frames the
   lever was on for: a frame that found no free pair is skipped and does not
   advance the counter, so a report is "300 sampled frames" rather than "300
   armed frames".

   The reported figure is a percentile over the last `RING` frames (256, in
   tagpu_ftime.c) and NOT a mean: a GPU frame time distribution has a tail (a
   shader compile, a restorer slice, the compositor) and a mean over a few
   hundred frames is whichever tail happened to land in the window. p50 is what
   "a frame costs" means; p99 is what says whether two builds have the same
   SHAPE and not merely the same middle. */

/* Once per frame on the render thread, before anything draws: reads the lever.
   Cheap -- the file attribute is polled on a timer, like tagpu_opt's. */
void tagpu_ftime_poll(void);
int  tagpu_ftime_armed(void);

/* The timestamps make it an ELAPSED SPAN ON THE GPU TIMELINE, not a busy
   counter: anything that stalls between them counts whether the GPU was
   working or idle. On a saturated frame the two coincide; on an idle one the
   number is the frame, not the work -- so say which a fixture is before
   quoting a figure from it. */

/* The Vulkan lane's figure, handed in by the seam once it has resolved a
   slot's two timestamps behind its own fence wait. Nanoseconds, already
   multiplied by the device's `timestampPeriod`. */
void tagpu_ftime_vk_sample(double ns);

/* The seam calls this when it brings the lane down, so a figure from a lane
   that no longer exists is not averaged into one that does. */
void tagpu_ftime_vk_reset(void);
#endif
