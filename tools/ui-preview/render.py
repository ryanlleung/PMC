#!/usr/bin/env python3
"""Convert a 480x272 RGB565 raw frame to PNG (2x scale for viewing)."""
import struct, sys
from PIL import Image

W, H = 480, 272
raw, png = sys.argv[1], sys.argv[2]
data = open(raw, 'rb').read()
img = Image.new('RGB', (W, H))
px = img.load()
for i, (v,) in enumerate(struct.iter_unpack('<H', data)):
    r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F
    px[i % W, i // W] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
img.resize((W * 2, H * 2), Image.NEAREST).save(png)
