#!/usr/bin/env python3
"""usage: python3 -I tools/frame_grid.py <out.png> <frame.ppm>...  — downscaled 2-column grid of dumped frames (PPM from KYTY_DUMP_FRAME)."""
import sys
from PIL import Image
out, files = sys.argv[1], sys.argv[2:]
W = Image.new('RGB', (960, 270 * ((len(files) + 1) // 2)))
for k, f in enumerate(files):
    W.paste(Image.open(f).convert('RGB').resize((480, 270)), ((k % 2) * 480, (k // 2) * 270))
W.save(out)
