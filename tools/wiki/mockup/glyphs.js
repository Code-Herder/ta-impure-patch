/* glyphs.js -- the wiki window's control glyphs and tree marks as shapes: play, pause and replay on the controls, the
   open and folded marks of a category and the dot of an entry. No HUD face has these characters, so they are kit icons,
   which tools/wiki/cursors.sh rasterises once from shape() below into art/wiki-full/kit/ and the fit carries into
   art/wiki/kit/; the mockup (wiki.js, data-glyph and data-part) and the DLL (tagpu_wikilay.c) both draw those PNGs. They
   follow the kit's own icons, i_close and i_wiki: a pale fill inside a black edge, which reads on a light face and on a
   dark one alike.

   Runs in a page (the global WikiGlyphs) and under node (module.exports), as stage.js does. */
const WikiGlyphs = (() => {
  const FILL = '#cdc8a0', EDGE = '#000';      // the fill and the edge of i_close and i_wiki
  const LINE = 0.7;                           // the edge's width, units
  /* each glyph's box in units: an icon is drawn as large as fits the box it is given (wiki.js drawIcon), so the box's
     margins are the room a glyph keeps round it -- a control's glyph is drawn at 0.6 of its button, a mark fills its
     7-unit mark box */
  const BOX = { i_play: [10, 10], i_pause: [10, 10], i_replay: [10, 10], i_open: [7, 7], i_shut: [7, 7], i_dot: [7, 7] };

  const poly = (g, s, pts) => {
    g.beginPath(); g.moveTo(pts[0][0] * s, pts[0][1] * s);
    for (const [x, y] of pts.slice(1)) g.lineTo(x * s, y * s);
    g.closePath(); g.fill(); g.stroke();
  };

  /* glyph `name` into the context `g` of its box at `s` pixels a unit, its top left at the canvas's */
  function shape(g, name, s) {
    g.fillStyle = FILL; g.strokeStyle = EDGE; g.lineWidth = LINE * s; g.lineJoin = 'round';
    switch (name) {
      case 'i_play': poly(g, s, [[3, 1.8], [8.4, 5], [3, 8.2]]); break;
      case 'i_pause': poly(g, s, [[2.4, 2], [4.4, 2], [4.4, 8], [2.4, 8]]); poly(g, s, [[5.6, 2], [7.6, 2], [7.6, 8], [5.6, 8]]); break;
      case 'i_replay': {
        // an anticlockwise open circle, as U+27F2 draws it: the ring from its head over the top round by the right, the
        // bottom and the left to its tail at the upper left, and the head pointing left, the way it turns
        const cx = 5 * s, cy = 5.5 * s, r = 3 * s, a0 = -0.5 * Math.PI, a1 = 1.06 * Math.PI;
        g.lineCap = 'butt';
        g.beginPath(); g.arc(cx, cy, r, a0, a1, false);
        g.lineWidth = (1 + 2 * LINE) * s; g.strokeStyle = EDGE; g.stroke();
        g.lineWidth = 1 * s; g.strokeStyle = FILL; g.stroke();
        g.lineWidth = LINE * s; g.strokeStyle = EDGE;
        const ex = cx + r * Math.cos(a0), ey = cy + r * Math.sin(a0);
        poly(g, 1, [[ex - 2 * s, ey], [ex + 0.6 * s, ey - 1.8 * s], [ex + 0.6 * s, ey + 1.8 * s]]);
        break;
      }
      case 'i_open': poly(g, s, [[1.4, 2.3], [5.6, 2.3], [3.5, 5.1]]); break;
      case 'i_shut': poly(g, s, [[2.3, 1.4], [5.1, 3.5], [2.3, 5.6]]); break;
      case 'i_dot': g.beginPath(); g.arc(3.5 * s, 3.5 * s, 1.35 * s, 0, 2 * Math.PI); g.fill(); g.stroke(); break;
      default: throw new Error(`glyphs.js: no glyph ${name}`);
    }
  }

  return { shape, BOX };
})();
if (typeof module !== 'undefined') module.exports = WikiGlyphs;
