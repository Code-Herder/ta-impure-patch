"""Distressed gunmetal looks: the parts the one-piece HUD draws besides its one-piece plates, and its icons."""
DOCK = []
COL = [
    ('bay_button', 32, 28, 6, 'button face that sits in a bay', ''),
    ('bay_button_down', 32, 28, 6, 'the same button pressed into its bay (on / held)', ''),
    ('chip_face', 56, 15, 4, 'choice button (sits in a tray)', ''),
    ('chip_face_down', 56, 15, 4, 'the same choice button pressed in: selected', ''),
    ('close_face', 16, 14, 3, 'close button (sits in a bay)', ''),
    ('close_face_down', 16, 14, 3, 'close button pressed', ''),
    ('hover_strip', 140, 22, 4, 'action strip shown over a row', ''),
    ('act_button_wide', 48, 18, 4, 'action button (icon + label) on the strip', ''),
    ('act_button_wide_down', 48, 18, 4, 'action button pressed', ''),
    ('scroll_button', 11, 11, 3, 'scroll arrow button', ''),
    ('scroll_track', 8, 80, 4, 'scroll track', ''),
    ('scroll_thumb', 8, 24, 3, 'scroll thumb', ''),
    ('track_fill', 60, 8, 3, 'played part of the timeline', 'core'),
    ('playhead', 6, 22, 0, 'playhead', 'fixed'),
    ('marker_badge', 12, 12, 0, 'round marker badge', 'fixed'),
]
WIN = []
ICONS = ['stats', 'rew', 'play', 'pause', 'fwd', 'bookmark', 'uiscale', 'leave', 'grip', 'close',
         'skull', 'fold_open', 'fold_closed', 'jump', 'sight', 'follow', 'arrow_up', 'arrow_down', 'death', 'defeat', 'flag']
ICON_LABEL = {'stats': 'stats', 'rew': 'rewind', 'fwd': 'forward', 'uiscale': 'UI scale',
              'close': 'close X', 'fold_open': 'team open', 'fold_closed': 'team shut', 'arrow_up': 'up', 'arrow_down': 'down',
              'death': 'RED skull', 'defeat': 'RED X'}
SHEETS = {'col': (COL, 5)}
