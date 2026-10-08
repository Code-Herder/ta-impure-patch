"""The team header painted as one piece: the plate and its recesses, in HUD units (design size 250 x 23)."""
DESIGN = (250, 23)
S = 6
CELL = (90, 110)
ZONES = {
    'title': (3, 3, 112, 17),
    'sum':   (118, 3, 38, 17),
    'm_inc': (159, 3, 88, 8),
    'e_inc': (159, 12, 88, 8),
}
LEGEND = {
    'title': 'team title, with a fold arrow at its left end (the game writes it)',
    'sum': 'the team\'s unit count',
    'm_inc': 'the team\'s metal production + / -',
    'e_inc': 'the team\'s energy production + / -',
}
TITLE = 'TEAM HEADER, ONE PIECE: the plate with its four recesses'
XB = [['title'], ['m_inc', 'e_inc']]
YB = [['m_inc', 'title', 'sum'], ['e_inc', 'title', 'sum']]
