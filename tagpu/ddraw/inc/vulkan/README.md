# Vulkan-Headers — vendored, verbatim

Khronos' Vulkan API headers, pinned and copied into this tree so the DLL builds with nothing
but a mingw cross compiler. This file covers **both** vendored directories:

| directory | what |
|---|---|
| `inc/vulkan/` | `vulkan.h`, `vulkan_core.h`, `vk_platform.h`, `vulkan_win32.h` |
| `inc/vk_video/` | the seven video-codec headers `vulkan_core.h` includes unconditionally |

Both are required: `vulkan_core.h` does `#include "vk_video/…"`, which resolves through `-Iinc`
(already in the Makefile's `CFLAGS`), so the two directories must stay siblings under `inc/`.

## The pin

| | |
|---|---|
| upstream | <https://github.com/KhronosGroup/Vulkan-Headers> |
| tag | `v1.3.275` |
| commit | `217e93c664ec6704ec2d8c36fa116c1a4a1e2d40` (tagged 2024-01-05) |
| vendored | 11 headers, 1,075,570 bytes, byte-for-byte upstream |

**Why this tag and not the newest.** It matches the Vulkan loader on the reference setup
(`1.3.275`), and it is the version every measurement behind Phase G was taken with — device
enumeration, surface creation, and the swapchain/present gate, at both bitnesses, under two
wines. Newer tags were checked and change nothing that matters here: the ray-tracing extensions
this project may eventually want arrived in 1.2.162, so they are already declared.

## Why vendored rather than a package

CI installs `gcc-mingw-w64-i686`, `make` and `zip` and nothing else
(`.github/workflows/build.yml`). A `libvulkan-dev` build dependency would break the release
build, and an unpinned system header would make the DLL's contents depend on the machine that
built it. This is the same reason `glcorearb.h` and `ddraw.h` are already here. (`d3d9shader.h` was a
third example until landing 11-1 deleted the Direct3D9 lane.)

## Licence — Apache-2.0, and it is NOT this repository's MIT

Every one of the 11 vendored headers carries `SPDX-License-Identifier: Apache-2.0`. Upstream's
own `LICENSE.md` (kept beside this file as `LICENSE.upstream.md`) notes that some files in that
repository are additionally MIT — **none of the ones used here are**. Checked again at `v1.4.309`
and `v1.4.321`: still Apache-2.0 only. There is no newer tag that makes these MIT.

This is fine and needs nothing renegotiated. Apache-2.0 code may be distributed inside an
MIT-licensed project; the project stays MIT, these files stay Apache-2.0, and they are not
relicensed. The obligations it does create are all discharged here:

- **A copy of the licence** — `LICENSE.txt` beside this file, the full Apache-2.0 text.
- **Retained notices** — every header keeps its Khronos copyright and SPDX line, because the
  files are copied verbatim.
- **Modified files marked** — none are modified. **Do not edit them.** If something needs
  changing, change the tag and re-fetch.
- **NOTICE file** — upstream ships none (checked: `NOTICE` and `NOTICE.txt` both 404), so there
  is nothing to reproduce.

Two consequences worth knowing rather than rediscovering. Apache-2.0 is **incompatible with
GPLv2** (GPLv3+ only), which constrains anyone downstream who wanted to combine this source with
GPLv2-only code — nothing in this project's dependency graph is GPLv2. And Apache-2.0 carries an
**express patent grant** that MIT does not, which for these files leaves us better covered, not
worse.

## Re-fetching or bumping the tag

Verbatim copies, so this is the whole procedure:

```sh
TAG=v1.3.275          # change this to bump
cd tagpu/ddraw/inc
for f in vulkan.h vk_platform.h vulkan_core.h vulkan_win32.h; do
    curl -sSf -o "vulkan/$f" \
      "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$TAG/include/vulkan/$f"
done
for f in vulkan_video_codecs_common.h \
         vulkan_video_codec_h264std.h vulkan_video_codec_h264std_decode.h \
         vulkan_video_codec_h264std_encode.h vulkan_video_codec_h265std.h \
         vulkan_video_codec_h265std_decode.h vulkan_video_codec_h265std_encode.h; do
    curl -sSf -o "vk_video/$f" \
      "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$TAG/include/vk_video/$f"
done
curl -sSf -o vulkan/LICENSE.txt \
  "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$TAG/LICENSES/Apache-2.0.txt"
curl -sSf -o vulkan/LICENSE.upstream.md \
  "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/$TAG/LICENSE.md"
```

After bumping, **re-check the SPDX lines** (`grep -h SPDX-License-Identifier vulkan/*.h
vk_video/*.h | sort -u`) before assuming the licence section above still holds, and update the
commit and byte count in the pin table. Then rebuild `tools/vkpresent.c` for both bitnesses and
re-run it — that is the gate these headers exist to serve, and it is cheap:

```sh
i686-w64-mingw32-gcc   -std=c99 -O1 -Wall -Itagpu/ddraw/inc -o vkpresent32.exe tools/vkpresent.c
x86_64-w64-mingw32-gcc -std=c99 -O1 -Wall -Itagpu/ddraw/inc -o vkpresent64.exe tools/vkpresent.c
```

## What includes these today

Nothing in the DLL yet — Phase G / G19a is the first consumer. `tools/vkpresent.c` uses them
already, and is how the headers were proven to build under both mingw targets.
