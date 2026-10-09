"""The player row painted as one piece: the plate and its recesses, in HUD units (design size 250 x 27)."""
DESIGN = (250, 27)
S = 6                      # px per unit on the sheet (4K at HUD 150 is 4.5)
CELL = (90, 110)           # the row cell's top-left on the sheet, px
ZONES = {                  # name: (x, y, w, h) of each recess, units
    'stripe':  (4, 3, 6, 21),
    'name':    (13, 3, 65, 21),
    'units':   (81, 3, 27, 21),
    'm_store': (111, 3, 82, 10),
    'm_inc':   (196, 3, 50, 10),
    'e_store': (111, 14, 82, 10),
    'e_inc':   (196, 14, 50, 10),
}
LEGEND = {
    'stripe': 'colour stripe slot (the game fills it with the player colour)',
    'name': 'player name (a skull may show at its right end)',
    'units': 'unit count',
    'm_store': 'metal stored / storage (the game draws a thin bar under the numbers)',
    'm_inc': 'metal income + / -',
    'e_store': 'energy stored / storage (thin bar under the numbers)',
    'e_inc': 'energy income + / -',
}
TITLE = 'PLAYER ROW, ONE PIECE: the plate with its seven recesses'
# stretch bands run through the middle of these recesses (a band across must cross every recess listed)
XB = [['name'], ['m_store', 'e_store'], ['m_inc', 'e_inc']]
YB = [['m_store', 'm_inc', 'name', 'units', 'stripe'], ['e_store', 'e_inc', 'name', 'units', 'stripe']]
