#ifndef TAGPU_FTIME_H
#define TAGPU_FTIME_H
/* tagpu_ftime.h -- WHAT A FRAME COSTS ON EACH LANE, in GPU time (Phase G, G19f
   landing 6).

   THE GATE'S OWN WORDING IS "frame time no worse than GL", AND WALL CLOCK
   CANNOT ANSWER IT. What is comparable is the GPU time a lane spends on its own
   passes, measured on the device by the device.

   IT USED TO BE A ONE-PROCESS COMPARISON AND THERE IS NOW ONLY ONE LANE AT
   ALL. Under route D the Vulkan lane drew into a window of its own beside the
   GL lane, in the same iteration of render_ogl.c's loop: a frame with the lane
   armed did BOTH lanes' work, so the wall time of one iteration was their sum
   while the two GPU brackets were separable and comparable. **Landing 4d-1
   deleted route D**, which ended the one-process comparison; **landing 11-2
   deleted render_ogl.c itself**, which ended the GL lane. (Those are two
   different landings and this file has conflated them before -- the 11-5e-1
   review's MEDIUM-3. 4d-1 = route D, 11-2 = the backend.) Since 11-2 the only
   lane polled here is the Vulkan one, from render_vk.c.
   **What the numbers support is a CROSS-BUILD comparison**: this build's
   Vulkan GPU time against a previous build's, or against a GL figure taken
   from a build that still had the GL renderer.
   [THE RATIO AND THE SECOND POLL SITE WERE FOUND INERT BY THE 4d-1 LANDING
   REVIEW; the GL half they belonged to was deleted in 11-5e-1.]

   TIMESTAMPS, NOT SCOPED QUERIES, and that was forced rather than chosen.
   Kept because it is the reason the surviving lane looks the way it does:
   GL_TIME_ELAPSED is a scoped query and only ONE may be active per target, and
   tagpu_restoreglsl.c runs one around every restorer slice, so a GL frame
   bracket of that kind would either fail to begin or break the restorer's. Two
   TIMESTAMP counters have no such rule, and they are exactly what the Vulkan
   half does (`vkCmdWriteTimestamp`) -- which is why the two lanes were
   measured the same way rather than two ways that had to be argued
   equivalent, and why this lane's shape needs no revisiting now it is alone.

   NOTHING EVER BLOCKS. The result is read behind the fence the seam ALREADY
   waits on before it re-records that slot, so it costs no wait at all. A frame
   whose result is not ready yet is carried, never waited for -- a harness that
   stalls the thread it is measuring measures the stall. (The deleted GL half
   honoured the same rule a different way, by polling
   GL_QUERY_RESULT_AVAILABLE and reading a pair only once the driver said it
   was there. The rule is the design; that was one implementation of it.)

   THE LEVER IS `tagpu_ftime.on` and nothing here runs without it: two queries a
   frame is cheap but it is not free, and a cost harness that is always on is a
   cost. Read live, like every other lever in this tree.

   `REPORT_FRAMES` counts frames that actually STAMPED a pair, not frames the
   lever was on for: a frame that found no free pair is skipped and does not
   advance the counter, so a report is "300 sampled frames" rather than "300
   armed frames".

   The reported figure is a percentile over the last `RING` frames (256, in
   tagpu_ftime.c -- this line said `TAGPU_FTIME_RING`, a macro that has never
   existed anywhere in the tree) and NOT a mean: a GPU frame time distribution has a tail (a shader compile, a
   restorer slice, the compositor) and a mean over a few hundred frames is
   whichever tail happened to land in the window. p50 is what "a frame costs"
   means; p99 is what says whether the two lanes have the same SHAPE and not
   merely the same middle. */

/* Once per frame on the render thread, before anything draws: reads the lever.
   Cheap -- the file attribute is polled on a timer, like tagpu_opt's. */
void tagpu_ftime_poll(void);
int  tagpu_ftime_armed(void);

/* THE GL LANE'S BRACKET IS GONE (11-5e-1), and with it the second ring, the
   query pool and the `s_glDrives` arbitration. `tagpu_ftime_gl_begin` and
   `_gl_end` had exactly one caller between them -- render_ogl.c -- and landing
   4d-1 deleted that backend, so from 4d-1 onward they were code no build could
   reach and a ring no frame could fill. What they recorded is worth keeping
   even though the code is not:

     - `glQueryCounter` made it an ELAPSED SPAN ON THE GPU TIMELINE, not a busy
       counter: anything that stalled inside the bracket (the render thread's
       own critical section, the restorer's synchronising read-back) counted
       whether the GPU was working or idle. On a saturated frame the two
       coincide; on an idle one that number was the frame, not the work. The
       Vulkan lane's timestamps have the same property, so say which a fixture
       is before quoting a figure from it -- that caveat did not go away with
       the bracket, it moved lanes.
     - It ended immediately before `tagpu_vk_frame` and NOT before the buffer
       swap, so that the GL bracket never contained the other lane's submit.
       [The looser "before the buffer swap" wording was caught by the landing-6
       review; render_ogl.c's own comment had it right.]

   Nothing is ported: a bracket needs two lanes to be worth having, and a build
   now has one. */

/* The Vulkan lane's figure, handed in by the seam once it has resolved a
   slot's two timestamps behind its own fence wait. Nanoseconds, already
   multiplied by the device's `timestampPeriod`. */
void tagpu_ftime_vk_sample(double ns);

/* The seam calls this when it brings the lane down, so a figure from a lane
   that no longer exists is not averaged into one that does. */
void tagpu_ftime_vk_reset(void);
#endif
