#!/usr/bin/env python3
"""Build the TotalA.exe modding wiki from research/notes/*.md into research/site/."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import date
from pathlib import Path

try:
    import markdown
except ModuleNotFoundError:                                 # pragma: no cover
    # The system python is PEP 668 externally-managed and has none of this. The
    # project's environment is .venv-undither at the MAIN CHECKOUT — one venv,
    # shared by every worktree (find_python() below resolves it the same way for
    # the bake subprocess). Say so here rather than dying on a bare ImportError,
    # because from a worktree the obvious `python3 research/build_wiki.py` is
    # exactly the command that fails.
    _here = Path(__file__).resolve().parent
    _cands = [_here.parent / ".venv-undither" / "bin" / "python"]
    try:
        _c = subprocess.run(["git", "rev-parse", "--git-common-dir"], capture_output=True,
                            text=True, check=True, cwd=_here).stdout.strip()
        _main = (Path(_c) if Path(_c).is_absolute() else _here.parent / _c).resolve().parent
        _cands.append(_main / ".venv-undither" / "bin" / "python")
    except Exception:
        pass
    _py = next((c for c in _cands if c.exists()), None)
    sys.exit(
        "build_wiki: no 'markdown' module in {}.\n"
        "Run it with the project venv instead:\n    {} {}\n"
        "{}".format(
            sys.executable,
            _py or "<main checkout>/.venv-undither/bin/python", __file__,
            "" if _py else "(no .venv-undither found — create one and "
                           "`pip install markdown`; see CLAUDE.md)"))

ROOT = Path(__file__).resolve().parent
NOTES = ROOT / "notes"
SITE = ROOT / "site"
REPO = ROOT.parent
ASSETS = SITE / "assets"

SITE_TITLE = "TotalA.exe Modding Wiki"
SUBTITLE = "How the Total Annihilation community adds engine features to a closed-source 1997 binary"

# slug -> (nav label, section). Order here is the nav order.
PAGES = [
    ("project-map",                "Project map",                 "Overview"),
    ("roadmap",                    "GPU renderer roadmap",        "Overview"),
    ("gpu-status",                 "GPU status, hooks & limits",  "Overview"),
    ("field-notes",                "Field notes & gotchas",       "Overview"),

    ("frame-composition",          "Frame composition (G3)",      "Renderer"),
    ("unit-3do-bridge",            "Unit → 3DO bridge",           "Renderer"),
    ("composite-buffer",           "Composite buffer (G6)",       "Renderer"),
    ("own-the-draw",               "Own the draw (G7)",           "Renderer"),
    ("gpu-render3do",              "GPU 3DO geometry (phase B)",  "Renderer"),
    ("build-state",                "Build state (nanoframe)",     "Renderer"),
    ("shadows-cloak",              "Shadows & cloaking",          "Renderer"),
    ("terrain-depth",              "Terrain, features & depth",   "Renderer"),
    ("effects",                    "Effects (fire, explosions, debris)", "Renderer"),
    ("features",                   "Features (trees, rocks, wreckage)",  "Renderer"),
    ("ui-markers",                 "UI markers",                  "Renderer"),
    ("native-res-design",          "Native-resolution pass (G12)","Renderer"),
    ("renderers",                  "Classic and Classic++ renderers", "Renderer"),

    ("tacli-design",               "tacli — launcher & driver",   "Tooling"),
    ("model-export",               "Model export (3DO → glTF)",   "Tooling"),
    ("model-import",               "Model import (glTF → engine)","Tooling"),
    ("input-firewall",             "The input firewall",          "Tooling"),
    ("gui-gadgets",                "GUI gadgets",                 "Tooling"),
    ("scenario-format",            "JSON scenarios",              "Tooling"),
    ("tascene-design",             "tascene — browser render lab", "Tooling"),
    ("windowed-mode",              "Windowed mode",               "Tooling"),

    ("binary-patches",            "The core mechanism",        "Mechanism"),
    ("exe-reverse-engineering",   "Reverse-engineering the exe","Mechanism"),
    ("runtime-injection",         "Injection & hooking",       "Mechanism"),
    ("api-wrappers",              "The DirectDraw boundary",   "Mechanism"),
    ("memory-manager-investigation","Memory Manager Investigation","Mechanism"),
    ("deep-plugin-abi-and-corpus","Cavedog's plugin ABI",      "Mechanism"),
    ("extra-weapons",             "More weapons per unit",     "Mechanism"),
    ("line-of-sight",             "Line of sight & fog",       "Mechanism"),
    ("factory-build",             "Factories: build & carry",  "Mechanism"),

    ("deep-tadr",                 "TADR / tdraw.dll",          "Projects"),
    ("release-matrix",            "Releases & feature matrix", "Projects"),
    ("deep-patch-3902",           "Unofficial Patch 3.9.02",   "Projects"),
    ("deep-patch-loader",         "TA Patch Loader",           "Projects"),
    ("deep-ta-zero",              "TA Zero",                   "Projects"),
    ("deep-total-mayhem",         "Total Mayhem",              "Projects"),
    ("deep-prota",                "ProTA",                     "Projects"),
    ("deep-ta-esc",               "TA: Escalation",            "Projects"),
    ("networking-lobbies",        "TA Forever & netcode",      "Projects"),

    ("file-formats",              "File formats (3DO, COB, GAF)","Reference"),
    ("undither",                  "Undithering screenshots",   "Reference"),
    ("cmdline-options",           "Launch knobs (cmdline & INI)","Reference"),
    ("resolution",                "Resolution pipeline",         "Reference"),

    ("candidates-community",      "Community sweep",           "Survey"),
    ("candidates-features",       "Feature-first sweep",       "Survey"),
    ("candidates-code",           "Code-host sweep",           "Survey"),
    ("patching-playbooks",        "Playbooks from other games","Survey"),
]

SECTION_ORDER = ["Overview", "Renderer", "Tooling", "Mechanism", "Projects",
                 "Reference", "Survey"]
SECTION_BLURB = {
    "Overview": "Where the project stands, and the gotchas that cost time",
    "Renderer": "The GPU renderer: TA's draw paths mapped, and our passes",
    "Tooling": "Driving the game from a CLI — instances, input, UI, scenarios",
    "Mechanism": "How the patching actually works",
    "Projects": "The mods and patches, one page each",
    "Survey": "Discovery passes and comparisons",
    "Reference": "Asset formats and the stock engine's external surfaces",
}

CSS = r"""
/* TotalA.exe wiki — gunmetal & amber, after TA's own HUD readouts */
:root{
  --ground:#e9ecee; --panel:#f6f8f9; --panel-2:#dfe4e7; --edge:#c3ccd1;
  --ink:#161c20; --ink-2:#47555d; --ink-3:#6b7a83;
  --accent:#9a5b00; --accent-ink:#7a4800; --link:#116b73; --link-hover:#0a4f55;
  --ok:#2f6f42; --ok-bg:#dff0e4; --warn:#9c3a1e; --warn-bg:#fbe4dc;
  --lead:#6b5a1f; --lead-bg:#f4ecd2;
  --code-bg:#eceff1; --code-ink:#22303a;
  --shadow:0 1px 2px rgba(20,30,36,.08),0 4px 14px rgba(20,30,36,.05);
}
@media (prefers-color-scheme:dark){
  :root:not([data-theme="light"]){
    --ground:#11161a; --panel:#171e23; --panel-2:#1e262c; --edge:#2c373f;
    --ink:#dbe3e8; --ink-2:#9aa9b2; --ink-3:#71828d;
    --accent:#f0a828; --accent-ink:#ffc257; --link:#5ec6c0; --link-hover:#8fdad5;
    --ok:#7fc98a; --ok-bg:#17301f; --warn:#e8815f; --warn-bg:#331c14;
    --lead:#d8c07a; --lead-bg:#2e2716;
    --code-bg:#141b20; --code-ink:#c3d0d8;
    --shadow:0 1px 2px rgba(0,0,0,.4),0 6px 20px rgba(0,0,0,.28);
  }
}
:root[data-theme="dark"]{
  --ground:#11161a; --panel:#171e23; --panel-2:#1e262c; --edge:#2c373f;
  --ink:#dbe3e8; --ink-2:#9aa9b2; --ink-3:#71828d;
  --accent:#f0a828; --accent-ink:#ffc257; --link:#5ec6c0; --link-hover:#8fdad5;
  --ok:#7fc98a; --ok-bg:#17301f; --warn:#e8815f; --warn-bg:#331c14;
  --lead:#d8c07a; --lead-bg:#2e2716;
  --code-bg:#141b20; --code-ink:#c3d0d8;
  --shadow:0 1px 2px rgba(0,0,0,.4),0 6px 20px rgba(0,0,0,.28);
}

*{box-sizing:border-box}
html{scroll-behavior:smooth}
@media (prefers-reduced-motion:reduce){html{scroll-behavior:auto} *{animation:none!important;transition:none!important}}
body{
  margin:0; background:var(--ground); color:var(--ink);
  font-family:"IBM Plex Sans",system-ui,-apple-system,"Segoe UI",sans-serif;
  font-size:16px; line-height:1.62; -webkit-font-smoothing:antialiased;
}
code,kbd,pre,.mono{font-family:"IBM Plex Mono",ui-monospace,"SF Mono",Menlo,Consolas,monospace}

/* ---------- shell ---------- */
.shell{display:grid; grid-template-columns:288px minmax(0,1fr); min-height:100vh}
@media (max-width:1000px){ .shell{grid-template-columns:1fr} }

/* ---------- sidebar ---------- */
.side{
  background:var(--panel); border-right:1px solid var(--edge);
  padding:22px 18px 40px; position:sticky; top:0; height:100vh; overflow-y:auto;
  display:flex; flex-direction:column; gap:18px;
}
@media (max-width:1000px){ .side{position:static; height:auto; border-right:0; border-bottom:1px solid var(--edge)} }
.brand{display:flex; flex-direction:column; gap:2px; text-decoration:none}
.brand .mark{
  font-family:"Saira Condensed","Arial Narrow",sans-serif; font-weight:700;
  font-size:1.34rem; letter-spacing:.02em; color:var(--ink); line-height:1.1;
}
.brand .mark em{font-style:normal; color:var(--accent)}
.brand .sub{font-size:.72rem; color:var(--ink-3); letter-spacing:.04em; text-transform:uppercase}
.search{
  width:100%; padding:8px 11px; border-radius:7px; border:1px solid var(--edge);
  background:var(--ground); color:var(--ink); font:inherit; font-size:.87rem;
}
.search:focus{outline:2px solid var(--accent); outline-offset:1px}
.navgroup{margin-bottom:4px}
.navgroup h4{
  font-family:"Saira Condensed","Arial Narrow",sans-serif; font-size:.76rem; font-weight:600;
  letter-spacing:.13em; text-transform:uppercase; color:var(--ink-3);
  margin:14px 0 7px; padding-bottom:5px; border-bottom:1px solid var(--edge);
}
.nav a{
  display:block; padding:5px 9px; margin:1px 0; border-radius:6px; text-decoration:none;
  color:var(--ink-2); font-size:.885rem; border-left:2px solid transparent;
}
.nav a:hover{background:var(--panel-2); color:var(--ink)}
.nav a.current{background:var(--panel-2); color:var(--ink); border-left-color:var(--accent); font-weight:600}
.nav a:focus-visible{outline:2px solid var(--accent); outline-offset:1px}
.side-foot{margin-top:auto; padding-top:14px; border-top:1px solid var(--edge); font-size:.75rem; color:var(--ink-3)}
.themebtn{
  background:var(--ground); border:1px solid var(--edge); color:var(--ink-2); cursor:pointer;
  border-radius:6px; padding:5px 10px; font:inherit; font-size:.78rem; margin-top:8px;
}
.themebtn:hover{color:var(--ink); border-color:var(--accent)}

/* ---------- main ---------- */
.main{min-width:0; display:grid; grid-template-columns:minmax(0,1fr) 216px; gap:34px; padding:34px 40px 90px}
@media (max-width:1240px){ .main{grid-template-columns:minmax(0,1fr)} .toc{display:none} }
@media (max-width:640px){ .main{padding:22px 18px 60px} }
.doc{min-width:0}
.doc>p,.doc>ul,.doc>ol,.doc>blockquote{max-width:74ch}
.tablewrap svg{display:block; width:100%; height:auto}

.toc{position:sticky; top:34px; align-self:start; max-height:calc(100vh - 68px); overflow-y:auto; font-size:.82rem}
.toc h5{
  font-family:"Saira Condensed",sans-serif; text-transform:uppercase; letter-spacing:.12em;
  font-size:.72rem; color:var(--ink-3); margin:0 0 8px;
}
.toc a{display:block; padding:3px 0 3px 10px; color:var(--ink-3); text-decoration:none; border-left:2px solid var(--edge)}
.toc a:hover{color:var(--link); border-left-color:var(--accent)}
.toc a.lvl3{padding-left:22px; font-size:.95em}

/* ---------- typography ---------- */
.doc h1{
  font-family:"Saira Condensed","Arial Narrow",sans-serif; font-weight:700; font-size:2.45rem;
  line-height:1.08; letter-spacing:.005em; margin:0 0 10px; text-wrap:balance; color:var(--ink);
}
.doc h2{
  font-family:"Saira Condensed",sans-serif; font-weight:600; font-size:1.52rem; letter-spacing:.01em;
  margin:2.4em 0 .55em; padding-bottom:.24em; border-bottom:1px solid var(--edge); text-wrap:balance;
}
.doc h3{font-family:"Saira Condensed",sans-serif; font-weight:600; font-size:1.17rem; margin:1.9em 0 .4em; color:var(--ink); text-wrap:balance}
.doc h4{font-size:.97rem; font-weight:600; margin:1.5em 0 .3em; color:var(--ink-2)}
.doc p{margin:0 0 1.02em}
.doc a{color:var(--link); text-decoration:none; border-bottom:1px solid color-mix(in srgb,var(--link) 35%,transparent)}
.doc a:hover{color:var(--link-hover); border-bottom-color:currentColor}
.doc a:focus-visible{outline:2px solid var(--accent); outline-offset:2px}
.doc ul,.doc ol{margin:0 0 1.02em; padding-left:1.35em}
.doc li{margin:.24em 0}
.doc li>ul,.doc li>ol{margin:.24em 0}
.doc strong{color:var(--ink); font-weight:600}
.doc hr{border:0; border-top:1px solid var(--edge); margin:2.2em 0}
.doc blockquote{
  margin:1.2em 0; padding:.6em 1.05em; border-left:3px solid var(--accent);
  background:var(--panel); color:var(--ink-2); border-radius:0 7px 7px 0;
}
.doc blockquote p:last-child{margin-bottom:0}

.doc :not(pre)>code{
  background:var(--code-bg); color:var(--code-ink); padding:.1em .36em; border-radius:4px;
  font-size:.875em; border:1px solid var(--edge); font-variant-numeric:tabular-nums;
  overflow-wrap:break-word;
}
.doc pre{
  background:var(--code-bg); border:1px solid var(--edge); border-radius:9px;
  padding:13px 15px; overflow-x:auto; margin:0 0 1.15em; font-size:.83rem; line-height:1.55;
}
.doc pre code{background:none; border:0; padding:0; color:var(--code-ink)}

/* ---------- tables ---------- */
.tablewrap{overflow-x:auto; margin:0 0 1.3em; border:1px solid var(--edge); border-radius:9px; background:var(--panel); box-shadow:var(--shadow)}
.doc table{border-collapse:collapse; width:100%; font-size:.855rem; font-variant-numeric:tabular-nums}
.doc th{
  text-align:left; padding:9px 12px; background:var(--panel-2); color:var(--ink);
  font-family:"Saira Condensed",sans-serif; font-size:.85rem; font-weight:600;
  letter-spacing:.05em; text-transform:uppercase; border-bottom:1px solid var(--edge); white-space:nowrap;
}
.doc td{padding:8px 12px; border-bottom:1px solid var(--edge); vertical-align:top; color:var(--ink-2)}
.doc tbody tr:last-child td{border-bottom:0}
.doc tbody tr:hover td{background:var(--panel-2)}
.doc td code{font-size:.85em}

/* ---------- evidence pills ---------- */
.pill{
  display:inline-block; font-family:"IBM Plex Mono",monospace; font-size:.685rem; font-weight:600;
  letter-spacing:.07em; padding:.1em .48em; border-radius:4px; vertical-align:.08em; white-space:nowrap;
}
.pill-ok{background:var(--ok-bg); color:var(--ok); border:1px solid color-mix(in srgb,var(--ok) 34%,transparent)}
.pill-warn{background:var(--warn-bg); color:var(--warn); border:1px solid color-mix(in srgb,var(--warn) 34%,transparent)}
.pill-lead{background:var(--lead-bg); color:var(--lead); border:1px solid color-mix(in srgb,var(--lead) 34%,transparent)}

/* ---------- page head ---------- */
.crumb{font-size:.75rem; letter-spacing:.1em; text-transform:uppercase; color:var(--ink-3); margin-bottom:12px}
.crumb a{color:var(--ink-3); text-decoration:none}
.crumb a:hover{color:var(--link)}
.lede{font-size:1.06rem; color:var(--ink-2); margin:0 0 26px; padding-bottom:20px; border-bottom:1px solid var(--edge)}

/* ---------- index ---------- */
.hero{margin-bottom:34px}
.hero h1{font-size:3.1rem; margin-bottom:6px}
@media (max-width:640px){ .hero h1{font-size:2.25rem} }
.hero .tag{font-size:1.08rem; color:var(--ink-2); max-width:62ch}
.statrow{display:grid; grid-template-columns:repeat(auto-fit,minmax(132px,1fr)); gap:12px; margin:26px 0 8px}
.stat{background:var(--panel); border:1px solid var(--edge); border-radius:9px; padding:13px 15px; box-shadow:var(--shadow)}
.stat .n{font-family:"Saira Condensed",sans-serif; font-size:1.85rem; font-weight:700; color:var(--accent); line-height:1; font-variant-numeric:tabular-nums}
.stat .l{font-size:.75rem; color:var(--ink-3); text-transform:uppercase; letter-spacing:.07em; margin-top:5px}
.cards{display:grid; grid-template-columns:repeat(auto-fill,minmax(258px,1fr)); gap:13px; margin:14px 0 6px}
.card{
  display:block; background:var(--panel); border:1px solid var(--edge); border-radius:10px;
  padding:15px 17px; text-decoration:none; color:inherit; box-shadow:var(--shadow);
}
.card:hover{border-color:var(--accent); transform:translateY(-1px)}
.card:focus-visible{outline:2px solid var(--accent); outline-offset:2px}
.card h3{font-family:"Saira Condensed",sans-serif; margin:0 0 5px; font-size:1.1rem; color:var(--ink)}
.card p{margin:0; font-size:.85rem; color:var(--ink-3); line-height:1.5}

/* search results */
.hit{display:block; padding:9px 11px; border-radius:7px; text-decoration:none; color:inherit; border:1px solid var(--edge); background:var(--panel); margin-bottom:7px}
.hit:hover{border-color:var(--accent)}
.hit b{display:block; font-family:"Saira Condensed",sans-serif; font-size:1rem; color:var(--ink)}
.hit span{font-size:.8rem; color:var(--ink-3)}
#searchout:not(:empty){margin-top:10px}

/* ============ mobile / iOS Safari ============ */
html,body{min-height:100%}
html{background:var(--ground); -webkit-text-size-adjust:100%}

/* long hex strings, URLs and identifiers must never widen the page */
.doc :not(pre)>code{overflow-wrap:anywhere; word-break:break-word}
.doc p a{overflow-wrap:anywhere}
.tablewrap,.doc pre{-webkit-overflow-scrolling:touch}

/* iOS zooms any input whose font-size is under 16px on focus */
.search{font-size:16px}

.topbar{display:none}

@media (max-width:1000px){
  .shell{display:block}

  .topbar{
    display:flex; align-items:center; gap:12px; position:sticky; top:0; z-index:40;
    background:var(--panel); border-bottom:1px solid var(--edge);
    padding:10px max(14px,env(safe-area-inset-right)) 10px max(14px,env(safe-area-inset-left));
  }
  .topbar .brand .mark{font-size:1.1rem}
  .topbar .brand .sub{font-size:.62rem}
  .menubtn{
    margin-left:auto; display:inline-flex; align-items:center; gap:7px; min-height:44px; padding:0 14px;
    background:var(--ground); border:1px solid var(--edge); border-radius:8px;
    color:var(--ink); font:inherit; font-size:.86rem; cursor:pointer;
  }
  .menubtn:focus-visible{outline:2px solid var(--accent); outline-offset:2px}
  .menubtn .bars{display:inline-block; width:15px; height:2px; background:currentColor; position:relative}
  .menubtn .bars::before,.menubtn .bars::after{content:""; position:absolute; left:0; width:15px; height:2px; background:currentColor}
  .menubtn .bars::before{top:-5px} .menubtn .bars::after{top:5px}

  /* Without JS the sidebar simply stays in flow and visible. */
  .side{
    position:static; height:auto; border-right:0; border-bottom:1px solid var(--edge);
    padding-left:max(18px,env(safe-area-inset-left)); padding-right:max(18px,env(safe-area-inset-right));
  }
  html.js .side{
    position:fixed; inset:0 auto 0 0; width:min(84vw,320px); z-index:60;
    transform:translateX(-100%); transition:transform .22s ease;
    height:100dvh; overflow-y:auto; border-right:1px solid var(--edge); border-bottom:0;
    padding-top:max(18px,env(safe-area-inset-top)); padding-bottom:max(28px,env(safe-area-inset-bottom));
    box-shadow:0 0 0 100vmax rgba(0,0,0,0);
  }
  html.js .side.open{transform:none; box-shadow:12px 0 34px rgba(0,0,0,.34)}
  html.js .side .brand{display:none}
  .scrim{
    display:none; position:fixed; inset:0; z-index:50; background:rgba(6,10,12,.55);
    -webkit-backdrop-filter:blur(2px); backdrop-filter:blur(2px);
  }
  html.js .scrim.show{display:block}

  .main{
    display:block;
    padding:22px max(18px,env(safe-area-inset-right)) max(64px,env(safe-area-inset-bottom)) max(18px,env(safe-area-inset-left));
  }
  .doc{max-width:none}
  .doc h1{font-size:1.92rem}
  .doc h2{font-size:1.3rem; margin-top:1.9em}
  .doc h3{font-size:1.08rem}
  .doc table{font-size:.8rem}
  .doc th,.doc td{padding:7px 10px}
  .nav a{padding:9px 10px; font-size:.92rem}   /* comfortable tap targets */
  .hero h1{font-size:2.1rem}
  .statrow{grid-template-columns:repeat(2,1fr)}
  .cards{grid-template-columns:1fr}
}

@media (max-width:1000px) and (prefers-reduced-motion:reduce){
  html.js .side{transition:none}
}
"""

JS = r"""
(function(){
  var root=document.documentElement;
  root.classList.add('js');
  try{var s=localStorage.getItem('tawiki-theme'); if(s) root.setAttribute('data-theme',s);}catch(e){}

  /* ---- mobile drawer ---- */
  var side=document.querySelector('.side'), scrim=document.querySelector('.scrim'),
      menu=document.getElementById('menubtn');
  function setDrawer(open){
    if(!side) return;
    side.classList.toggle('open',open);
    if(scrim) scrim.classList.toggle('show',open);
    if(menu) menu.setAttribute('aria-expanded',open?'true':'false');
    document.body.style.overflow = open ? 'hidden' : '';
  }
  if(menu) menu.addEventListener('click',function(){ setDrawer(!side.classList.contains('open')); });
  if(scrim) scrim.addEventListener('click',function(){ setDrawer(false); });
  document.addEventListener('keydown',function(e){ if(e.key==='Escape') setDrawer(false); });
  if(side) side.addEventListener('click',function(e){ if(e.target.closest('a')) setDrawer(false); });
  var btn=document.getElementById('themebtn');
  if(btn) btn.addEventListener('click',function(){
    var cur=root.getAttribute('data-theme');
    if(!cur) cur = matchMedia('(prefers-color-scheme: dark)').matches ? 'dark':'light';
    var next = cur==='dark' ? 'light':'dark';
    root.setAttribute('data-theme',next);
    try{localStorage.setItem('tawiki-theme',next);}catch(e){}
  });

  var box=document.getElementById('search'), out=document.getElementById('searchout'), nav=document.getElementById('nav');
  if(!box) return;
  var idx=null;
  fetch(BASE+'assets/search.json').then(function(r){return r.json()}).then(function(d){idx=d}).catch(function(){});
  box.addEventListener('input',function(){
    var q=box.value.trim().toLowerCase();
    if(!q){ out.innerHTML=''; nav.style.display=''; return; }
    nav.style.display='none';
    if(!idx){ out.innerHTML='<p style="font-size:.8rem;color:var(--ink-3)">indexing…</p>'; return; }
    var hits=idx.map(function(p){
      var hay=(p.t+' '+p.h+' '+p.b).toLowerCase(), sc=0, i=hay.indexOf(q);
      if(p.t.toLowerCase().indexOf(q)>=0) sc+=10;
      if(i>=0) sc+=3;
      return sc? {p:p,s:sc,i:i} : null;
    }).filter(Boolean).sort(function(a,b){return b.s-a.s}).slice(0,12);
    out.innerHTML = hits.length ? hits.map(function(h){
      var ctx=''; if(h.i>=0){ var b=h.p.b; var st=Math.max(0,h.i-45); ctx=(st?'…':'')+b.substr(st,120)+'…'; }
      return '<a class="hit" href="'+BASE+h.p.u+'"><b>'+h.p.t+'</b><span>'+ctx.replace(/[<>]/g,'')+'</span></a>';
    }).join('') : '<p style="font-size:.82rem;color:var(--ink-3)">No match.</p>';
  });
})();
"""


def pillify(html: str) -> str:
    """Render [VERIFIED] / [CLAIMED] / [LEAD] evidence tags as styled pills."""
    def sub(m):
        word = m.group(1).upper()
        cls = {"VERIFIED": "pill-ok", "CLAIMED": "pill-warn", "LEAD": "pill-lead",
               "UNVERIFIED": "pill-warn", "REFUTED": "pill-warn"}.get(word, "pill-lead")
        return f'<span class="pill {cls}">{word}</span>'
    # Only outside of code/pre — markdown has already escaped brackets inside code spans.
    parts = re.split(r"(<code[^>]*>.*?</code>|<pre.*?</pre>)", html, flags=re.S)
    for i in range(0, len(parts), 2):
        parts[i] = re.sub(r"\[(VERIFIED|CLAIMED|LEAD|UNVERIFIED|REFUTED)\]", sub, parts[i], flags=re.I)
    return "".join(parts)


def wrap_tables(html: str) -> str:
    return re.sub(r"<table>", '<div class="tablewrap"><table>', html).replace("</table>", "</table></div>")


def slugify(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def build_page(md_text: str):
    md = markdown.Markdown(extensions=["tables", "fenced_code", "sane_lists", "attr_list", "toc"],
                           extension_configs={"toc": {"anchorlink": False, "permalink": False}})
    html = md.convert(md_text)
    # cross-links between notes are authored as `page.md` (correct in the repo);
    # the site serves `page.html`
    html = re.sub(r'href="([a-z0-9_-]+)\.md(#[^"]*)?"', lambda m: f'href="{m.group(1)}.html{m.group(2) or ""}"', html)
    html = wrap_tables(pillify(html))
    toc = getattr(md, "toc_tokens", [])
    return html, toc


def flatten_toc(tokens, depth=2, out=None):
    if out is None:
        out = []
    for t in tokens:
        out.append((depth, t["id"], t["name"]))
        if depth < 3:
            flatten_toc(t.get("children", []), depth + 1, out)
    return out


def strip_html(h: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", h)).strip()


def blurb_from(md_text: str, fallback: str, limit: int = 165) -> str:
    """Pull a human blurb from the note's Summary section, else the first real paragraph."""
    body = md_text
    m = re.search(r"^##\s+Summary[^\n]*\n(.+?)(?=^##\s)", md_text, flags=re.M | re.S)
    if m:
        body = m.group(1)
    else:
        body = re.sub(r"^#[^\n]*\n", "", md_text, count=1)

    for para in re.split(r"\n\s*\n", body):
        p = para.strip()
        # skip headings, metadata lines, tables, lists, code and blockquotes
        if not p or p.startswith(("#", "|", ">", "```", "-", "*", "1.")):
            continue
        if re.match(r"^\*\*[A-Za-z][^*]{0,24}:\*\*", p):
            continue
        p = re.sub(r"\[(VERIFIED|CLAIMED|LEAD|UNVERIFIED|REFUTED)\]", "", p, flags=re.I)
        p = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", p)          # links -> text
        p = re.sub(r"[*_`#]+", "", p)                            # emphasis / code marks
        p = re.sub(r"\s+", " ", p).strip()
        if len(p) < 40:
            continue
        if len(p) <= limit:
            return p
        cut = p[:limit]
        sp = cut.rfind(" ")
        return (cut[:sp] if sp > 60 else cut).rstrip(" ,;:.") + "…"
    return fallback[:limit] + "…"


ASSET_VER = hashlib.md5((CSS + JS).encode()).hexdigest()[:10]

STATIC = NOTES / "assets"          # copied verbatim to site/assets/, pages link into it


# --------------------------------------------------------------------------- learned bakes
# The undither page's "Learned CNN" preset shows frames restored offline by the
# shipped model (unditherer/models/full.*).  Those 15 lossless WebPs are 21 MB and
# stay out of git; they are generated here on the first build and whenever the
# model or a frame is newer than its bake.

BAKE_SHOTS = NOTES / "assets" / "undither" / "shots"
BAKE_OUT = NOTES / "assets" / "undither" / "learned"
BAKE_MODEL = [REPO / "unditherer" / "models" / "full.pt", REPO / "unditherer" / "models" / "full.onnx"]


def bake_frames(shots=BAKE_SHOTS):
    return sorted(p for p in Path(shots).glob("*.png") if "mapfield" not in p.name)


def stale_bakes(frames, out_dir, model_paths):
    """Frames whose bake is missing or older than the model or the frame itself."""
    out_dir = Path(out_dir)
    model_mtime = max((Path(m).stat().st_mtime for m in model_paths if Path(m).exists()), default=0.0)
    stale = []
    for f in frames:
        bake = out_dir / (Path(f).stem + ".webp")
        if not bake.exists() or bake.stat().st_mtime < max(model_mtime, Path(f).stat().st_mtime):
            stale.append(Path(f))
    if frames and not (out_dir / "model.json").exists():
        return list(map(Path, frames))
    return stale


def find_python():
    """The unditherer environment if there is one (repo root, or the main checkout a
    worktree belongs to), else whatever runs this script."""
    cands = [REPO / ".venv-undither" / "bin" / "python"]
    try:
        common = subprocess.run(["git", "rev-parse", "--git-common-dir"], capture_output=True, text=True,
                                check=True, cwd=REPO).stdout.strip()
        main_root = (Path(common) if Path(common).is_absolute() else REPO / common).resolve().parent
        cands.append(main_root / ".venv-undither" / "bin" / "python")
    except Exception:
        pass
    for c in cands:
        if c.exists():
            return str(c)
    return sys.executable


def ensure_learned_bakes(mode="auto"):
    """mode: auto (bake if missing/stale), force, or skip.  Never fatal: without the
    model runtime the page still builds and its viewer says how to get the bakes."""
    if mode == "skip":
        return
    frames = bake_frames()
    if not frames:
        return
    stale = frames if mode == "force" else stale_bakes(frames, BAKE_OUT, BAKE_MODEL)
    if not stale:
        print(f"learned bakes: {len(frames)} up to date in {BAKE_OUT.relative_to(REPO)}")
        return
    if not any(m.exists() for m in BAKE_MODEL):
        print("!! learned bakes: unditherer/models/full.* not found; the Learned CNN preset will have no images")
        return
    py = find_python()
    print(f"learned bakes: {len(stale)} of {len(frames)} missing or stale -> baking with {py}")
    cmd = [py, "-m", "unditherer.infer", "--model", "full", "--in", str(BAKE_SHOTS), "--out", str(BAKE_OUT)]
    r = subprocess.run(cmd, cwd=REPO)
    if r.returncode:
        print("!! learned bakes failed (is onnxruntime or torch installed in that environment?); "
              "the page builds without them -- see unditherer/README.md")


def copy_static():
    """Mirror research/notes/assets/ into the site and return a cache-bust tag.

    Pages that ship their own CSS/JS (the undither viewer, for one) link to
    assets/<dir>/<file>?v=__ASSETV__; the token is substituted at render time so a
    changed asset busts the browser cache the same way wiki.css does."""
    if not STATIC.exists():
        return "0"
    shutil.copytree(STATIC, ASSETS, dirs_exist_ok=True)
    h = hashlib.md5()
    for f in sorted(STATIC.rglob("*")):
        if f.is_file():
            h.update(f.name.encode())
            h.update(str(f.stat().st_size).encode())
            if f.suffix in (".js", ".css", ".json"):
                h.update(f.read_bytes())
    return h.hexdigest()[:10]


def render(title, body, nav_html, toc_html, base, is_index=False, lede=""):
    crumb = "" if is_index else f'<div class="crumb"><a href="{base}index.html">Wiki</a> &nbsp;/&nbsp; {title}</div>'
    ledehtml = f'<p class="lede">{lede}</p>' if lede else ""
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<title>{title} — {SITE_TITLE}</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Saira+Condensed:wght@500;600;700&family=IBM+Plex+Sans:ital,wght@0,400;0,500;0,600;1,400&family=IBM+Plex+Mono:wght@400;600&display=swap">
<link rel="stylesheet" href="{base}assets/wiki.css?v={ASSET_VER}">
</head>
<body>
<header class="topbar">
  <a class="brand" href="{base}index.html">
    <span class="mark">TotalA<em>.exe</em></span>
    <span class="sub">Modding Wiki</span>
  </a>
  <button class="menubtn" id="menubtn" type="button" aria-expanded="false" aria-controls="sidenav">
    <span class="bars" aria-hidden="true"></span> Menu
  </button>
</header>
<div class="scrim" aria-hidden="true"></div>
<div class="shell">
  <aside class="side" id="sidenav">
    <a class="brand" href="{base}index.html">
      <span class="mark">TotalA<em>.exe</em></span>
      <span class="sub">Modding Wiki</span>
    </a>
    <input class="search" id="search" type="search" placeholder="Search the wiki…" aria-label="Search">
    <div id="searchout"></div>
    <div id="nav" class="nav">{nav_html}</div>
    <div class="side-foot">
      Built {date.today().isoformat()} · evidence tagged
      <span class="pill pill-ok">VERIFIED</span> / <span class="pill pill-warn">CLAIMED</span>
      <br><button class="themebtn" id="themebtn" type="button">Toggle theme</button>
    </div>
  </aside>
  <div class="main">
    <article class="doc">{crumb}{ledehtml}{body}</article>
    {toc_html}
  </div>
</div>
<script>var BASE="{base}";</script>
<script src="{base}assets/wiki.js?v={ASSET_VER}"></script>
</body>
</html>
"""


def main(bake="auto"):
    SITE.mkdir(parents=True, exist_ok=True)
    ASSETS.mkdir(parents=True, exist_ok=True)
    (ASSETS / "wiki.css").write_text(CSS)
    (ASSETS / "wiki.js").write_text(JS)
    ensure_learned_bakes(bake)
    static_ver = copy_static()

    present = [(s, l, sec) for s, l, sec in PAGES if (NOTES / f"{s}.md").exists()]
    known = {s for s, _, _ in PAGES}
    for f in sorted(NOTES.glob("*.md")):
        if f.stem.startswith("_"):
            continue
        if f.stem not in known:
            present.append((f.stem, f.stem.replace("-", " ").title(), "Survey"))

    def nav_for(current, base):
        out = []
        for sec in SECTION_ORDER:
            items = [(s, l) for s, l, sc in present if sc == sec]
            if not items:
                continue
            out.append(f'<div class="navgroup"><h4>{sec}</h4>')
            for s, l in items:
                cls = ' class="current"' if s == current else ""
                out.append(f'<a href="{base}{s}.html"{cls}>{l}</a>')
            out.append("</div>")
        return "".join(out)

    search_index = []
    meta = {}

    for slug, label, sec in present:
        md_text = (NOTES / f"{slug}.md").read_text()
        body, toc = build_page(md_text)
        flat = flatten_toc(toc)
        title = flat[0][2] if flat and flat[0][0] == 2 else label
        # H1 is emitted by markdown inside body; find real title from the first H1
        m = re.search(r"^#\s+(.+)$", md_text, flags=re.M)
        if m:
            title = m.group(1).strip()

        heads = [n for d, i, n in flat]
        toc_links = "".join(
            f'<a class="lvl{d}" href="#{i}">{n}</a>' for d, i, n in flat if d in (2, 3)
        )
        toc_html = f'<nav class="toc"><h5>On this page</h5>{toc_links}</nav>' if toc_links else ""

        body = body.replace("__ASSETV__", static_ver)
        plain = strip_html(body)
        search_index.append({"u": f"{slug}.html", "t": title, "h": " ".join(heads), "b": plain[:2600]})
        meta[slug] = {"title": title, "label": label, "section": sec,
                      "words": len(plain.split()), "blurb": blurb_from(md_text, plain)}

        (SITE / f"{slug}.html").write_text(
            render(title, body, nav_for(slug, ""), toc_html, "")
        )

    # ---- index ----
    total_words = sum(m["words"] for m in meta.values())
    cards = {sec: [] for sec in SECTION_ORDER}
    for slug, label, sec in present:
        m = meta[slug]
        cards[sec].append(
            f'<a class="card" href="{slug}.html"><h3>{m["label"]}</h3>'
            f'<p>{m["blurb"]}</p></a>'
        )

    index_md = (NOTES / "_index.md")
    overview_html = ""
    if index_md.exists():
        overview_html, _ = build_page(index_md.read_text())

    sections_html = ""
    for sec in SECTION_ORDER:
        if not cards[sec]:
            continue
        sections_html += (f'<h2 id="{slugify(sec)}">{sec}</h2>'
                          f'<p style="color:var(--ink-3);margin-top:-.4em">{SECTION_BLURB[sec]}</p>'
                          f'<div class="cards">{"".join(cards[sec])}</div>')

    hero = f"""<div class="hero">
<h1>{SITE_TITLE}</h1>
<p class="tag">{SUBTITLE}</p>
</div>
<div class="statrow">
  <div class="stat"><div class="n">{len(present)}</div><div class="l">Pages</div></div>
  <div class="stat"><div class="n">{total_words:,}</div><div class="l">Words</div></div>
  <div class="stat"><div class="n">1997</div><div class="l">Target binary</div></div>
  <div class="stat"><div class="n">MSVC 5.0</div><div class="l">Toolchain</div></div>
</div>"""

    (SITE / "index.html").write_text(
        render("Home", hero + overview_html + sections_html, nav_for(None, ""), "", "", is_index=True)
    )

    (ASSETS / "search.json").write_text(json.dumps(search_index, separators=(",", ":")))
    print(f"built {len(present)} pages + index -> {SITE}  ({total_words:,} words)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--rebake", action="store_true", help="regenerate the undither page's learned bakes even if fresh")
    g.add_argument("--no-bake", action="store_true", help="never run the model; build with whatever bakes exist")
    a = ap.parse_args()
    main("force" if a.rebake else "skip" if a.no_bake else "auto")
