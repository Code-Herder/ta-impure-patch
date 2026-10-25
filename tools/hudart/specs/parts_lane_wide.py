"""The watched fork lane's Back to main recording button and its pressed version, painted at about the size the
lane draws them: as wide as their label (79 units at layout.json's `face_xs`), as tall as the lane's other
buttons. Drawn from the 13-unit lane_button instead, its highlight stretched six times into a streak."""
DOCK = []
COL = [
    ('lane_wide_button', 79, 12, 2, 'wide button', ''),
    ('lane_wide_button_down', 79, 12, 2, 'wide PRESSED', ''),
]
WIN = []
ICONS = []
ICON_LABEL = {}
SHEETS = {'col': (COL, 10)}
