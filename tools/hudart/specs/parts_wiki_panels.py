"""The in-game wiki window's kit, look "panels": separate framed steel panels per column, mounted on a dark
gunmetal backing frame like the dock's housing, their faces the lighter worn paint of the team header plate;
rows like the picker's rows, buttons the dock's warm-steel buttons. Sizes in HUD units (a frame is 720 tall).

Two layouts, each at an aspect Nano Banana Pro takes exactly (16:9), every part placed at its own position:
LAYOUT[sheet] = (width, height, px per unit, parts, refs)
  a part:  (name, x, y, w, h, kind, label, inner)
           kind   'plate' a raised plate, 'gun' the dark gunmetal frame, 'face' a framed panel (a steel rim round
                  a lighter painted face), 'red' the warning plate, 'pressed' a raised part pushed in,
                  'glyph' a stencil glyph on the key (no plate)
           inner  the recesses and faces inside the part, each (what, x, y, w, h) relative to the part, or a
                  glyph drawn on it ('x', 'book')
  a ref:   (finished part in art/hud-full/parts, its crop box in units, where it goes in units)
SHEETS is what cut.py reads: (name, w, h, the smallest inset in units, label, flag); 'fixed' parts are cut whole
(a button with its glyph, drawn at one size), and the glyphs are cut by iconcut.py instead."""
DOCK, WIN, ICONS, ICON_LABEL = [], [], [], {}

BIG = [
    # the window: a heavy gunmetal frame, a title band (16 units) with the close bay at its right, and a broad
    # backing field the panels are mounted on; drawn about 1150 x 650 at 16:9 and 90 %
    ('window', 8, 12, 270, 152, 'gun', 'window: frame, title band, close bay, backing field',
     [('rec', 6, 4, 238, 15), ('rec', 248, 3, 17, 17), ('field', 6, 23, 258, 123)]),
    # a column panel: a steel rim round a light painted face; drawn about 200 x 560 (tree, demos) and 800 x 140 (text)
    ('panel', 292, 12, 110, 160, 'face', 'panel: steel rim, light painted face',
     [('face', 4, 4, 102, 152)]),
    # the frame round the demo stage; the stage canvas covers its dark interior; drawn about 780 x 400
    ('bezel', 8, 180, 166, 85, 'plate', 'bezel: frame round the stage',
     [('rec', 6, 6, 154, 73)]),
]
BIG_REFS = [
    ('win_one', (372, 0, 428, 70), (412, 12)),
    ('tp_one', (124, 0, 284, 46.6), (186, 184)),
    ('head_one', (0, 0, 100, 23.29), (360, 236)),
]

SMALL = [
    # list rows for the tree and the demo list, like the picker's rows: a rim round a dark recess; ~18 tall, ~200 long
    ('row', 10, 12, 200, 18, 'plate', 'list row', [('rec', 1.5, 1.5, 197, 15)]),
    ('row_selected', 10, 42, 200, 18, 'pressed', 'list row PRESSED (selected)', [('rec', 1.5, 1.5, 197, 15)]),
    # control buttons (play, replay, 1x, 2x, Try it yourself): ~16 tall, 20 to 110 long
    ('button', 10, 74, 80, 16, 'plate', 'button', []),
    ('button_down', 100, 74, 80, 16, 'pressed', 'button PRESSED', []),
    # a key cap for the keys strip: ~13 tall, 20 to 70 long
    ('keycap', 190, 75, 44, 13, 'cap', 'key cap', []),
    # the "Turned off in this mod" plate: ~14 tall
    ('badge', 10, 104, 110, 14, 'red', 'warning plate', []),
    # the close button, ~14 square, with its X
    ('close', 132, 104, 14, 14, 'plate', 'close', ['x']),
    ('close_down', 156, 104, 14, 14, 'pressed', 'PRESSED', ['x']),
    # the top bar's wiki button, 28 x 28, with its book, beside the rewind and gear buttons
    ('wikibtn', 180, 104, 28, 28, 'plate', 'wiki button', ['book']),
    ('wikibtn_down', 213, 104, 28, 28, 'pressed', 'wiki PRESSED', ['book']),
    # the two glyphs alone, for iconcut.py
    ('i_wiki', 10, 140, 20, 20, 'glyph', 'book', ['book']),
    ('i_close', 40, 142, 16, 16, 'glyph', 'X', ['x']),
]
SMALL_REFS = [
    ('pick_row', (0, 0, 80, 15), (222, 13)),
    ('pick_row_down', (0, 0, 80, 15), (222, 43)),
    ('act_button_wide', (0, 0, 48.4, 18), (246, 72)),
    ('act_button_wide_down', (0, 0, 48.4, 18), (246, 96)),
    ('rewind_button', None, (256, 124)),          # the whole button, its glyph i_rewind on it, at 28 units
    ('i_rewind', None, (72, 140)),                # the glyph alone, 20 units
]

LAYOUT = {'big': (480, 270, 6, BIG, BIG_REFS), 'small': (320, 180, 9, SMALL, SMALL_REFS)}
_INSET = {'window': 8, 'panel': 4, 'bezel': 6}
_FLAG = {'close': 'fixed', 'close_down': 'fixed', 'wikibtn': 'fixed', 'wikibtn_down': 'fixed'}
SHEETS = {k: ([(n, w, h, _INSET.get(n, 2), lab, _FLAG.get(n, '')) for n, x, y, w, h, kind, lab, inner in v[3]
               if kind != 'glyph'], 0) for k, v in LAYOUT.items()}
