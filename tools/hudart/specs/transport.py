"""The transport plate painted as one piece: one bay per button, in HUD units (design 284 x 46)."""
DESIGN = (284, 46)
S = 5.5
CELL = (60, 100)
ZONES = {
    'rew':      (6, 5, 34, 36),
    'play':     (43, 5, 36, 36),
    'fwd':      (82, 5, 34, 36),
    'speed':    (119, 5, 44, 36),
    'bookmark': (166, 5, 32, 36),
    'uiscale':  (201, 5, 40, 36),
    'leave':    (244, 5, 34, 36),
}
LEGEND = {
    'rew': 'bay: rewind button', 'play': 'bay: play / pause button', 'fwd': 'bay: forward button',
    'speed': 'bay: speed button (1x, 2x ...)', 'bookmark': 'bay: bookmark button',
    'uiscale': 'bay: UI scale button (icon + 150%)', 'leave': 'bay: leave the replay',
}
TITLE = 'TRANSPORT PLATE, ONE PIECE: a bay for each button (the buttons are separate parts)'
XB = [['speed'], ['uiscale']]
YB = [['rew', 'play', 'fwd', 'speed', 'bookmark', 'uiscale', 'leave']]
