#!/usr/bin/env python3
"""Convert a 480x272 RGB565 raw frame to PNG (2x scale for viewing)."""
import sys
from PIL import Image

W, H = 480, 272
raw, png = sys.argv[1], sys.argv[2]
# "BGR;16" is Pillow's name for little-endian RGB565 (red in the top 5 bits).
img = Image.frombytes('RGB', (W, H), open(raw, 'rb').read(), 'raw', 'BGR;16')
img.resize((W * 2, H * 2), Image.NEAREST).save(png)
