/* bar.js -- the game frame's two bars in the running mod's own art: the resource bar along the top (DrawGameScreen
   0x468E3F..0x46960B) and the strip along the bottom (0x46A860), composed as the engine composes them, in palette
   indices, from the mod's sidebar pack (side.js read: art.bar, art.logos, art.gui, PANELSIDE). The engine's rules
   are DISASSEMBLED, each cited where it is applied (research/notes/ingame-wiki-replay-plan.md, *What R0 found*),
   and the result is pixel-exact against a capture of the game frame (tools/wiki/barcheck.mjs).

   compose() is a pure function: art, the side, the badge colour, the frame's size and the numbers in; palette
   indices out. The DLL's tagpu_wikibar.c is to be its port, compared byte for byte. All arithmetic is in doubles,
   ftol is the engine's 0x4E43A0 under RC=11 (truncation), and every write is clipped to the picture.

   THE PICTURE is BAR_W x BAR_H indices, BAR_W a row: rows 0..31 the top bar, rows 32..63 the bottom bar, each
   w = Wg - 128 columns wide, column x being the frame's column 128 + x. Column 0 is the frame's column 128, which
   PANELSIDE covers where it is 129 columns wide (retail's): the bars themselves start at the frame's 129.

   Runs in a page (the global WikiBar) and under node (module.exports), as side.js does. */
const WikiBar = (() => {
  const BAR_W = 2048;              // the widest bar: frame x 128 .. 128 + 16 x WIKISTAGE_MAP (128 cells) - 1
  const BAR_H = 64;                // the top bar's 32 rows, then the bottom bar's 32
  const ROWS = 32;
  const COL = 128;                 // the frame column the picture's column 0 is
  const TILE_X = 0x81;             // 129: the first tile's frame column, 0x469050 and 0x46A860
  const GUI_INCOME = 0xA, GUI_EXPENSE = 0xC, GUI_TEXT = 0xF;   // the GUI colour table entries the bar inks with
  const ZERO = { e: 0, m: 0, emax: 0, mmax: 0, ei: 0, eo: 0, mi: 0, mo: 0, raw_e: 0, raw_m: 0, lev_e: 0, lev_m: 0 };

  /* 0x4E43A0, fistp under RC=11: truncation toward zero; a value outside int32 (or NaN) gives the integer
     indefinite, -2^31, as the x87 does. */
  function ftol(v) {
    const t = Math.trunc(v);
    return t >= -2147483648 && t <= 2147483647 ? t : -2147483648;
  }
  // `cdq; xor; sub`: the int32 absolute value, whose -2^31 stays -2^31
  const iabs = v => v === -2147483648 ? v : Math.abs(v);
  /* "%.1f", as an explicit rule both ports share: r = floor(|v| x 10 + 0.5) in doubles, then r / 10 "." r % 10, with a
     minus sign when v < 0 and r > 0. The engine CRT's rounding of an exact tie (2.25) is [INFERRED] to be the same;
     a live capture with non-zero metal rates is the check. */
  function fixed1(v) {
    const r = Math.floor(Math.abs(v) * 10 + 0.5), s = `${Math.floor(r / 10)}.${r % 10}`;
    return v < 0 && r > 0 ? `-${s}` : s;
  }

  // a font glyph's offset for code c, 0 none: the table starts at byte 4, the first code is 0 (side.js fntProblem)
  const glyphAt = (font, c) => font[4 + 2 * c] | font[5 + 2 * c] << 8;
  // 0x4C1480: the string's width, the glyphs' widths summed up to a newline
  function textWidth(font, s) {
    let w = 0;
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i) & 0xff;
      if (c === 10) break;
      const o = glyphAt(font, c);
      if (o) w += font[o];
    }
    return w;
  }

  /* THE BARS for layout side `side` with badge colour `colour`, in a frame Wg x Hg game pixels, showing the numbers
     `num` ({e, m: the eased stored energy and metal; emax, mmax: the storages; ei, eo, mi, mo: the rates, income and
     expense; raw_e, raw_m: the stored values and lev_e, lev_m: the share levels, for the ticks}). Returns { idx, w }
     or null when the side has no bar art (the stage draws the whitebox strips) or the frame is outside
     129..128 + BAR_W columns or under 64 rows. */
  function compose(art, side, colour, Wg, Hg, num = ZERO) {
    const B = art && art.bar && art.bar[side], gui = art && art.gui;
    if (!B || !gui || !Number.isInteger(Wg) || !Number.isInteger(Hg) || Wg - COL < 1 || Wg - COL > BAR_W || Hg < 2 * ROWS)
      return null;
    const w = Wg - COL, idx = new Uint8Array(BAR_W * BAR_H), S = art.sheet, R = B.rects, font = B.font;

    // a frame {f: [sx, sy, w, h, hx, hy], key} with its top left at picture (dx, dy) whatever its hotspot, its key
    // index skipped, clipped to the columns and to rows [y0, y1)
    const blit = (fr, dx, dy, y0, y1) => {
      const [sx, sy, fw, fh] = fr.f;
      for (let r = Math.max(0, y0 - dy); r < fh && dy + r < y1; r++) {
        const src = (sy + r) * S.w + sx, dst = (dy + r) * BAR_W + dx;
        for (let c = Math.max(0, -dx); c < fw && dx + c < w; c++) {
          const p = S.px[src + c];
          if (p !== fr.key) idx[dst + c] = p;
        }
      }
    };
    // a frame pixel of the top bar, clipped to the picture (frame column fx, row fy)
    const put = (fx, fy, p) => {
      const x = fx - COL;
      if (x >= 0 && x < w && fy >= 0 && fy < ROWS) idx[fy * BAR_W + x] = p;
    };

    /* COLUMN 0 is the frame's column 128: PANELSIDE's column 128 where PANELSIDE has it (it lands with its top left
       at the frame's corner, side.js compose), else the engine's clear, index 0. On the bottom bar's rows it is
       PANELSIDE's row Hg - 32 + r, below the panel's picture the clear. */
    const ps = art.panelside && art.panelside[side];
    if (ps && ps.f[2] > COL) {
      const [sx, sy, , fh] = ps.f;
      const at = row => { const p = S.px[(sy + row) * S.w + sx + COL]; return p === ps.key ? 0 : p; };
      for (let r = 0; r < ROWS; r++) {
        if (r < fh) idx[r * BAR_W] = at(r);
        if (Hg - ROWS + r < fh) idx[(ROWS + r) * BAR_W] = at(Hg - ROWS + r);
      }
    }

    /* THE TILES, 0x469050..0x46909D: from frame column 129 while x < Wg, PANELTOP[side] for the first tile and
       PANELBOT[side] for every later one (the index s + 5 x (x > 0x81)), each frame 0 at its top left, x advanced by
       its width. PANELBOT is 33 rows; its last lands on the viewport's first row, outside this picture. */
    for (let x = TILE_X, first = true; x < Wg; first = false) {
      const fr = first ? B.top : B.bot;
      blit(fr, x - COL, 0, 0, ROWS);
      x += fr.f[2];
    }

    /* THE BADGE, 0x467C00 through GAF_DrawTransformed 0x4C7580: frame `colour` of the logos sequence into the LOGO
       rect, half-open, each step the 16.16 accumulator (fw << 16) / dw. THE ENGINE HOLDS NO BOUND on the colour
       (record 10's colour 10 crashed at 0x4C74C7, research/notes/engine/recorder.md); this one is held below the
       sequence's frame count, and past it no badge is drawn. */
    const L = art.logos;
    if (L && Number.isInteger(colour) && colour >= 0 && colour < L.length) {
      const fr = L[colour], [sx, sy, fw, fh] = fr.f, [x1, y1, x2, y2] = R.logo, dw = x2 - x1, dh = y2 - y1;
      if (dw > 0 && dh > 0) {
        const su = Math.floor(fw * 65536 / dw), sv = Math.floor(fh * 65536 / dh);
        for (let r = 0; r < dh; r++) for (let c = 0; c < dw; c++) {
          const p = S.px[(sy + Math.floor(r * sv / 65536)) * S.w + sx + Math.floor(c * su / 65536)];
          if (p !== fr.key) put(x1 + c, y1 + r, p);
        }
      }
    }

    // DrawBar 0x4BF6F0 -> 0x4CCDEA: inclusive of both ends, after the clip to the surface 0x4BF620
    const fill = (l, t, r, b, col) => {
      for (let y = Math.max(t, 0); y <= Math.min(b, ROWS - 1); y++)
        for (let x = Math.max(l, COL); x <= Math.min(r, Wg - 1); x++) idx[y * BAR_W + x - COL] = col;
    };
    /* A FILL AND ITS TICK, both skipped at storage 0 (0x4690BD, 0x4690ED): x1..ftol(x1 + (x2 - x1) v / max); the tick
       three columns T..T+2 in GUI[0xC], only when the level is not 0 and the raw stored value is above it. */
    const bar = (rect, v, max, col, raw, level) => {
      if (max === 0) return;
      const [l, t, r, b] = R[rect];
      fill(l, t, ftol(l + (r - l) * v / max), b, col);
      if (level !== 0 && raw > level) {
        const T = ftol(l + (r - l) * level / max);
        fill(T, t, T + 2, b, gui[GUI_EXPENSE]);
      }
    };
    /* DrawTextCustomFont 0x4C14F0, one ink and no background (0x4CCF60 skips the clear bits). The string is REFUSED
       WHOLE unless its box {x, y, x + width, y + rows} lies inside the surface's clip {0, 0, Wg - 1, Hg - 1}
       (0x4C1629..0x4C169E, the test 0x4B6750, the clip 0x4C69C0 set at 0x468E3A; DISASSEMBLED). Drawn, each glyph's
       top row is y - (int8)font[2], its bits MSB-first across rows, x advanced by its width; a code with no glyph
       draws nothing. What lands outside the top bar is clipped. */
    const rows = font[0], yoff = font[2] << 24 >> 24;
    const text = (s, x, y, col) => {
      const tw = textWidth(font, s);
      if (x < 0 || x > Wg - 1 || x + tw < 0 || x + tw > Wg - 1 || y < 0 || y > Hg - 1 || y + rows < 0 || y + rows > Hg - 1)
        return;
      for (let i = 0; i < s.length; i++) {
        const o = glyphAt(font, s.charCodeAt(i) & 0xff);
        if (!o) continue;
        const gw = font[o];
        for (let r = 0, k = 0; r < rows; r++) for (let c = 0; c < gw; c++, k++)
          if (font[o + 1 + (k >> 3)] & (0x80 >> (k & 7))) put(x + c, y - yoff + r, col);
        x += gw;
      }
    };
    const at = (s, rect, col) => text(s, R[rect][0], R[rect][1], col);
    const right = (s, rect, col) => text(s, R[rect][0] - textWidth(font, s), R[rect][1], col);

    // the energy fill and tick, then its five strings (0x4690B0..0x46938C)
    bar('energybar', num.e, num.emax, B.ecol, num.raw_e, num.lev_e);
    at(`${ftol(num.e)}`, 'energynum', gui[GUI_TEXT]);
    at('0', 'energy0', gui[GUI_TEXT]);
    right(`${ftol(num.emax)}`, 'energymax', gui[GUI_TEXT]);
    // income past 99999 and expense below -99999 take "%dK" of trunc(ftol(v) / 1000); the expense's test is the
    // income's mirrored, so A POSITIVE EXPENSE NEVER TAKES THE K: the engine's quirk, copied. The expense's compare
    // is written so a NaN takes the K, as fcomp's unordered C0 does (0x469311).
    const kilo = v => `${Math.trunc(ftol(v) / 1000)}K`;
    at(num.ei > 99999 ? kilo(num.ei) : `${ftol(num.ei)}`, 'energyproduced', gui[GUI_INCOME]);
    at(!(num.eo >= -99999) ? kilo(num.eo) : `${iabs(ftol(num.eo))}`, 'energyconsumed', gui[GUI_EXPENSE]);
    // the metal fill and tick, then its strings, the rates "%.1f" (0x469391..0x46960B)
    bar('metalbar', num.m, num.mmax, B.mcol, num.raw_m, num.lev_m);
    at(`${ftol(num.m)}`, 'metalnum', gui[GUI_TEXT]);
    at('0', 'metal0', gui[GUI_TEXT]);
    right(`${ftol(num.mmax)}`, 'metalmax', gui[GUI_TEXT]);
    at(fixed1(num.mi), 'metalproduced', gui[GUI_INCOME]);
    at(fixed1(Math.abs(num.mo)), 'metalconsumed', gui[GUI_EXPENSE]);

    /* THE BOTTOM BAR, 0x46A860 (0x46A8A6..0x46A8D5): PANELBOT[side] tiled from frame column 129 along the frame's
       last 32 rows, and nothing else on it while nothing is selected. */
    for (let x = TILE_X; x < Wg; x += B.bot.f[2]) blit(B.bot, x - COL, ROWS, ROWS, BAR_H);

    return { idx, w };
  }

  // the composed bars as RGBA through the pack's palette, BAR_W a row; the columns past w stay transparent
  function rgba(art, c) {
    const out = new Uint8ClampedArray(BAR_W * BAR_H * 4), pal = art.pal;
    for (let y = 0; y < BAR_H; y++) for (let x = 0; x < c.w; x++) {
      const i = y * BAR_W + x, p = c.idx[i] * 4;
      out[i * 4] = pal[p]; out[i * 4 + 1] = pal[p + 1]; out[i * 4 + 2] = pal[p + 2]; out[i * 4 + 3] = 255;
    }
    return out;
  }

  return { compose, rgba, ftol, fixed1, textWidth, BAR_W, BAR_H, COL, ZERO };
})();
if (typeof module !== 'undefined') module.exports = WikiBar;
