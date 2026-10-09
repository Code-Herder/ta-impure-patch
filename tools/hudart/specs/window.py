"""The game-stats window painted as one piece: its plate and recesses, in HUD units (design 428 x 272)."""
DESIGN = (428, 272)
S = 3.6
CELL = (60, 90)
ZONES = {
    'title':     (6, 4, 394, 14),
    'close':     (404, 3, 18, 16),
    'tray_eco':  (6, 24, 416, 18),
    'tray_army': (6, 45, 416, 18),
    'chart':     (6, 66, 334, 184),
    'legend':    (344, 66, 78, 184),
    'foot':      (6, 254, 416, 14),
}
LEGEND = {
    'title': 'title strip: a drag grip at its left, then the window title (the game writes them)',
    'close': 'bay for the close button (a separate part sits in it)',
    'tray_eco': 'tray: the label ECONOMY, then choice buttons (separate parts) sitting in it',
    'tray_army': 'tray: the label ARMY, then choice buttons',
    'chart': 'the line chart: axes, grid and lines are drawn by the game',
    'legend': 'the legend: player names and values, drawn by the game',
    'foot': 'caption strip',
}
TITLE = 'GAME STATS WINDOW, ONE PIECE: the plate with its seven recesses'
XB = [['title', 'tray_eco', 'tray_army', 'chart', 'foot']]
YB = [['chart', 'legend']]
