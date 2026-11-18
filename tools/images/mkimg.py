# Converts a PNG to an LVGL 9 RGB565 image in C, scaled to a height, near-white made white
# so the picture has no visible edge on the white start-up screen.
# Usage, from the repo root:
#   python3 tools/images/mkimg.py tools/images/mercshore-ipc-logo.png 136 ui_img_logo firmware/ui_img_logo.c
import sys
from PIL import Image
src, h, name, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
im = Image.open(src).convert('RGB')
w = round(im.width * h / im.height)
im = im.resize((w, h), Image.LANCZOS)
data = bytearray()
raw = im.tobytes()
for i in range(0, len(raw), 3):
    r, g, b = raw[i], raw[i + 1], raw[i + 2]
    if min(r, g, b) >= 248:
        r = g = b = 255
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    data += bytes((v & 0xFF, v >> 8))
lines = [f'/* {src.split("/")[-1]}, {w}x{h} RGB565 (built by tools/images/mkimg.py). */',
         '#include "lvgl.h"', '',
         f'static const uint8_t {name}_map[] = {{']
for i in range(0, len(data), 24):
    lines.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + 24]) + ',')
lines += ['};', '',
          f'const lv_image_dsc_t {name} = {{',
          '    .header.magic = LV_IMAGE_HEADER_MAGIC,',
          '    .header.cf = LV_COLOR_FORMAT_RGB565,',
          f'    .header.w = {w},', f'    .header.h = {h},', f'    .header.stride = {w * 2},',
          f'    .data_size = sizeof {name}_map,', f'    .data = {name}_map,', '};', '']
open(out, 'w').write('\n'.join(lines))
print(f'{w}x{h}, {len(data)} bytes')
