"""Variation B's new parts (forks as lanes, take over, live rewind): every 9-slice face, its pressed version, and the icons."""
DOCK = []
COL = [
    ('lanes_plate', 150, 46, 5, 'lane plate (lanes sit on it)', ''),
    ('lane', 110, 14, 2.5, 'fork lane', ''),
    ('lane_down', 110, 14, 2.5, 'fork lane PRESSED: watched', ''),
    ('lane_button', 13, 12, 2, 'small button', ''),
    ('lane_button_down', 13, 12, 2, 'small PRESSED', ''),
    ('takeover_button', 140, 20, 4, 'TAKE OVER button', ''),
    ('takeover_button_down', 140, 20, 4, 'TAKE OVER PRESSED', ''),
    ('picker_one', 110, 90, 6, 'picker window', ''),
    ('pick_row', 100, 15, 2.5, 'picker row', ''),
    ('pick_row_down', 100, 15, 2.5, 'picker row PRESSED', ''),
    ('rewind_button', 28, 28, 5, 'top-bar button', ''),
    ('rewind_button_down', 28, 28, 5, 'top-bar PRESSED', ''),
    ('live_one', 160, 34, 5, 'rewind strip plate', ''),
    ('live_groove', 120, 14, 3, 'rewind strip groove', ''),
]
WIN = []
ICONS = ['export', 'delete', 'rewind']
ICON_LABEL = {'export': 'export (save)', 'delete': 'delete X', 'rewind': 'rewind (go back)'}
SHEETS = {'col': (COL, 6)}
