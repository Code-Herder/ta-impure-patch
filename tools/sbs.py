"""side-by-side compositor: sbs.py out.png label1 img1 label2 img2 [...]"""
import sys
from PIL import Image, ImageDraw
out, pairs = sys.argv[1], sys.argv[2:]
imgs = []
for i in range(0, len(pairs), 2):
    label, path = pairs[i], pairs[i+1]
    im = Image.open(path).convert('RGB')
    imgs.append((label, im))
h = max(im.height for _, im in imgs)
tot = sum(im.width for _, im in imgs) + 4 * (len(imgs) - 1)
canvas = Image.new('RGB', (tot, h + 26), (24, 24, 24))
x = 0
for label, im in imgs:
    canvas.paste(im, (x, 26))
    d = ImageDraw.Draw(canvas)
    d.text((x + 6, 6), label, fill=(240, 240, 240))
    x += im.width + 4
canvas.save(out)
print('wrote', out, canvas.size)
