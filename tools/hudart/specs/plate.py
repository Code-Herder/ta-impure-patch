"""The stats / map / capture plate, one piece: three button bays, no lamps (on = the button pressed in)."""
DESIGN = (124, 36)
S = 10
CELL = (220, 110)
ZONES = {
    'bay_stats': (5, 3, 34, 30),
    'bay_map':   (45, 3, 34, 30),
    'bay_cam':   (85, 3, 34, 30),
}
LEGEND = {
    'bay_stats': 'bay for the game-stats button (a separate part sits in it)',
    'bay_map': 'bay for the map-overlay button',
    'bay_cam': 'bay for the screen-capture button',
}
TITLE = 'BUTTON PLATE, ONE PIECE: three bays'
XB = [['bay_stats'], ['bay_map'], ['bay_cam']]
YB = [['bay_stats', 'bay_map', 'bay_cam']]
