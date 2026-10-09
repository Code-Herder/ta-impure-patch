"""The team header painted as a solid plate: the team's title is written on the plate itself, so the header
is a field of paint where a row is a frame around dark recesses. Design 250 x 23 units, as `head`."""
DESIGN = (250, 23)
S = 6
CELL = (90, 110)
ZONES = {
    'sum':   (118, 3, 38, 17),
    'm_inc': (159, 3, 88, 8),
    'e_inc': (159, 12, 88, 8),
}
TEXT = {
    'title': (3, 3, 112, 17),
}
LEGEND = {
    'sum': 'the team\'s unit count',
    'm_inc': 'the team\'s metal production + / -',
    'e_inc': 'the team\'s energy production + / -',
    'title': 'the team title, written ON the plate: NO recess here, plain painted plate',
}
TITLE = 'TEAM HEADER AS A SOLID PLATE: three recesses, the title written on the plate'
XB = [['title'], ['m_inc', 'e_inc']]
YB = [['m_inc', 'title', 'sum'], ['e_inc', 'title', 'sum']]
