#!/usr/bin/env python3
"""Serve the rendered cuts over Tailscale for review on another device.

    promo/review-server.py <dir-of-mp4s> [--port 8800] [--script promo/tacli-promo.json]
                           [--bind tailscale|local|IP]

Binds to the **Tailscale address only** by default, so the page is reachable
from your own devices on the tailnet and from nowhere else. It does not
touch `tailscale serve` or `tailscale funnel` — Funnel in particular would publish
the video on the public internet, which is not what "share it with my phone" means.

Two things about serving video that are not optional, both learned by watching a
page fail in ways that look like broken files:

* **Range requests.** `http.server` answers none, and a `<video>` that cannot be
  range-served cannot be SEEKED — which is most of what reviewing a cut is.
* **Threading.** The page holds two renditions; a single-threaded server
  serialises them and the second stays black while the first streams.

With `--script`, the montage file's captions and camera keyframes become chapter
buttons, so a beat can be re-watched without hunting for it on the scrubber.
"""

import argparse
import html
import json
import os
import re
import socket
import subprocess
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

RANGE = re.compile(r"bytes=(\d*)-(\d*)")


class RangeHandler(SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler + 206 Partial Content."""

    def send_head(self):
        hdr = self.headers.get("Range")
        if not hdr:
            return super().send_head()
        path = self.translate_path(self.path)
        if os.path.isdir(path):
            return super().send_head()
        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(404)
            return None
        size = os.fstat(f.fileno()).st_size
        m = RANGE.match(hdr.strip())
        if not m:
            f.close()
            self.send_error(400)
            return None
        start, end = m.group(1), m.group(2)
        if start == "":                        # suffix range: the last N bytes
            start, end = max(0, size - int(end)), size - 1
        else:
            start = int(start)
            end = int(end) if end else size - 1
        end = min(end, size - 1)
        if start > end:
            f.close()
            self.send_error(416)
            return None
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        f.seek(start)
        return _Slice(f, end - start + 1)

    def end_headers(self):
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *a):
        sys.stderr.write(f"  {self.address_string()} {fmt % a}\n")


class _Slice:
    """A file object that stops after n bytes, so copyfile sends just the range."""

    def __init__(self, f, n):
        self.f, self.n = f, n

    def read(self, amt=-1):
        if self.n <= 0:
            return b""
        if amt is None or amt < 0 or amt > self.n:
            amt = self.n
        b = self.f.read(amt)
        self.n -= len(b)
        return b

    def close(self):
        self.f.close()


def tailscale_ip() -> str | None:
    try:
        out = subprocess.run(["tailscale", "ip", "-4"], capture_output=True,
                             text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return None
    ip = out.stdout.strip().splitlines()
    return ip[0].strip() if ip and out.returncode == 0 else None


def tailscale_host() -> str | None:
    try:
        out = subprocess.run(["tailscale", "status", "--json"],
                             capture_output=True, text=True, timeout=5)
        d = json.loads(out.stdout)
        name = d.get("Self", {}).get("DNSName", "").rstrip(".")
        return name or None
    except Exception:
        return None


def probe(path: Path) -> dict:
    """Resolution, duration and frame count straight from the file."""
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height,duration,nb_frames,r_frame_rate",
             "-of", "json", str(path)],
            capture_output=True, text=True, timeout=30)
        s = json.loads(out.stdout)["streams"][0]
        return {"w": int(s.get("width", 0)), "h": int(s.get("height", 0)),
                "dur": float(s.get("duration", 0) or 0),
                "fps": s.get("r_frame_rate", "")}
    except Exception:
        return {"w": 0, "h": 0, "dur": 0.0, "fps": ""}


def chapters(script: Path | None) -> list[dict]:
    """Beats worth jumping to, read off the montage script."""
    if not script or not script.exists():
        return []
    d = json.loads(script.read_text())
    out = [{"t": 0.0, "label": "Open"}]
    for w in d.get("windows", []):
        if "t_game" in w:
            out.append({"t": float(w["t_game"]),
                        "label": f"{w.get('label', 'window')} runs"})
    for c in d.get("captions", []):
        out.append({"t": float(c["t0"]),
                    "label": (c.get("text") or "")[:28]})
    for k in d.get("camera", []):
        if float(k.get("cols", 0)) >= 20:
            out.append({"t": float(k["t"]), "label": f"wide · {k['cols']:g} cols"})
    seen, uniq = set(), []
    for c in sorted(out, key=lambda c: c["t"]):
        key = round(c["t"], 1)
        if key in seen:
            continue
        seen.add(key)
        uniq.append(c)
    return uniq


PAGE = """<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>tacli promo — review</title>
<style>
  :root {{ color-scheme: dark; }}
  * {{ box-sizing: border-box; }}
  body {{ margin:0; background:#0b0d10; color:#e8ebef;
         font:15px/1.55 system-ui,-apple-system,Segoe UI,sans-serif; }}
  header {{ padding:18px 20px 10px; display:flex; gap:14px; align-items:baseline;
           flex-wrap:wrap; }}
  h1 {{ margin:0; font-size:18px; letter-spacing:.2px; }}
  .sub {{ color:#77818e; font-size:13px; }}
  main {{ padding:0 20px 40px; max-width:1600px; margin:0 auto; }}
  video {{ width:100%; display:block; background:#000; aspect-ratio:16/9;
          border-radius:10px; border:1px solid #222831; }}
  .row {{ display:flex; gap:10px; flex-wrap:wrap; align-items:center;
         margin:14px 0 6px; }}
  button {{ appearance:none; border:1px solid #2a3038; background:#1b2128;
           color:#cfd6de; padding:8px 13px; border-radius:8px;
           font:600 13px system-ui; cursor:pointer; }}
  button:hover {{ background:#232b34; }}
  button[aria-pressed=true] {{ background:#3a7bd5; border-color:#3a7bd5; color:#fff; }}
  .chips {{ display:flex; gap:7px; flex-wrap:wrap; margin-top:8px; }}
  .chips button {{ font-weight:500; font-size:12px; padding:6px 10px;
                  background:#151a20; }}
  .meta {{ color:#8b95a1; font-size:12.5px; margin-top:10px;
          font-variant-numeric:tabular-nums; }}
  .note {{ color:#6f7884; font-size:12.5px; margin-top:18px; max-width:70ch; }}
  .hint {{ color:#ffb454; font-size:13px; margin-top:12px; max-width:70ch;
          background:#231c10; border:1px solid #4a3a1c; border-radius:8px;
          padding:10px 13px; }}
  kbd {{ background:#1b2128; border:1px solid #2a3038; border-bottom-width:2px;
        border-radius:5px; padding:1px 5px; font:600 11px ui-monospace,monospace; }}
</style>

<header>
  <h1>tacli promo — review</h1>
  <span class="sub">{sub}</span>
</header>

<main>
  <video id="v" controls playsinline preload="metadata" src="{first_src}"></video>

  <div class="row" id="renditions"></div>
  <div class="chips" id="chapters"></div>
  <div class="meta" id="meta"></div>
  <p class="hint" id="hint" hidden>Nothing loading? Chrome will not fetch video
  into a <b>background tab</b> &mdash; it sits at readyState 0 with no error,
  which looks exactly like a broken file and is not one. Bring this tab to the
  front and it starts.</p>

  <p class="note">
    <kbd>space</kbd> play/pause · <kbd>←</kbd><kbd>→</kbd> 5 s ·
    <kbd>,</kbd><kbd>.</kbd> one frame · <kbd>1</kbd>–<kbd>9</kbd> chapters.
    Switching rendition keeps your place. Served on the tailnet only — not
    published.
  </p>
</main>

<script>
const RENDITIONS = {renditions};
const CHAPTERS = {chapters};
const v = document.getElementById('v');
const $ = id => document.getElementById(id);
let cur = 0;

function fmt(t) {{
  t = Math.max(0, t || 0);
  const m = Math.floor(t / 60), s = (t % 60).toFixed(1).padStart(4, '0');
  return `${{m}}:${{s}}`;
}}

RENDITIONS.forEach((r, i) => {{
  const b = document.createElement('button');
  b.textContent = r.label;
  b.setAttribute('aria-pressed', String(i === 0));
  b.onclick = () => pick(i);
  $('renditions').appendChild(b);
}});

function pick(i) {{
  if (i === cur) return;
  // Keep the playhead across a source swap — the point is comparing the same
  // moment at two resolutions.
  const at = v.currentTime, playing = !v.paused;
  cur = i;
  v.src = RENDITIONS[i].src;
  v.addEventListener('loadedmetadata', () => {{
    v.currentTime = at;
    if (playing) v.play().catch(() => {{}});
    paint();
  }}, {{once: true}});
  [...$('renditions').children].forEach((b, n) =>
    b.setAttribute('aria-pressed', String(n === i)));
}}

CHAPTERS.forEach((c, i) => {{
  const b = document.createElement('button');
  b.textContent = (i < 9 ? `${{i + 1}}. ` : '') + c.label + '  ' + fmt(c.t);
  b.onclick = () => {{ v.currentTime = c.t; v.play().catch(() => {{}}); }};
  $('chapters').appendChild(b);
}});

function paint() {{
  const r = RENDITIONS[cur];
  $('meta').textContent =
    `${{r.w}}\\u00d7${{r.h}} · ${{r.size}} · ${{fmt(v.duration)}} · ` +
    `at ${{fmt(v.currentTime)}}`;
}}
v.addEventListener('timeupdate', paint);
v.addEventListener('loadedmetadata', paint);

// CHROME DOES NOT LOAD MEDIA IN A HIDDEN TAB. A background tab sits at
// readyState 0 with networkState LOADING, duration NaN and NO ERROR, EVER --
// indistinguishable from a broken file, a broken encode or a broken server, and
// it is none of them (fetch() of the same URL succeeds in milliseconds the whole
// time). So say so on the page, and kick the load when the tab is actually
// looked at, instead of leaving a spinner that means nothing.
const hint = document.getElementById('hint');
function checkVisibility() {{
  const stuck = v.readyState === 0 && !v.error;
  if (document.visibilityState === 'hidden') return;
  hint.hidden = true;
  if (stuck) v.load();                 // it refused to load while hidden
}}
document.addEventListener('visibilitychange', checkVisibility);
setTimeout(() => {{
  if (v.readyState === 0 && !v.error) {{
    hint.hidden = false;               // still nothing after 3s
  }}
}}, 3000);
checkVisibility();

addEventListener('keydown', e => {{
  if (e.target.tagName === 'INPUT') return;
  if (e.code === 'Space') {{ e.preventDefault(); v.paused ? v.play() : v.pause(); }}
  else if (e.key === 'ArrowRight') v.currentTime += 5;
  else if (e.key === 'ArrowLeft') v.currentTime -= 5;
  else if (e.key === '.') {{ v.pause(); v.currentTime += 1 / 30; }}
  else if (e.key === ',') {{ v.pause(); v.currentTime -= 1 / 30; }}
  else {{
    const n = parseInt(e.key, 10);
    if (n >= 1 && n <= Math.min(9, CHAPTERS.length))
      {{ v.currentTime = CHAPTERS[n - 1].t; v.play().catch(() => {{}}); }}
  }}
}});
</script>
"""


def human(n: int) -> str:
    return f"{n / 1048576:.1f} MB" if n < 1 << 30 else f"{n / (1 << 30):.2f} GB"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--port", type=int, default=8800)
    ap.add_argument("--script", help="montage script, for chapter markers")
    ap.add_argument("--bind", default="tailscale",
                    help="tailscale (default), local, or an explicit address")
    args = ap.parse_args()

    root = Path(args.dir).resolve()
    if not root.is_dir():
        raise SystemExit(f"not a directory: {root}")
    vids = sorted(root.glob("*.mp4"), key=lambda p: p.stat().st_size)
    if not vids:
        raise SystemExit(f"no .mp4 in {root}")

    rends = []
    for p in vids:
        info = probe(p)
        rends.append({"src": p.name, "label": "", "w": info["w"],
                      "h": info["h"], "size": human(p.stat().st_size),
                      "dur": info["dur"], "stem": p.stem})
    # Label by RESOLUTION when the files are renditions of one cut, and by NAME
    # when they are not -- pointed at a directory of six source clips, "1536p"
    # four times over says nothing about which is which.
    heights = [r["h"] for r in rends]
    by_res = len(set(heights)) == len(heights)
    for r in rends:
        if by_res:
            r["label"] = "4K master" if r["h"] >= 2000 else f"{r['h']}p"
        else:
            r["label"] = r["stem"]
    rends.sort(key=lambda r: (r["h"], r["stem"]) if by_res else (r["stem"],))

    chaps = chapters(Path(args.script)) if args.script else []
    sub = " · ".join(f"{r['label']} {r['size']}" for r in rends)
    page = PAGE.format(
        sub=html.escape(sub),
        first_src=html.escape(rends[0]["src"]),
        renditions=json.dumps(rends),
        chapters=json.dumps(chaps),
    )
    (root / "index.html").write_text(page)

    if args.bind == "tailscale":
        host = tailscale_ip()
        if not host:
            raise SystemExit("no Tailscale address — is tailscaled up? "
                             "(`tailscale ip -4`). Use --bind local to serve "
                             "on 127.0.0.1 instead.")
    elif args.bind == "local":
        host = "127.0.0.1"
    else:
        host = args.bind

    handler = partial(RangeHandler, directory=str(root))
    try:
        srv = ThreadingHTTPServer((host, args.port), handler)
    except OSError as e:
        raise SystemExit(f"cannot bind {host}:{args.port}: {e}")
    srv.daemon_threads = True

    name = tailscale_host()
    print(f"serving {root}")
    for r in rends:
        print(f"  {r['label']:>10}  {r['w']}x{r['h']}  {r['size']}")
    print(f"\n  http://{host}:{args.port}/")
    if name and host != "127.0.0.1":
        print(f"  http://{name}:{args.port}/        (MagicDNS)")
    print(f"\n  bound to {host} only — reachable from your tailnet, nowhere else.")
    print("  Ctrl-C to stop.\n")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
