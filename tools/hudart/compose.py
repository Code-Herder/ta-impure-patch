"""compose.py <out.png> <label>=<prefix> ...: one block per button set from compare.sh captures: the transport plate
(play held) beside the stats plate (on), the chip trays, a hovered row (JUMP held). Halved unless NATIVE=1."""
import os, sys
from PIL import Image, ImageDraw, ImageFont
F = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 30)
blocks = []
for arg in sys.argv[2:]:
    lab, pre = arg.split('=', 1)
    tp, ft, ch, hv = (Image.open(f'{pre}_{k}.png').convert('RGB') for k in ('tp', 'foot', 'chips', 'hov'))
    W = max(tp.width + 20 + ft.width, ch.width, hv.width); H = 50 + max(tp.height, ft.height) + 14 + ch.height + 14 + hv.height
    b = Image.new('RGB', (W, H), (28, 28, 28)); d = ImageDraw.Draw(b); d.text((4, 6), lab, font=F, fill=(240, 230, 170))
    y = 50; b.paste(tp, (0, y)); b.paste(ft, (tp.width + 20, y)); y += max(tp.height, ft.height) + 14
    b.paste(ch, (0, y)); y += ch.height + 14; b.paste(hv, (0, y)); blocks.append(b)
W = max(b.width for b in blocks); o = Image.new('RGB', (W, sum(b.height + 30 for b in blocks)), (12, 12, 12)); y = 0
for b in blocks: o.paste(b, (0, y)); y += b.height + 30
(o if os.environ.get('NATIVE') else o.resize((o.width // 2, o.height // 2), Image.LANCZOS)).save(sys.argv[1]); print(o.size)
