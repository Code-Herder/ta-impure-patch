#ifndef TAGPU_FTIME_H
#define TAGPU_FTIME_H
/* tagpu_ftime.h -- WHAT A FRAME COSTS ON EACH LANE, in GPU time (Phase G, G19f
   landing 6).

   THE GATE'S OWN WORDING IS "frame time no worse than GL", AND WALL CLOCK
   CANNOT ANSWER IT. What is comparable is the GPU time a lane spends on its own
   passes, measured on the device by the device.

   IT USED TO BE A ONE-PROCESS COMPARISON AND IT IS NOT ANY MORE. Under route D
   the Vulkan lane drew into a window of its own beside the GL lane, in the same
   iteration of render_ogl.c's loop: a frame with the lane armed did BOTH lanes'
   work, so the wall time of one iteration was their sum while the two GPU
   brackets were separable and comparable. Landing 4d-1 deleted route D. The two
   halves can no longer be live in one process -- `report()`'s `vk/gl` ratio,
   which needs `s_glN > 0 && s_vkN > 0`, is therefore unreachable -- and each
   lane is now polled from its own backend: GL from render_ogl.c, Vulkan from
   render_vk.c. **What the numbers support now is a CROSS-BUILD comparison**:
   this build's Vulkan GPU time against a previous build's, or against a GL
   figure taken from a build that still had the GL renderer.
   [THE DELETION OF THE RATIO AND THE SECOND POLL SITE ARE FROM THE 4d-1
   LANDING REVIEW, which found this module inert on the only surviving lane.]

   TIMESTAMPS, NOT SCOPED QUERIES, and that is forced rather than chosen:
   GL_TIME_ELAPSED is a scoped query and only ONE may be active per target, and
   tagpu_restoreglsl.c already runs one around every restorer slice. A frame
   bracket of that kind would either fail to begin or break the restorer's. Two
   TIMESTAMP counters have no such rule, and they are also exactly what the
   Vulkan half does (`vkCmdWriteTimestamp`), so the two lanes are measured the
   same way rather than two ways that have to be argued equivalent.

   NOTHING EVER BLOCKS. The GL side polls GL_QUERY_RESULT_AVAILABLE and reads a
   frame's pair only once the driver says it is there; the Vulkan side is read
   behind the fence the seam ALREADY waits on before it re-records that slot, so
   its results cost no wait at all. A frame whose result is not ready yet is
   carried, never waited for -- a harness that stalls the thread it is measuring
   measures the stall.

   THE LEVER IS `tagpu_ftime.on` and nothing here runs without it: two queries a
   frame is cheap but it is not free, and a cost harness that is always on is a
   cost. Read live, like every other lever in this tree.

   `REPORT_FRAMES` counts frames that actually STAMPED a pair, not frames the
   lever was on for: a frame that found no free pair is skipped and does not
   advance the counter, so a report is "300 sampled frames" rather than "300
   armed frames".

   The reported figure is a percentile over the last `TAGPU_FTIME_RING` frames
   and NOT a mean: a GPU frame time distribution has a tail (a shader compile, a
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
