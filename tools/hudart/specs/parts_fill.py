"""The timeline's played fill, one long strip per colour: each is cut, then made to repeat along x by
tile.py, because the fill runs up to 1134 units long (HUD 75, 16:9) and a stretched painting smears.
Each colour is a ramp of TA's own palette (unditherer/ta-palette.json): green 232-236, orange 197-200,
amber 193-196 and 209, red 201-205 and 211."""
DOCK = []
COL = [
    ('track_fill_green', 200, 12, 0, 'GREEN fill strip', 'tile'),
    ('track_fill_orange', 200, 12, 0, 'ORANGE fill strip', 'tile'),
    ('track_fill_amber', 200, 12, 0, 'AMBER fill strip', 'tile'),
    ('track_fill_red', 200, 12, 0, 'RED fill strip', 'tile'),
]
WIN = []
ICONS = []
ICON_LABEL = {}
SHEETS = {'col': (COL, 7)}
