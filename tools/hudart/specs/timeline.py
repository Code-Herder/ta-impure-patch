"""The timeline housing painted as one piece, in HUD units (design 420 x 46); drawn as wide as the screen allows."""
DESIGN = (420, 46)
S = 3.8
CELL = (60, 100)
ZONES = {
    'markers': (8, 5, 342, 14),
    'track':   (8, 22, 342, 15),
    'time':    (356, 8, 56, 30),
}
LEGEND = {
    'markers': 'marker lane: round event badges (deaths, bookmarks) sit in it along the game',
    'track': 'the timeline groove: the game draws the played part and the playhead in it',
    'time': 'time readout (1:10 / 2:12)',
}
TITLE = 'TIMELINE HOUSING, ONE PIECE: marker lane, timeline groove, time readout'
XB = [['markers', 'track']]
YB = [['time', 'markers'], ['time', 'track']]
