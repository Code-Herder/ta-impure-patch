/* sound.js -- the in-game wiki mockup's sound: tagpu/ddraw/src/tagpu_wikisound.c's rules over WebAudio.

   The sounds are the simulation's (sim.js, tagpu_wikisim.h THE SOUND): every event its view's ring holds past the last
   one played, after each frame's steps (`drain`, as tagpu_wiki.c's sound_drain), and the page's own buttons say the
   allsound name `Options` (`named`). The files are tools/wiki/gfx.py's: `<root>sounds.json` beside the stage packs
   (gfx/ or gfx/side/<mod>/stage/) names each type's category and counts, which is the sim's sound source (`source`),
   each category's WAVs per kind and the button sounds'; the WAVs are copies from the player's install in the gitignored
   pack, served by tools/wiki/serve.py as files.

   THE PLAYER'S SETTINGS ARE THE ENGINE'S DEFAULTS (0x45C570): fxvol 27, speechfx on, unitchat 10, Sound Mode 1, so every
   reply and every named sound plays. Each play is at the engine's -585 (-5.85 dB), times fxvol's share of the wave
   volume the Options slider sets, (27 << 10) / 0xFFFF, which in the game is the process's volume and here the gain.
   Neither sound is positional. A sound has WIKISOUND_VOICES voices (0x4CF570): a stopped one is reused, else a new one,
   else the one furthest through its samples restarts. The page stays silent until the browser lets audio start (the
   first click); a WAV not decoded yet when its event comes is skipped, as a refused one is silent in the game. */
const WikiSound = (() => {
  const VOICES = 4, GAIN = Math.pow(10, -585 / 2000) * ((27 << 10) / 0xffff);
  const NAMES = ['SelectMultipleUnits', 'oktobuild', 'notoktobuild', 'addbuild', 'buildbutton', 'ordersbutton',
    'nextbuildmenu', 'immediateorders', 'Options'];
  const ROOTS = {};               // root -> { doc, clips: file -> AudioBuffer | 'loading' | null }
  let ac = null;

  // the root's sounds.json, read once and synchronously, as cob.js asks the COB host: the sim takes its source from it
  function root(dir) {
    if (!(dir in ROOTS)) {
      let doc = null;
      try {
        const x = new XMLHttpRequest();
        x.open('GET', `${dir}sounds.json`, false);
        x.send();
        if (x.status === 200) doc = JSON.parse(x.responseText);
      } catch (e) { doc = null; }
      ROOTS[dir] = { doc, clips: {} };
    }
    return ROOTS[dir];
  }
  // sim.js's ctx.snd: a type's {cat, count}, null for one the pack gives none
  function source(dir) {
    const d = root(dir).doc;
    return d && d.types ? { type: name => d.types[name] || null } : null;
  }
  function audio() {
    if (!ac && typeof AudioContext !== 'undefined') ac = new AudioContext();
    if (ac && ac.state === 'suspended') ac.resume();
    return ac;
  }
  // the clip's buffer, loading it on first ask; null until it is decoded
  function clip(r, dir, file) {
    if (!file || !audio()) return null;
    const c = r.clips[file];
    if (c === undefined) {
      r.clips[file] = 'loading';
      fetch(`${dir}${file}`).then(x => x.arrayBuffer()).then(b => ac.decodeAudioData(b))
        .then(buf => { r.clips[file] = { buf, voices: [] }; }, () => { r.clips[file] = null; });
      return null;
    }
    return c && c !== 'loading' ? c : null;
  }
  function play(dir, file) {
    const r = root(dir), c = clip(r, dir, file);
    if (!c) return false;
    const now = ac.currentTime;
    // the voices still playing, by how far through the samples each is
    c.voices = c.voices.filter(v => now < v.t0 + c.buf.duration);
    if (c.voices.length >= VOICES) {
      const far = c.voices.reduce((a, v) => (v.t0 < a.t0 ? v : a));
      far.src.stop();
      c.voices.splice(c.voices.indexOf(far), 1);
    }
    const src = ac.createBufferSource(), g = ac.createGain();
    src.buffer = c.buf;
    g.gain.value = GAIN;
    src.connect(g).connect(ac.destination);
    src.start();
    c.voices.push({ src, t0: now });
    return true;
  }
  function reply(dir, cat, kind, variant) {
    const d = root(dir).doc, c = d && d.cats && d.cats[cat], k = c && c.kinds[kind];
    return play(dir, k && k[variant]);
  }
  function named(dir, name) {
    const d = root(dir).doc;
    return play(dir, d && d.named && d.named[name]);
  }
  /* the view's events past `heard` (a count the caller keeps per sim, from 0), played in the order made; returns the
     new count. A burst past the ring's WIKI_SND_LOG is lost, as in the game. */
  function drain(dir, v, heard) {
    const s = v && v.sound;
    if (!s) return heard;
    const first = s.total - s.events.length;
    for (let i = Math.max(0, heard - first); i < s.events.length; i++) {
      const [, kind, name, cat, variant] = s.events[i];
      if (kind) reply(dir, cat, kind, variant);
      else named(dir, NAMES[name]);
    }
    return s.total;
  }
  return { source, drain, named, NAMES };
})();
if (typeof module !== 'undefined') module.exports = WikiSound;
