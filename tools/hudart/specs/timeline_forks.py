"""The timeline housing with the Forks bay, painted as one piece, in HUD units (design 456 x 44, measured off the
V3 full-screen painting); drawn as wide as the screen allows."""
DESIGN = (456, 44)
S = 3.5
CELL = (40, 100)
ZONES = {
    'bay':     (8.5, 5.5, 17.5, 31),
    'markers': (27, 5.5, 353, 12),
    'track':   (27, 21, 353, 15.5),
    'time':    (386, 5.5, 61, 31),
}
LEGEND = {
    'bay': 'Forks bay pocket: the game sets a small upright button in it',
    'markers': 'marker lane: round event badges (deaths, bookmarks) sit in it along the game',
    'track': 'the timeline groove: the game draws the played part and the playhead in it',
    'time': 'time readout (2:00 / 2:50)',
}
TITLE = 'TIMELINE HOUSING WITH THE FORKS BAY, ONE PIECE: bay pocket, marker lane, timeline groove, time readout'
XB = [['markers', 'track']]
YB = [['bay', 'time', 'markers'], ['bay', 'time', 'track']]
