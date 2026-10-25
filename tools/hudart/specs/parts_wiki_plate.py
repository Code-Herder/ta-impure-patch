"""The in-game wiki window's kit in the look `plate`: the window is one heavy worn-steel plate like the stats window
(win_one), its columns are recesses pressed into it, its rows engraved strips, its buttons the dock's warm steel.
Sizes are HUD units (a frame is 720 units tall; the window is drawn about 1150 x 650 at 16:9).

Two layouts, each at an aspect Nano Banana Pro takes exactly, both 6 px a unit:
  window  16:9, 533.33 x 300: the window's 9-slice alone, with pieces of the finished HUD beside it as the reference;
  parts   16:9, 480 x 270:    every other 9-slice, the pressed ones darker, and the two glyphs.
SHEETS is what cut.py reads: (name, w, h, the smallest inset in units, label, flag). PLACE puts each cell on its
layout (x, y in units), RECESS is the dark area drawn inside a cell (inset top, right, bottom, left), GLYPHS are the
icons painted on the parts layout (cut by iconcut.py, not cut.py), REFS the finished pieces placed on each layout:
(part in art/hud-full/parts, its crop in the part's design units or None for all of it, x, y)."""
WINDOW = [
    ('window', 448, 268, 4, 'window', ''),
]
PARTS = [
    ('panel', 120, 190, 3, 'recessed panel', ''),
    ('bezel', 200, 104, 4, 'stage bezel', ''),
    ('row', 200, 18, 3, 'list row', ''),
    ('row_selected', 200, 18, 3, 'list row PRESSED', ''),
    ('button', 72, 16, 3, 'button', ''),
    ('button_down', 72, 16, 3, 'button PRESSED', ''),
    ('keycap', 40, 13, 3, 'key cap', ''),
    ('badge', 98, 14, 3, 'warning plate', ''),
    ('close', 14, 14, 3, 'close', ''),
    ('close_down', 14, 14, 3, 'close PRESSED', ''),
    ('wikibtn', 28, 28, 4, 'top bar button', ''),
    ('wikibtn_down', 28, 28, 4, 'PRESSED', ''),
]
SHEETS = {'window': (WINDOW, 6), 'parts': (PARTS, 6)}
CANVAS = {'window': (3200, 1800), 'parts': (2880, 1620)}
PLACE = {
    'window': (8, 16),
    'panel': (12, 24), 'bezel': (150, 24),
    'row': (150, 148), 'row_selected': (150, 180),
    'button': (150, 214), 'button_down': (236, 214), 'keycap': (322, 215),
    'badge': (370, 24),
    'close': (370, 52), 'close_down': (394, 52),
    'wikibtn': (370, 84), 'wikibtn_down': (406, 84),
}
# the title band (16 units) and the close bay at its right sit in the window's top edge; the body is plain plate,
# on which the game draws the panels and the bezel
RECESS = {
    'window': [(5, 4, 414, 16), (423, 4, 20, 16)],
    'panel': [(2, 2, 116, 186)],
    'bezel': [(7, 7, 186, 90)],
    'row': [(1.5, 1.5, 197, 15)],
    'row_selected': [(1.5, 1.5, 197, 15)],
}
PRESSED = {'row_selected', 'button_down', 'close_down', 'wikibtn_down'}
WARM = {'button', 'button_down', 'keycap', 'close', 'close_down', 'wikibtn', 'wikibtn_down'}
GLYPHS = [('i_wiki', 'book', 370, 132, 22), ('i_x', 'x', 402, 136, 14)]
REFS = {
    'window': [('win_one', (362, 0, 428, 70), 466, 16), ('tp_one', (0, 0, 60, 46.6), 466, 100),
               ('bay_button', None, 466, 160), ('bay_button_down', None, 466, 200)],
    'parts': [('tp_one', (0, 0, 120, 46.6), 12, 221), ('i_rewind', None, 430, 134),
              ('pick_row', (0, 0, 98, 15), 370, 172), ('pick_row_down', (0, 0, 98, 15), 370, 196),
              ('bay_button', None, 370, 228), ('bay_button_down', None, 410, 228)],
}
ICONS = []
ICON_LABEL = {}
