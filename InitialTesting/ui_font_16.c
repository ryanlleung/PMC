/* unscii-16 (public domain, viznut.fi/unscii), lv_font_conv 1.5.3, 1 bpp for the Pokemon (Game Boy) look. ASCII, degree, plus-minus, micro, en dash, bullet; LV_SYMBOL glyphs from the fallback. */
/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Opts: --no-compress --no-prefilter --bpp 1 --size 16 --font unscii-16.ttf -r 0x20-0x7E,0xB0,0xB1,0xB5,0x2022,0x2013 --format lvgl --lv-font-name ui_font_16 --lv-fallback lv_font_montserrat_14 -o out/ui_font_16.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef UI_FONT_16
#define UI_FONT_16 1
#endif

#if UI_FONT_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0xff, 0xf,

    /* U+0022 "\"" */
    0xcf, 0x3c, 0xc0,

    /* U+0023 "#" */
    0x6c, 0xd9, 0xb7, 0xf6, 0xcd, 0x9b, 0x7f, 0x6c,
    0xd9, 0xb0,

    /* U+0024 "$" */
    0x30, 0xc7, 0xb3, 0xc1, 0x83, 0x6, 0xf, 0x37,
    0x8c, 0x30,

    /* U+0025 "%" */
    0x7, 0x8f, 0x36, 0x61, 0x83, 0xc, 0x18, 0x66,
    0xcf, 0x1e, 0x0,

    /* U+0026 "&" */
    0x38, 0xd9, 0xb1, 0xc3, 0xf, 0x77, 0xe6, 0xcd,
    0x99, 0xd8,

    /* U+0027 "'" */
    0x6d, 0xe0,

    /* U+0028 "(" */
    0x36, 0x6c, 0xcc, 0xcc, 0xcc, 0x66, 0x30,

    /* U+0029 ")" */
    0xc6, 0x63, 0x33, 0x33, 0x33, 0x66, 0xc0,

    /* U+002A "*" */
    0x66, 0x66, 0x3c, 0xff, 0x3c, 0x66, 0x66,

    /* U+002B "+" */
    0x30, 0xc3, 0x3f, 0x30, 0xc3, 0x0,

    /* U+002C "," */
    0x73, 0x36, 0xc0,

    /* U+002D "-" */
    0xfc,

    /* U+002E "." */
    0xfc,

    /* U+002F "/" */
    0x3, 0x3, 0x6, 0x6, 0xc, 0xc, 0x18, 0x18,
    0x30, 0x30, 0x60, 0x60, 0xc0, 0xc0,

    /* U+0030 "0" */
    0x38, 0xdb, 0x1e, 0x3c, 0xfa, 0xf9, 0xe3, 0xc6,
    0xd8, 0xe0,

    /* U+0031 "1" */
    0x31, 0xcf, 0xc, 0x30, 0xc3, 0xc, 0x30, 0xcf,
    0xc0,

    /* U+0032 "2" */
    0x7b, 0x3c, 0xc3, 0xc, 0x63, 0x18, 0xc3, 0xf,
    0xc0,

    /* U+0033 "3" */
    0x7b, 0x3c, 0xc3, 0xc, 0xe0, 0xc3, 0xcf, 0x37,
    0x80,

    /* U+0034 "4" */
    0xc, 0x38, 0xf3, 0x6c, 0xd9, 0xbf, 0x86, 0xc,
    0x18, 0x30,

    /* U+0035 "5" */
    0xff, 0xc, 0x30, 0xf8, 0x30, 0xc3, 0xcf, 0x37,
    0x80,

    /* U+0036 "6" */
    0x39, 0x8c, 0x30, 0xfb, 0x3c, 0xf3, 0xcf, 0x37,
    0x80,

    /* U+0037 "7" */
    0xfc, 0x30, 0xc3, 0x18, 0x63, 0xc, 0x30, 0xc3,
    0x0,

    /* U+0038 "8" */
    0x7b, 0x3c, 0xf3, 0xed, 0xed, 0xf3, 0xcf, 0x37,
    0x80,

    /* U+0039 "9" */
    0x7b, 0x3c, 0xf3, 0xcd, 0xf0, 0xc3, 0xc, 0x67,
    0x0,

    /* U+003A ":" */
    0xfc, 0x3, 0xf0,

    /* U+003B ";" */
    0x33, 0x30, 0x0, 0x7, 0x33, 0x6c,

    /* U+003C "<" */
    0xc, 0x63, 0x18, 0xc1, 0x83, 0x6, 0xc,

    /* U+003D "=" */
    0xfc, 0x0, 0x0, 0xfc,

    /* U+003E ">" */
    0xc1, 0x83, 0x6, 0xc, 0x63, 0x18, 0xc0,

    /* U+003F "?" */
    0x7b, 0x3c, 0xc3, 0x18, 0xc3, 0xc, 0x0, 0x3,
    0xc,

    /* U+0040 "@" */
    0x7d, 0x8f, 0x1e, 0x3d, 0xfb, 0xf7, 0xee, 0xc1,
    0x81, 0xf0,

    /* U+0041 "A" */
    0x31, 0xec, 0xf3, 0xcf, 0xfc, 0xf3, 0xcf, 0x3c,
    0xc0,

    /* U+0042 "B" */
    0xfb, 0x3c, 0xf3, 0xdb, 0xcd, 0xb3, 0xcf, 0x3f,
    0x80,

    /* U+0043 "C" */
    0x7b, 0x3c, 0xf0, 0xc3, 0xc, 0x30, 0xcf, 0x37,
    0x80,

    /* U+0044 "D" */
    0xf3, 0x6c, 0xf3, 0xcf, 0x3c, 0xf3, 0xcf, 0x6f,
    0x0,

    /* U+0045 "E" */
    0xff, 0xc, 0x30, 0xc3, 0xec, 0x30, 0xc3, 0xf,
    0xc0,

    /* U+0046 "F" */
    0xff, 0xc, 0x30, 0xfb, 0xc, 0x30, 0xc3, 0xc,
    0x0,

    /* U+0047 "G" */
    0x7b, 0x3c, 0xf0, 0xc3, 0x7c, 0xf3, 0xcf, 0x37,
    0xc0,

    /* U+0048 "H" */
    0xcf, 0x3c, 0xf3, 0xcf, 0xfc, 0xf3, 0xcf, 0x3c,
    0xc0,

    /* U+0049 "I" */
    0xfc, 0xc3, 0xc, 0x30, 0xc3, 0xc, 0x30, 0xcf,
    0xc0,

    /* U+004A "J" */
    0xc, 0x30, 0xc3, 0xc, 0x30, 0xc3, 0xcf, 0x37,
    0x80,

    /* U+004B "K" */
    0xc7, 0x8f, 0x36, 0x6d, 0x9e, 0x36, 0x66, 0xcd,
    0x8f, 0x18,

    /* U+004C "L" */
    0xc3, 0xc, 0x30, 0xc3, 0xc, 0x30, 0xc3, 0xf,
    0xc0,

    /* U+004D "M" */
    0xc7, 0xdf, 0xbf, 0xfd, 0x7a, 0xf1, 0xe3, 0xc7,
    0x8f, 0x18,

    /* U+004E "N" */
    0xc7, 0x8f, 0x9f, 0x3f, 0x7f, 0xf7, 0xe7, 0xcf,
    0x8f, 0x18,

    /* U+004F "O" */
    0x7b, 0x3c, 0xf3, 0xcf, 0x3c, 0xf3, 0xcf, 0x37,
    0x80,

    /* U+0050 "P" */
    0xfb, 0x3c, 0xf3, 0xcf, 0xec, 0x30, 0xc3, 0xc,
    0x0,

    /* U+0051 "Q" */
    0x7b, 0x3c, 0xf3, 0xcf, 0x3c, 0xf3, 0xcf, 0x37,
    0x86, 0xc,

    /* U+0052 "R" */
    0xfb, 0x3c, 0xf3, 0xcf, 0xed, 0xb3, 0xcf, 0x3c,
    0xc0,

    /* U+0053 "S" */
    0x7b, 0x3c, 0xf0, 0x60, 0xc1, 0x83, 0xcf, 0x37,
    0x80,

    /* U+0054 "T" */
    0xfc, 0xc3, 0xc, 0x30, 0xc3, 0xc, 0x30, 0xc3,
    0x0,

    /* U+0055 "U" */
    0xcf, 0x3c, 0xf3, 0xcf, 0x3c, 0xf3, 0xcf, 0x37,
    0x80,

    /* U+0056 "V" */
    0xcf, 0x3c, 0xf3, 0xcf, 0x3c, 0xde, 0x78, 0xc3,
    0x0,

    /* U+0057 "W" */
    0xc7, 0x8f, 0x1e, 0x3c, 0x7a, 0xf5, 0xff, 0xef,
    0xdf, 0x18,

    /* U+0058 "X" */
    0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x18, 0x18, 0x3c,
    0x66, 0xc3, 0xc3,

    /* U+0059 "Y" */
    0xc3, 0xc3, 0x66, 0x66, 0x3c, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18,

    /* U+005A "Z" */
    0xfc, 0x30, 0xc6, 0x18, 0xc6, 0x18, 0xc3, 0xf,
    0xc0,

    /* U+005B "[" */
    0xfc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xf0,

    /* U+005C "\\" */
    0xc0, 0xc0, 0x60, 0x60, 0x30, 0x30, 0x18, 0x18,
    0xc, 0xc, 0x6, 0x6, 0x3, 0x3,

    /* U+005D "]" */
    0xf3, 0x33, 0x33, 0x33, 0x33, 0x33, 0xf0,

    /* U+005E "^" */
    0x10, 0x71, 0xb3, 0x6c, 0x78, 0xc0,

    /* U+005F "_" */
    0xff,

    /* U+0060 "`" */
    0xcc, 0x63,

    /* U+0061 "a" */
    0x78, 0x37, 0xf3, 0xcf, 0x37, 0xc0,

    /* U+0062 "b" */
    0xc3, 0xc, 0x30, 0xfb, 0x3c, 0xf3, 0xcf, 0x3f,
    0x80,

    /* U+0063 "c" */
    0x7b, 0x3c, 0x30, 0xc3, 0x37, 0x80,

    /* U+0064 "d" */
    0xc, 0x30, 0xc3, 0x7f, 0x3c, 0xf3, 0xcf, 0x37,
    0xc0,

    /* U+0065 "e" */
    0x7b, 0x3c, 0xff, 0xc3, 0x7, 0x80,

    /* U+0066 "f" */
    0x3d, 0x86, 0x18, 0xfd, 0x86, 0x18, 0x61, 0x86,
    0x0,

    /* U+0067 "g" */
    0x7f, 0x3c, 0xf3, 0xcf, 0x37, 0xc3, 0xf, 0xe0,

    /* U+0068 "h" */
    0xc3, 0xc, 0x30, 0xfb, 0x3c, 0xf3, 0xcf, 0x3c,
    0xc0,

    /* U+0069 "i" */
    0x30, 0xc0, 0x0, 0xf0, 0xc3, 0xc, 0x30, 0xc3,
    0xc0,

    /* U+006A "j" */
    0x18, 0xc0, 0x1, 0x8c, 0x63, 0x18, 0xc6, 0x31,
    0xf8,

    /* U+006B "k" */
    0xc3, 0xc, 0x30, 0xcf, 0x3d, 0xbc, 0xdb, 0x3c,
    0xc0,

    /* U+006C "l" */
    0xf0, 0xc3, 0xc, 0x30, 0xc3, 0xc, 0x30, 0xc3,
    0xc0,

    /* U+006D "m" */
    0xcd, 0xff, 0x5e, 0xbd, 0x7a, 0xf1, 0x80,

    /* U+006E "n" */
    0xfb, 0x3c, 0xf3, 0xcf, 0x3c, 0xc0,

    /* U+006F "o" */
    0x7b, 0x3c, 0xf3, 0xcf, 0x37, 0x80,

    /* U+0070 "p" */
    0xfb, 0x3c, 0xf3, 0xcf, 0x3f, 0xb0, 0xc3, 0x0,

    /* U+0071 "q" */
    0x7f, 0x3c, 0xf3, 0xcf, 0x37, 0xc3, 0xc, 0x30,

    /* U+0072 "r" */
    0xfb, 0x3c, 0xf0, 0xc3, 0xc, 0x0,

    /* U+0073 "s" */
    0x7f, 0xc, 0x1e, 0xc, 0x3f, 0x80,

    /* U+0074 "t" */
    0x61, 0x86, 0x3f, 0x61, 0x86, 0x18, 0x60, 0xf0,

    /* U+0075 "u" */
    0xcf, 0x3c, 0xf3, 0xcf, 0x37, 0xc0,

    /* U+0076 "v" */
    0xcf, 0x3c, 0xf3, 0xcd, 0xe3, 0x0,

    /* U+0077 "w" */
    0xc7, 0x8f, 0x5e, 0xbd, 0x6f, 0x9b, 0x0,

    /* U+0078 "x" */
    0xc7, 0x8d, 0xb1, 0xc6, 0xd8, 0xf1, 0x80,

    /* U+0079 "y" */
    0xcf, 0x3c, 0xf3, 0xcf, 0x37, 0xc3, 0xd, 0xe0,

    /* U+007A "z" */
    0xfc, 0x31, 0x8c, 0x63, 0xf, 0xc0,

    /* U+007B "{" */
    0xe, 0x30, 0x60, 0xc1, 0x83, 0x3c, 0xc, 0x18,
    0x30, 0x60, 0xc0, 0xe0,

    /* U+007C "|" */
    0xff, 0xff, 0xff, 0xf0,

    /* U+007D "}" */
    0xe0, 0x60, 0xc1, 0x83, 0x6, 0x7, 0x98, 0x30,
    0x60, 0xc1, 0x8e, 0x0,

    /* U+007E "~" */
    0x73, 0xae, 0x70,

    /* U+00B0 "°" */
    0x7b, 0x3c, 0xde,

    /* U+00B1 "±" */
    0x30, 0xc3, 0x3f, 0x30, 0xc3, 0x0, 0xfc,

    /* U+00B5 "µ" */
    0x66, 0xcd, 0x9b, 0x36, 0x6c, 0xdf, 0x30, 0x61,
    0x80,

    /* U+2013 "–" */
    0xfe,

    /* U+2022 "•" */
    0x66, 0xff, 0xff, 0x66
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 128, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 16},
    {.bitmap_index = 1, .adv_w = 128, .box_w = 2, .box_h = 12, .ofs_x = 3, .ofs_y = 3},
    {.bitmap_index = 4, .adv_w = 128, .box_w = 6, .box_h = 3, .ofs_x = 1, .ofs_y = 12},
    {.bitmap_index = 7, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 17, .adv_w = 128, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 27, .adv_w = 128, .box_w = 7, .box_h = 12, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 38, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 48, .adv_w = 128, .box_w = 3, .box_h = 4, .ofs_x = 2, .ofs_y = 11},
    {.bitmap_index = 50, .adv_w = 128, .box_w = 4, .box_h = 13, .ofs_x = 2, .ofs_y = 2},
    {.bitmap_index = 57, .adv_w = 128, .box_w = 4, .box_h = 13, .ofs_x = 2, .ofs_y = 2},
    {.bitmap_index = 64, .adv_w = 128, .box_w = 8, .box_h = 7, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 71, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 77, .adv_w = 128, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 80, .adv_w = 128, .box_w = 6, .box_h = 1, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 81, .adv_w = 128, .box_w = 2, .box_h = 3, .ofs_x = 3, .ofs_y = 3},
    {.bitmap_index = 82, .adv_w = 128, .box_w = 8, .box_h = 14, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 96, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 106, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 115, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 124, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 133, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 143, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 152, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 161, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 170, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 179, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 188, .adv_w = 128, .box_w = 2, .box_h = 10, .ofs_x = 3, .ofs_y = 3},
    {.bitmap_index = 191, .adv_w = 128, .box_w = 4, .box_h = 12, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 197, .adv_w = 128, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 204, .adv_w = 128, .box_w = 6, .box_h = 5, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 208, .adv_w = 128, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 215, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 224, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 234, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 243, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 252, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 261, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 270, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 279, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 288, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 297, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 306, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 315, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 324, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 334, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 343, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 353, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 363, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 372, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 381, .adv_w = 128, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 391, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 400, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 409, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 418, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 427, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 436, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 446, .adv_w = 128, .box_w = 8, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 457, .adv_w = 128, .box_w = 8, .box_h = 11, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 468, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 477, .adv_w = 128, .box_w = 4, .box_h = 13, .ofs_x = 2, .ofs_y = 2},
    {.bitmap_index = 484, .adv_w = 128, .box_w = 8, .box_h = 14, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 498, .adv_w = 128, .box_w = 4, .box_h = 13, .ofs_x = 2, .ofs_y = 2},
    {.bitmap_index = 505, .adv_w = 128, .box_w = 7, .box_h = 6, .ofs_x = 0, .ofs_y = 9},
    {.bitmap_index = 511, .adv_w = 128, .box_w = 8, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 512, .adv_w = 128, .box_w = 4, .box_h = 4, .ofs_x = 3, .ofs_y = 11},
    {.bitmap_index = 514, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 520, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 529, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 535, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 544, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 550, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 559, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 567, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 576, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 585, .adv_w = 128, .box_w = 5, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 594, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 603, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 612, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 619, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 625, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 631, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 639, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 647, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 653, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 659, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 667, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 673, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 679, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 686, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 693, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 701, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 707, .adv_w = 128, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 719, .adv_w = 128, .box_w = 2, .box_h = 14, .ofs_x = 3, .ofs_y = 2},
    {.bitmap_index = 723, .adv_w = 128, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 735, .adv_w = 128, .box_w = 7, .box_h = 3, .ofs_x = 0, .ofs_y = 12},
    {.bitmap_index = 738, .adv_w = 128, .box_w = 6, .box_h = 4, .ofs_x = 1, .ofs_y = 11},
    {.bitmap_index = 741, .adv_w = 128, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 748, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 757, .adv_w = 128, .box_w = 7, .box_h = 1, .ofs_x = 0, .ofs_y = 8},
    {.bitmap_index = 758, .adv_w = 128, .box_w = 4, .box_h = 8, .ofs_x = 2, .ofs_y = 4}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_1[] = {
    0x0, 0x1, 0x5, 0x1f63, 0x1f72
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 176, .range_length = 8051, .glyph_id_start = 96,
        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = 5, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 2,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};

extern const lv_font_t lv_font_montserrat_14;


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/* Pokemon style: all capitals. Lower case letters take the upper case
 * glyphs; the label text itself is unchanged. */
static bool caps_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t next)
{
    // m stays small before B or V, so mB and mV/V keep their meaning.
    bool unit = letter == 'm' && (next == 'B' || next == 'V');
    if (letter >= 'a' && letter <= 'z' && !unit) letter -= 'a' - 'A';
    if (next >= 'a' && next <= 'z') next -= 'a' - 'A';
    return lv_font_get_glyph_dsc_fmt_txt(font, dsc, letter, next);
}

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t ui_font_16 = {
#else
lv_font_t ui_font_16 = {
#endif
    .get_glyph_dsc = caps_glyph_dsc,                   /*Capitals only, as Pokemon Red and Blue*/    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 17,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = &lv_font_montserrat_14,
#endif
    .user_data = NULL,
};



#endif /*#if UI_FONT_16*/

