#!/usr/bin/env python3
"""modslaycheck.py -- the DLL's Mods screen (tagpu_modslay.c) against the mockup's (tools/mods/mockup/).

    python3 tools/mods/modslaycheck.py [--keep DIR] [--shots DIR]
    python3 tools/mods/modslaycheck.py --check-only

First, and alone with --check-only (no browser), the layout's load-time check, tagpu_modslay_check as the game runs it: the
fullest page must build whole (modslaycheck.c --check).

For each case below it opens the mockup headless (playwright-cli, as tools/wiki/wikilaycheck.py does, whose helpers this
imports) at the case's resolution, one CSS pixel a device pixel -- the game's grid -- with the scrollbars a real browser shows
(Playwright's headless default hides them, and a hidden scrollbar takes no room). It takes the page's state and every element
in units, runs modslaycheck.c -- tagpu_modslay.c compiled natively over the sources under art/ -- on the same state, and pairs
the two: every part by name, every control by its key, every disc by its colour, the backdrop by its colour, every text line by
its string, with its start, its width and its baseline, each panel's scroll limit, and each scrollbar thumb against the one
Chrome paints. A part or a line must also agree with the opacity it is drawn at: that is what greys a row and Confirm. It prints
the worst difference per kind and every element only one side has, and exits 1 when anything differs by more than
wikilaycheck.TOL_CSS or is unmatched.

A baseline that differs where Chrome rounds the face's ascent or descent to another whole pixel than round(em x size) does, and
that the same rule on Chrome's own metrics reproduces, is named, not failed, as wikilaycheck.py does. A character no HUD face has
fails the check, naming it: the game would draw no glyph for it.

--shots DIR writes a picture of each case's frame to DIR, for looking at; it needs Pillow to cut the frame out.

The standard library, gcc and playwright-cli; it serves the repository root itself.
"""
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "wiki"))
import wikilaycheck as wl  # noqa: E402

SRC = ROOT / "tagpu" / "ddraw" / "src"
TOL_CSS, TOL_THUMB_PX = wl.TOL_CSS, wl.TOL_THUMB_PX
# an element's opacity, out of 255: the two sides round 0.45 x 255 apart by less than this
TOL_ALPHA = 1.0
SCREENS = [(1024, 768), (1280, 720), (1920, 1080), (3840, 2160), (3440, 1440)]

# (fixture state, resolution, what is done to the page before it is read): held -- a control held down, by its key; scroll --
# each panel's scroll, in pixels, or "max" or "half" of its limit
CASES = (
    [(st, res, {}) for st in ("manage", "explore", "updates", "empty") for res in SCREENS]
    + [("long", res, {}) for res in SCREENS[:3]]
    + [("long", (1920, 1080), {"scroll": {"detail": "half"}}), ("long", (1024, 768), {"scroll": {"detail": "max"}}),
       ("long", (3840, 2160), {"scroll": {"detail": 137}})]
    + [("fullest", res, {}) for res in SCREENS]
    + [("fullest", (1024, 768), {"scroll": {"list": "max", "detail": "max"}}), ("fullest", (3840, 2160), {"scroll": {"list": "half", "detail": "half"}}),
       ("fullest", (1280, 720), {"scroll": {"detail": 333}}),
       ("fullest_broken", (1024, 768), {}), ("fullest_broken", (1920, 1080), {"scroll": {"list": "max", "detail": "half"}}),
       ("fullest_locked", (1280, 720), {}), ("fullest_locked", (3440, 1440), {"scroll": {"detail": "max"}})]
    + [("manage", (1920, 1080), {"held": "confirm"}), ("manage", (1920, 1080), {"held": "tab:0"}), ("manage", (1024, 768), {"held": "tab:1"}),
       ("manage", (1920, 1080), {"held": "act:2"}), ("manage", (1920, 1080), {"held": "row:3"}), ("manage", (1920, 1080), {"held": "opt:0:-"}),
       ("manage", (1920, 1080), {"held": "opt:2:t"}), ("manage", (2560, 1440), {"held": "reset:0"}), ("manage", (2560, 1440), {"held": "swreset:3"}),
       ("manage", (2560, 1440), {"held": "sw:4"}), ("manage", (3840, 2160), {"held": "back"}), ("explore", (1920, 1080), {"held": "act:0"}),
       ("updates", (1920, 1080), {"held": "updateall"})]
)
# the panels, in the order of MODSPAGE.scroll
SCROLLS = {"list": "#list", "detail": "#detail"}

COLLECT = r"""async () => {
  const A = ACTIONS, CPS = FALLBACK, ROLES = ROLEMAP;
  const raf = () => new Promise(r => requestAnimationFrame(() => r()));
  const $ = s => document.querySelector(s), SCR = { list: '#list', detail: '#detail' };
  const L = await fetch('../../../art/hud/layout.json').then(r => r.json());
  await raf(); await raf();
  for (const [k, v] of Object.entries(A.scroll || {})) {
    const e = $(SCR[k]), mx = e.scrollHeight - e.clientHeight;
    e.scrollTop = v === 'max' ? mx : v === 'half' ? Math.round(mx / 2) : v;
  }
  await raf(); await raf();

  const f = $('#frame').getBoundingClientRect(), u = f.height / 720;
  const R = r => [(r.left - f.left) / u, (r.top - f.top) / u, r.width / u, r.height / u];
  const B = e => R(e.getBoundingClientRect());
  const client = e => { const r = e.getBoundingClientRect(), sb = e.offsetWidth - e.clientWidth;
    return [(r.left - f.left) / u, (r.top - f.top) / u, (r.width - sb) / u, r.height / u]; };
  const overlapY = (a, k) => a[1] < k[1] + k[3] && a[1] + a[3] > k[1];
  const inter = (a, k) => { const x0 = Math.max(a[0], k[0]), y0 = Math.max(a[1], k[1]), x1 = Math.min(a[0] + a[2], k[0] + k[2]), y1 = Math.min(a[1] + a[3], k[1] + k[3]);
    return [x0, y0, Math.max(0, x1 - x0), Math.max(0, y1 - y0)]; };
  const clipOf = e => { const s = e.closest('.scr'); if (s) return client(s); const k = e.closest('.wt'); return k ? B(k) : null; };
  // the opacity an element is drawn at, out of 255: its own and each ancestor's
  const alpha = e => { let a = 255; for (let n = e; n && n !== document.body; n = n.parentElement) a *= +getComputedStyle(n).opacity; return a; };
  const els = [];
  els.push({ kind: 'solid', rgb: getComputedStyle($('#backdrop')).backgroundColor, r: B($('#backdrop')) });
  for (const e of document.querySelectorAll('#frame [data-part]')) {
    if (!e._pk) continue;
    const r = B(e), k = clipOf(e);
    if (k && !overlapY(r, k)) continue;
    els.push({ kind: 'part', part: e._pk.split('|')[1], r, alpha: alpha(e) });
    // uikit.js paints a box's glyph over the middle 60 % of it, as ui_glyph does
    if (e.dataset.glyph) els.push({ kind: 'part', part: e.dataset.glyph, r: [r[0] + r[2] * .2, r[1] + r[3] * .2, r[2] * .6, r[3] * .6], alpha: alpha(e) });
  }
  const hot = (e, key, box) => { let r = box || B(e); const k = clipOf(e); if (k && e.closest('.scr') !== e) r = inter(r, k);
    if (r[2] > 0 && r[3] > 0) els.push({ kind: 'hot', key, r }); };
  for (const [k, sel] of Object.entries(SCR)) hot($(sel), 'scroll:' + k, client($(sel)));
  for (const e of document.querySelectorAll('[data-key]')) hot(e, e.dataset.key);
  // the discs: a lock's 1 px border round a fill, a marker's one fill
  for (const e of document.querySelectorAll('#win .lock')) {
    const r = B(e), k = clipOf(e), p = 1 / u;
    if (k && !overlapY(r, k)) continue;
    els.push({ kind: 'dot', rgb: '5a0808', r }, { kind: 'dot', rgb: 'd01818', r: [r[0] + p, r[1] + p, r[2] - 2 * p, r[3] - 2 * p] });
  }
  for (const e of document.querySelectorAll('#win .mark')) {
    const r = B(e), k = clipOf(e);
    if (!k || overlapY(r, k)) els.push({ kind: 'dot', rgb: 'e2a030', r });
  }
  // every line of text: its characters grouped by baseline, each character's baseline its box's top plus the ascent of its face
  const ctx = document.createElement('canvas').getContext('2d');
  const walker = document.createTreeWalker($('#win'), NodeFilter.SHOW_TEXT);
  for (let t = walker.nextNode(); t; t = walker.nextNode()) {
    const e = t.parentElement;
    if (!t.data.trim()) continue;
    const cs = getComputedStyle(e);
    ctx.font = `${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`;
    const lines = [], rg = document.createRange();
    for (let i = 0; i < t.data.length; i++) {
      rg.setStart(t, i); rg.setEnd(t, i + 1);
      const rs = rg.getClientRects(); if (!rs.length) continue;
      const m = ctx.measureText(t.data[i]), base = rs[0].top + m.fontBoundingBoxAscent;
      let L = lines.find(l => l.last === i - 1 && Math.abs(l.base - base) < 2);
      if (!L) { L = { s: '', base, top: rs[0].top, bot: rs[0].bottom, x0: Infinity, x1: -Infinity, last: i }; lines.push(L); }
      L.s += t.data[i]; L.last = i;
      if (t.data[i] !== ' ') { L.x0 = Math.min(L.x0, rs[0].left); L.x1 = Math.max(L.x1, rs[0].right); }
    }
    const k = clipOf(e), fm = ctx.measureText('M'), a = alpha(e);
    for (const L of lines) {
      const s = L.s.trim(); if (!s) continue;
      const box = [(L.x0 - f.left) / u, (L.top - f.top) / u, (L.x1 - L.x0) / u, (L.bot - L.top) / u];
      if (k && !overlapY(box, k)) continue;
      els.push({ kind: 'text', text: s, r: box, base: (L.base - f.top) / u, size: parseFloat(cs.fontSize) / u, alpha: a,
                 asc: fm.fontBoundingBoxAscent, desc: fm.fontBoundingBoxDescent, colour: cs.color });
    }
  }
  // the browser's advance of each code point the HUD faces lack, in em, by style
  const fallback = {};
  for (const sn of new Set(Object.values(ROLES))) {
    const st = L.styles[sn]; fallback[sn] = {};
    ctx.font = `${st.weight} 100px '${st.font}', sans-serif`;
    for (const c of CPS) fallback[sn][c.codePointAt(0)] = ctx.measureText(c).width / 100;
  }
  // the reference string's width at each style's size, from which the C side finds the size Chrome sets the face at
  const sizes = {};
  for (const sn of new Set(Object.values(ROLES))) {
    const st = L.styles[sn], px = Math.fround(st.size * u);
    ctx.font = `${st.weight} ${px}px '${st.font}'`;
    sizes[sn] = { [px.toFixed(4)]: ctx.measureText('Hold Shift to queue one deposit after another').width };
  }
  const scroll = {};
  for (const [k, sel] of Object.entries(SCR)) { const e = $(sel);
    scroll[k] = { top: e.scrollTop / u, max: (e.scrollHeight - e.clientHeight) / u, bar: e.offsetWidth - e.clientWidth,
                  page: [e.getBoundingClientRect().right - (e.offsetWidth - e.clientWidth), e.getBoundingClientRect().top, e.clientHeight] }; }
  const P = ModsPage.page(), win = B($('#win'));
  const state = { ...P, res: ModsPage.state.res, held: ModsPage.state.held, W: f.width, H: f.height, scroll: ['list', 'detail'].map(k => scroll[k].top), fallback, sizes };
  return JSON.stringify({ state, els, u, scroll, window: win, frame: [f.left, f.top] });
}"""


def lacking():
    """Every character of the screen's strings that some HUD face lacks: the mockup's own, fixture and all."""
    have = None
    for f in (ROOT / "art/hud/fonts").glob("*.ttf"):
        cps = wl.cmap(f)
        have = cps if have is None else have & cps
    text = (ROOT / "tools/mods/mockup/mods.js").read_text()
    return sorted({ch for ch in text if ord(ch) > 0x7E and ord(ch) not in have})


def css_rgb(s):
    """`rgb(r, g, b)` as the hex the ops carry."""
    return "".join(f"{int(v):02x}" for v in s[s.index("(") + 1:s.index(")")].split(",")[:3])


def compare(name, dom, ops, faces, fallback):
    """Pair one case's two sides, printing the worst per kind. Returns (disagreements, the strings set in a fallback face)."""
    u = dom["u"]
    TOL = TOL_CSS / u
    bad, glyphs = 0, set()
    print(f"{name}: {len(ops['ops'])} ops, {len(dom['els'])} mockup elements, tolerance {TOL:.3f} units")
    if ops["overflow"]:
        bad += 1
        print("  the DLL's ops overflowed <-- DIFFERS")
    cs = {"part": [], "hot": [], "dot": [], "text": [], "solid": [], "thumb": []}
    for o in ops["ops"]:
        o["r"] = [o["x"], o["y"], o["w"], o["h"]]
        if o["kind"] == "solid" and o["rgba"] == "808078b3":
            cs["thumb"].append(o)
        elif o["kind"] in cs:
            cs[o["kind"]].append(o)
    ds = {k: [e for e in dom["els"] if e["kind"] == k] for k in cs}
    for c in cs["dot"] + cs["solid"]:
        c["rgb"] = c["rgba"][:6]
    for d in ds["solid"]:
        d["rgb"] = css_rgb(d["rgb"])
    keys = {"part": lambda e: e["part"], "hot": lambda e: e["key"], "dot": lambda e: e["rgb"], "solid": lambda e: e["rgb"], "text": lambda e: e["text"]}
    for k in ("part", "hot", "dot", "solid", "text"):
        pairs, oc, od = wl.pair(cs[k], ds[k], keys[k])
        extra = ""
        if k == "text":
            for c, d in pairs:
                c["cmp"] = [c["x"], c["w"]]
                d["cmp"] = [d["r"][0], d["r"][2]]
            worst = max((max(abs(a - b) for a, b in zip(c["cmp"], d["cmp"])) for c, d in pairs), default=0)

            def chrome_rounds(c, d):
                s = d["size"] * u
                a, de = faces[c["style"]]
                return (math.floor(a * s + 0.5), math.floor(de * s + 0.5)) != (round(d["asc"]), round(d["desc"]))
            off = [(c, d) for c, d in pairs if abs(c["base"] - d["base"]) > TOL]
            metric = [(c, d) for c, d in off if chrome_rounds(c, d) and abs(abs(c["base"] - d["base"]) * u - 1) <= TOL_CSS]
            rule = [p for p in off if p not in metric]
            bl = max((abs(c["base"] - d["base"]) for c, d in pairs if (c, d) not in metric), default=0)
            sz = max((abs(c["size"] - d["size"]) for c, d in pairs), default=0)
            extra = f", size within {sz:.4f}, baseline within {bl:.4f}"
            if metric:
                extra += f", Chrome's metric rounding moves {len(metric)} by a pixel"
            ink = [(c, d) for c, d in pairs if c["ink"] != (d["colour"] == "rgb(29, 29, 27)")]
            if ink:
                extra += f", ink disagrees {len(ink)}"
            for c, d in sorted(rule, key=lambda p: -abs(p[0]["base"] - p[1]["base"]))[:3]:
                print(f"        {c['text']!r} ({c['style']}): baseline DLL {c['base']:.3f} mockup {d['base']:.3f}")
            bad += bool(rule) + (sz > TOL) + len(ink)
            glyphs |= {c["text"] for c, d in pairs if any(ch in fallback for ch in c["text"])}
        else:
            worst = max((max(abs(a - b) for a, b in zip(c["r"], d["r"])) for c, d in pairs), default=0)
        faded = [(c, d) for c, d in pairs if "alpha" in c and "alpha" in d and abs(c["alpha"] - d["alpha"]) > TOL_ALPHA]
        if faded:
            extra += f", opacity disagrees {len(faded)}"
        flag = " <-- DIFFERS" if worst > TOL or oc or od or faded else ""
        bad += worst > TOL or bool(oc) or bool(od) or bool(faded)
        print(f"  {k:5s} {len(pairs):4d} paired, worst {worst:.4f} units{extra}{flag}")
        for c in oc[:6]:
            print(f"        only the DLL:    {keys[k](c)!r} at {[round(x, 2) for x in c['r']]}")
        for d in od[:6]:
            print(f"        only the mockup: {keys[k](d)!r} at {[round(x, 2) for x in d['r']]}")
        for c, d in sorted(pairs, key=lambda p: -max(abs(a - b) for a, b in zip(p[0].get('cmp', p[0]['r']), p[1].get('cmp', p[1]['r']))))[:3]:
            if max(abs(a - b) for a, b in zip(c.get("cmp", c["r"]), d.get("cmp", d["r"]))) > TOL:
                print(f"        {keys[k](c)!r}: DLL {[round(x, 3) for x in c.get('cmp', c['r'])]} mockup {[round(x, 3) for x in d.get('cmp', d['r'])]}")
    # the window's box, which the game thread reads a click outside of
    w = max(abs(a - b) for a, b in zip(ops["frame"]["window"], dom["window"]))
    flag = " <-- DIFFERS" if w > TOL else ""
    bad += w > TOL
    # each panel's scroll as drawn and its limit
    sc = []
    for i, pn in enumerate(SCROLLS):
        d = dom["scroll"][pn]
        sc.append(max(abs(ops["frame"]["scroll"][i] - d["top"]), abs(ops["frame"]["max"][i] - d["max"])))
    flag2 = " <-- DIFFERS" if max(sc) > TOL else ""
    bad += max(sc) > TOL
    print(f"  window within {w:.4f} units{flag}; scroll limits and offsets within {max(sc):.4f} units{flag2}")
    # the thumbs, which Chrome paints on whole pixels
    worst, miss = 0, []
    for pn in SCROLLS:
        d = dom["thumbs"].get(pn)
        cands = [c for c in cs["thumb"] if abs(c["x"] - ((dom["scroll"][pn]["page"][0] - dom["frame"][0]) / u)) < TOL]
        if (d is None) != (not cands):
            miss.append(pn)
        elif d:
            worst = max(worst, abs(cands[0]["y"] - d[0]), abs(cands[0]["h"] - d[1]))
    flag = " <-- DIFFERS" if miss or worst * u > TOL_THUMB_PX else ""
    bad += bool(miss) or worst * u > TOL_THUMB_PX
    print(f"  thumb {len(dom['thumbs']):4d} painted, worst {worst * u:.2f} px{', only one side: ' + ', '.join(miss) if miss else ''}{flag}")
    return bad, glyphs


def shot(run, tmp, dom, out):
    """The case's frame cut out of a screenshot of the page, to `out`."""
    from PIL import Image
    png = tmp / "frame.png"
    run("screenshot", f"--filename={png}")
    (fx, fy), s = dom["frame"], dom["state"]
    x, y = round(fx), round(fy)
    Image.open(png).convert("RGB").crop((x, y, x + round(s["W"]), y + round(s["H"]))).save(out)


def main():
    args = sys.argv[1:]
    only = args == ["--check-only"]
    keep = Path(args[args.index("--keep") + 1]) if "--keep" in args else None
    shots = Path(args[args.index("--shots") + 1]).resolve() if "--shots" in args else None
    t0 = time.time()
    tmp = Path(tempfile.mkdtemp(prefix="modslaycheck"))
    exe = tmp / "modslaycheck"
    cc = subprocess.run(["gcc", "-O1", "-std=c99", "-Wall", f"-I{SRC}", f"-I{SRC.parent / 'inc'}", str(ROOT / "tools/mods/modslaycheck.c"),
                         *(str(SRC / f) for f in ("tagpu_modslay.c", "tagpu_uikit.c", "tagpu_hudlay.c", "tagpu_hudtext.c", "tagpu_json.c", "lodepng.c")),
                         "-lm", "-o", str(exe)], capture_output=True, text=True)
    if cc.returncode:
        sys.exit("modslaycheck: the native build failed\n" + cc.stderr)
    arts = [str(ROOT / "art/hud"), str(ROOT / "art/wiki"), str(ROOT / "art/mods")]
    ck = subprocess.run([str(exe), "--check", *arts], capture_output=True, text=True)
    print((ck.stdout or ck.stderr).strip())
    if ck.returncode or only:
        shutil.rmtree(tmp, ignore_errors=True)
        return ck.returncode
    faces = wl.face_metrics()
    roles = json.loads((ROOT / "art/mods/layout.json").read_text())["roles"]
    fallback = lacking()
    httpd = wl.serve()
    cfg = tmp / "pw.json"
    cfg.write_text(json.dumps({"browser": {"contextOptions": {"deviceScaleFactor": 1, "viewport": {"width": 1200, "height": 900}},
                                           "launchOptions": {"ignoreDefaultArgs": ["--hide-scrollbars"]}}}))
    pw = ["playwright-cli", f"-s=modslaycheck{os.getpid()}", f"--config={cfg}"]
    run = lambda *a: subprocess.run(pw + list(a), capture_output=True, text=True, cwd=tmp)
    bad, glyphs = 0, set()
    run("open", "about:blank")
    try:
        for state, (gw, gh), acts in CASES:
            name = f"{gw}x{gh}-{state}" + "".join(f"-{k}-{v}" for k, v in acts.get("scroll", {}).items()) + (f"-held-{acts['held']}" if "held" in acts else "")
            run("resize", str(gw + 34), str(gh + 900))
            q = f"res={gw}x{gh}&state={state}" + (f"&held={acts['held']}" if "held" in acts else "")
            run("goto", f"http://127.0.0.1:{httpd.server_address[1]}/tools/mods/mockup/index.html?{q}")
            # waited for with no deadline: each condition waits on a load that resolves or fails, so the page always settles and a
            # page still loading is never measured; the slice decides nothing
            while (ok := wl.result(run("eval", "async () => { const ok = () => !!(window.ModsPage && document.querySelector('#win canvas.skin')"
                                       " && getComputedStyle(document.querySelector('#win .wt')).fontFamily.includes('Oswald') && document.fonts.status === 'loaded');"
                                       " const t0 = performance.now(); while (!ok() && performance.now() - t0 < 10000)"
                                       " await new Promise(r => setTimeout(r, 50)); return ok(); }").stdout)) is False:
                pass
            if ok is not True:
                sys.exit(f"modslaycheck: the mockup page for {name} could not be read: {ok}")
            code = COLLECT.replace("ACTIONS", json.dumps(acts)).replace("FALLBACK", json.dumps(fallback)).replace("ROLEMAP", json.dumps(roles))
            r = run("eval", code)
            v = wl.result(r.stdout)
            if not v:
                sys.exit(f"modslaycheck: the mockup gave nothing for {name}\n{r.stdout[-2000:]}\n{r.stderr}")
            dom = json.loads(v)
            if dom["state"]["res"] != f"{gw}x{gh}":
                sys.exit(f"modslaycheck: {name}: the mockup shows {dom['state']['res']}, not the case's {gw}x{gh}: it lists no such screen")
            if (dom["state"]["W"], dom["state"]["H"]) != (gw, gh):
                sys.exit(f"modslaycheck: {name}: the frame is {dom['state']['W']} x {dom['state']['H']}, not the screen's {gw} x {gh}")
            if shots:
                shots.mkdir(parents=True, exist_ok=True)
                shot(run, tmp, dom, shots / f"{name.replace(':', '_')}.png")
            dom["thumbs"] = wl.thumbs(run, tmp, dom)
            sp = tmp / "state.json"
            sp.write_text(json.dumps(dom["state"]))
            r = subprocess.run([str(exe), *arts, str(sp)], capture_output=True, text=True)
            if r.returncode:
                sys.exit(f"modslaycheck: {name}: {r.stderr}")
            ops = json.loads(r.stdout)
            if keep:
                keep.mkdir(parents=True, exist_ok=True)
                (keep / f"{name}.dom.json").write_text(json.dumps(dom, indent=1))
                (keep / f"{name}.ops.json").write_text(r.stdout)
            b, g = compare(name, dom, ops, faces, fallback)
            bad += b
            glyphs |= g
    finally:
        run("close")
        httpd.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    if glyphs:
        bad += 1
        print(f"the mockup sets {sorted(glyphs)} in the browser's fallback face, which the DLL does not carry: the game draws no glyph for them <-- DIFFERS")
    print(f"modslaycheck: {len(CASES)} cases in {time.time() - t0:.0f} s: "
          + ("the DLL and the mockup agree, every box, line and baseline" if not bad else f"{bad} disagreement(s)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
