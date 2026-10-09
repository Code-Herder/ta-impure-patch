"""The foot plate under the column, one piece: five button bays (game stats and the four replay overlays),
no lamps (on = the button pressed in)."""
DESIGN = (204, 36)
S = 7.5
CELL = (75, 110)
BAYS = ['stats', 'cursors', 'clicks', 'selections', 'chat']
ZONES = {f'bay_{b}': (5 + 40 * i, 3, 34, 30) for i, b in enumerate(BAYS)}
LEGEND = {
    'bay_stats': 'bay for the game-stats button (a separate part sits in it)',
    'bay_cursors': "bay for the players' cursors switch",
    'bay_clicks': "bay for the players' clicks switch",
    'bay_selections': "bay for the players' selections switch",
    'bay_chat': 'bay for the chat switch',
}
TITLE = 'BUTTON PLATE, ONE PIECE: five bays'
XB = [[z] for z in ZONES]
YB = [list(ZONES)]
