"""Package frames exported directly from AnimatedLogo.qml. Requires Pillow."""
import argparse
import shutil
from pathlib import Path
from PIL import Image, ImageChops

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('frames', type=Path)
parser.add_argument('--integration', type=Path)
args = parser.parse_args()
assets = Path(__file__).resolve().parents[1] / 'assets'
paths = sorted(args.frames.glob('frame*.png'))
if len(paths) != 76:
    raise SystemExit('Expected 76 frames from export-logo.qml')
rest = Image.open(paths[0]).convert('RGBA')
last = Image.open(paths[-1]).convert('RGBA')
assert ImageChops.difference(rest, last).getbbox() is None, 'Loop must return to rest'
art = rest.crop(rest.getchannel('A').getbbox())
art.resize((564, 752), Image.Resampling.LANCZOS).save(assets / 'app-icon.png')

def square(size):
    image = Image.new('RGBA', (size, size))
    scaled = art.copy()
    scaled.thumbnail((size, size), Image.Resampling.LANCZOS)
    image.alpha_composite(scaled, ((size-scaled.width)//2, (size-scaled.height)//2))
    return image

if args.integration:
    brand = args.integration / 'custom_components/what/brand'
    brand.mkdir(parents=True, exist_ok=True)
    for suffix, size in [('', 256), ('@2x', 512)]:
        square(size).save(brand / f'icon{suffix}.png')
        art.resize((size, round(size*art.height/art.width)), Image.Resampling.LANCZOS).save(brand / f'logo{suffix}.png')

frames = []
for path in paths:
    src = Image.open(path).convert('RGBA').resize((334,424), Image.Resampling.LANCZOS)
    indexed = src.convert('RGB').quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    shifted = indexed.point(lambda n:n+1)
    shifted.putpalette([0,0,0] + indexed.getpalette()[:765])
    shifted.paste(0, mask=src.getchannel('A').point(lambda a:255 if a<128 else 0))
    frames.append(shifted)
gif_path = assets / 'what-logo.gif'
frames[0].save(gif_path, save_all=True, append_images=frames[1:], duration=[20]*75+[180],
              loop=0, transparency=0, disposal=2, optimize=False)
gif = Image.open(gif_path)
duration = 0
for i in range(gif.n_frames):
    gif.seek(i)
    duration += gif.info['duration']
    assert gif.convert('RGBA').getpixel((0,0))[3] == 0
assert duration == 1680
if args.integration:
    target = args.integration / 'assets'
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(gif_path, target / gif_path.name)
print(f'Updated app PNG, brand assets and GIF: {gif.n_frames} frames, {duration} ms, transparent background.')
