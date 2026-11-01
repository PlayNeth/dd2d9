# compare.py [work dir]: per sample, how much of each screenshot is lit and how far dd2d9's is
# from Wine's own ddraw's (mean difference at 64x48, 0-255). Animated samples differ a little;
# look at the screenshots of any that stand out.
import sys, os, glob
from PIL import Image, ImageChops, ImageStat
R = os.path.join(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), 'work'), 'shots')
for p in sorted(glob.glob(R + '/*-dd.png')):
    n = os.path.basename(p)[:-7]
    a = Image.open(p).convert('RGB')
    b = Image.open(f'{R}/{n}-ref.png').convert('RGB')
    lit = lambda im: sum(1 for px in im.resize((128, 96)).getdata() if max(px) > 16) / (128 * 96)
    small = lambda im: im.resize((64, 48), Image.BILINEAR)
    diff = ImageStat.Stat(ImageChops.difference(small(a), small(b))).mean
    print(f'{n:14s} lit dd {lit(a):5.2f} ref {lit(b):5.2f}  meandiff {sum(diff) / 3:6.1f}')
