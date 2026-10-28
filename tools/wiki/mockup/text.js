/* text.js -- the wiki's shown text, filled in. An entry's title, subtitle and paragraphs, its demos' titles and their
   captions name units by cast ({builder}) and mouse buttons by action ({order}); fillText puts in the display names of
   the side the tutorial plays and the buttons of the player's interface type (research/notes/ingame-wiki.md, the owner's
   decisions 16 and 18). Filling is presentation only: the sim and its hash keep the raw text, so neither depends on the
   language, the side's names or the interface type. tagpu_wikitext.c fills by the same rules and the same words.

   The token rule, which both data loaders apply through checkEntry and its C twin: a token is a "{" and everything up to
   the next "}". Its name is one of CASTS (lower case only: a display name carries its own capitals) or a key of BUTTONS
   (the capital form starts a sentence). Any other name, or a "{" with no "}" after it, refuses the entry, so a typo
   fails the build and never reaches the player; a "}" on its own is plain text. Key names (Shift, Alt, Ctrl, Z, X) are
   literal in the text: nothing in an Impure game rebinds them. */
const WikiText = (() => {
  const CASTS = ['builder', 'advbuilder', 'mex', 'moho', 'geo', 'walker', 'tower', 'wall', 'solar'];
  /* each action's word, [interface type 0, type 1]. Type 0 orders with the left button and type 1 with the right; both
     select with the left button and cancel with the right, as the demos' actions resolve (decision 18), so only
     {order} follows the interface type. {select} and {cancel} read the same at both and exist so that no text spells a
     button itself */
  const BUTTONS = {
    order: ['left-click', 'right-click'], Order: ['Left-click', 'Right-click'],
    select: ['click', 'click'], Select: ['Click', 'Click'],
    cancel: ['right-click', 'right-click'], Cancel: ['Right-click', 'Right-click'],
  };
  /* TITLE_MAX: sim.js's, a display name and its terminator (31 code-page bytes at 3 UTF-8 bytes each, and 1). CAPTION_MAX:
     a caption filled and its terminator, the DLL's WIKI_CAPTION_MAX: the record's caption field, which checkEntry keeps
     every caption's fill within whatever the casting */
  const TITLE_MAX = 94, CAPTION_MAX = 512;
  const utf8 = s => new TextEncoder().encode(s).length;
  const isCast = n => CASTS.includes(n);
  const isButton = n => Object.prototype.hasOwnProperty.call(BUTTONS, n);

  /* the words a text is filled with: { cast: {cast name: display name}, iface }. `casting` is one side's record of a
     pack's `casting` list, or null for the entry's own types. A cast the casting binds takes its title (its UnitName
     when the title is empty); any other takes the entry's unit of that cast, by its `name` (its key when it has none),
     so an unbound cast still reads up to the tick its demo locks. `iface` is 1 only when it is exactly 1: the engine's
     left-button readers test main+0x37EFA `== 1` (0x43E505, 0x43F9EF, 0x499046, 0x499567, DISASSEMBLED), so any
     other value plays the left-click interface */
  function words(entry, casting, iface) {
    const cast = {}, units = (entry && entry.units) || {}, bound = (casting && casting.casts) || {};
    for (const c of CASTS) {
      const b = bound[c];
      if (b) { cast[c] = b.title || b.type; continue; }
      const t = Object.keys(units).find(k => units[k] && units[k].cast === c);
      if (t !== undefined) cast[c] = units[t].name || t;
    }
    return { cast, iface: iface === 1 ? 1 : 0 };
  }

  const wordOf = (name, w) => isButton(name) ? BUTTONS[name][w.iface === 1 ? 1 : 0] : isCast(name) ? w.cast[name] : undefined;

  /* `raw` with every token replaced by its word. A token with no word -- one checkEntry refuses, so no loaded entry
     holds one -- is kept as written rather than dropped, so a gap shows on the page instead of reading as a sentence */
  function fillText(raw, w) {
    const s = raw == null ? '' : String(raw);
    let out = '', i = 0;
    for (;;) {
      const a = s.indexOf('{', i);
      if (a < 0) return out + s.slice(i);
      const b = s.indexOf('}', a + 1);
      if (b < 0) return out + s.slice(i);
      const word = wordOf(s.slice(a + 1, b), w);
      out += s.slice(i, a) + (word != null ? word : s.slice(a, b + 1));
      i = b + 1;
    }
  }

  /* null when every token of `raw` is one, and each cast token names a cast in `casts` (a Set): the casts whose word
     is always there, the entry's units' own. Else why not */
  function checkText(raw, casts) {
    const s = raw == null ? '' : String(raw);
    for (let i = 0; ;) {
      const a = s.indexOf('{', i);
      if (a < 0) return null;
      const b = s.indexOf('}', a + 1);
      if (b < 0) return `the "{" at ${a} starts no token`;
      const name = s.slice(a + 1, b);
      if (isCast(name)) { if (!casts.has(name)) return `{${name}}: no unit of the entry has that cast`; }
      else if (!isButton(name)) return `{${name}} is not a token`;
      i = b + 1;
    }
  }

  /* THE CAPTION'S BOUND: the most UTF-8 bytes caption `raw` can fill to, whatever the casting and the interface type --
     its own bytes with a button token at its longer word and a cast token at the longer of TITLE_MAX - 1 bytes, past
     which checkCasting keeps every casting's names, and the entry's own name for that cast, which an unbound cast reads
     (words). Only for a text checkText accepted: every token is a cast or a button */
  function captionWorst(raw, entry) {
    const s = String(raw), own = words(entry, null, 1).cast;
    let n = 0, i = 0;
    for (;;) {
      const a = s.indexOf('{', i), b = a < 0 ? -1 : s.indexOf('}', a + 1);
      if (b < 0) return n + utf8(s.slice(i));
      const name = s.slice(a + 1, b);
      n += utf8(s.slice(i, a)) + (isButton(name) ? Math.max(...BUTTONS[name].map(utf8))
        : Math.max(TITLE_MAX - 1, own[name] != null ? utf8(own[name]) : 0));
      i = b + 1;
    }
  }

  /* every text an entry shows -- its title, subtitle and paragraphs, each demo's title and its inputs' captions --
     through checkText, each caption then through its bound; null, or the first refusal with where it stands */
  function checkEntry(e) {
    const units = (e && e.units) || {};
    const casts = new Set(Object.keys(units).map(k => units[k] && units[k].cast).filter(isCast));
    const texts = [['title', e.title], ['subtitle', e.subtitle]];
    (Array.isArray(e.text) ? e.text : []).forEach((p, i) => texts.push([`text[${i}]`, p]));
    (Array.isArray(e.demos) ? e.demos : []).forEach((d, i) => {
      texts.push([`demos[${i}].title`, d && d.title]);
      for (const ev of (d && Array.isArray(d.inputs) ? d.inputs : [])) if (ev && 'caption' in ev) texts.push([`demos[${i}] caption at ${ev.at}`, ev.caption, true]);
    });
    for (const [where, t, caption] of texts) {
      const why = checkText(t, casts);
      if (why) return `${where}: ${why}`;
      if (caption && typeof t === 'string') {
        const n = captionWorst(t, e);
        if (n >= CAPTION_MAX) return `${where}: it can fill to ${n} bytes, more than the ${CAPTION_MAX - 1} a caption holds`;
      }
    }
    return null;
  }

  return { CASTS, BUTTONS, TITLE_MAX, CAPTION_MAX, words, fillText, checkText, captionWorst, checkEntry };
})();
if (typeof module !== 'undefined') module.exports = WikiText;
