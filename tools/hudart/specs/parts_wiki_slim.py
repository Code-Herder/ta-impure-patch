"""The in-game wiki window's kit in the "slim" look: thin worn-steel bezels round dark recessed faces, the
replay HUD's paint and wear with the least metal. Every 9-slice part with its size on the painted sheet, in HUD
units (a frame is 720 units tall; the window is drawn about 1150 x 650), and where each sits on the job's
16:9 layout. `sheets.py parts_wiki_slim` packs the cells; the job's layout script places them at PLACE with the
recesses in RECESS, the glyphs as GLYPH, and finished replay HUD parts beside them as the reference.

The window's title band is 20 units, as the mockup's `.wt`: a 2.5-unit rim, a 16-unit title groove and the
close well, then a 1.5-unit divider above the body face."""
DOCK = []
# (name, w, h, the smallest 9-slice inset in units, label, flag) as cut.py reads a sheet's parts
COL = [
    ('window', 272, 153, 3, 'window', ''),
    ('bezel', 160, 82, 3, 'stage bezel', ''),
    ('panel', 98, 82, 2, 'panel', ''),
    ('row', 150, 18, 2, 'row', ''),
    ('row_selected', 150, 18, 2, 'row selected', ''),
    ('button', 72, 16, 2, 'button', ''),
    ('button_down', 72, 16, 2, 'button PRESSED', ''),
    ('keycap', 34, 13, 2, 'keycap', ''),
    ('badge', 110, 14, 2, 'warning plate', ''),
    ('close', 14, 14, 2, 'close', ''),
    ('close_down', 14, 14, 2, 'PRESSED', ''),
    ('wikibtn', 18, 18, 3, 'wiki btn', ''),
    ('wikibtn_down', 18, 18, 3, 'PRESSED', ''),
]
WIN = []
ICONS = ['close', 'wiki']
ICON_LABEL = {'close': 'X glyph', 'wiki': 'book glyph'}
SHEETS = {'wiki': (COL, 6)}

# the job's layout: 480 x 270 units (16:9), each part's top-left corner in units
SIZE = (480, 270)
PLACE = {
    'window': (8, 12), 'bezel': (8, 178), 'panel': (182, 178),
    'row': (294, 12), 'row_selected': (294, 40),
    'button': (294, 68), 'button_down': (378, 68),
    'keycap': (294, 94), 'badge': (342, 94),
    'close': (294, 118), 'close_down': (316, 118), 'wikibtn': (340, 116), 'wikibtn_down': (366, 116),
}
# the dark recessed faces inside each part, (x, y, w, h) in the part's units
RECESS = {
    'window': [(2.5, 2.5, 249, 16), (253.5, 2.5, 16, 16), (2.5, 20, 267, 130.5)],
    'bezel': [(4, 4, 152, 74)],
    'panel': [(2.5, 2.5, 93, 77)],
    'badge': [(1.5, 1.5, 107, 11)],
}
# the parts painted as a face rather than a raised plate: the row is a dark strip with an engraved line
FACE = {'row'}
PRESSED = {'button_down', 'close_down', 'wikibtn_down'}
LIT = {'row_selected', 'button', 'keycap', 'close', 'wikibtn'}
# glyph cells: (x, y, size) in units
GLYPH = {'i_close': (398, 117, 16), 'i_wiki': (424, 116, 18)}
