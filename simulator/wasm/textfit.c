/* Text fit check: walks the screen and the top layer and reports
 * every label whose text does not fit its box, is cut by a card or button
 * it sits in, or comes within GAP px of the label or object beside it.
 * Used by tests/text-fit.js through the simulator's sim_textfit,
 * sim_clickables and sim_popup_text. Not part of the board's firmware. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "textfit.h"

static char *out;
static size_t out_n, out_len;

static void put(const char *fmt, ...)
{
    if (out_len + 1 >= out_n) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + out_len, out_n - out_len, fmt, ap);
    va_end(ap);
    if (k > 0) out_len += (size_t)k < out_n - out_len ? (size_t)k : out_n - out_len - 1;
}

static bool hidden(const lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return true;
    return false;
}

static bool is_label(const lv_obj_t *o) { return lv_obj_check_type(o, &lv_label_class); }

// The text as one line for the report: newlines as " / ", at most 48 bytes.
static void short_text(char *b, size_t n, const char *t)
{
    size_t k = 0;
    for (; *t && k + 4 < n; t++) {
        if (*t == '\n') { b[k++] = ' '; b[k++] = '/'; b[k++] = ' '; }
        else b[k++] = *t;
        if (k > 48) { b[k++] = '.'; b[k++] = '.'; break; }
    }
    b[k] = 0;
}

// Where the label's text is drawn, in screen coordinates.
static void text_box(lv_obj_t *l, lv_area_t *tb, lv_point_t *full, lv_point_t *wrapped)
{
    const char *t = lv_label_get_text(l);
    const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    int32_t ls = lv_obj_get_style_text_letter_space(l, LV_PART_MAIN);
    int32_t lns = lv_obj_get_style_text_line_space(l, LV_PART_MAIN);
    lv_area_t ca;
    lv_obj_get_content_coords(l, &ca);
    int32_t cw = lv_area_get_width(&ca);
    bool fit = lv_obj_get_style_width(l, LV_PART_MAIN) == LV_SIZE_CONTENT;
    lv_text_get_size(full, t, f, ls, lns, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(wrapped, t, f, ls, lns, fit ? LV_COORD_MAX : cw, LV_TEXT_FLAG_NONE);
    lv_text_align_t a = lv_obj_get_style_text_align(l, LV_PART_MAIN);
    int32_t w = wrapped->x;
    tb->x1 = a == LV_TEXT_ALIGN_CENTER ? ca.x1 + (cw - w) / 2 : a == LV_TEXT_ALIGN_RIGHT ? ca.x2 + 1 - w : ca.x1;
    tb->x2 = tb->x1 + w - 1;
    tb->y1 = ca.y1;
    tb->y2 = ca.y1 + wrapped->y - 1;
    // Only a wrapping label grows to its text; the others draw inside their box.
    if (lv_label_get_long_mode(l) != LV_LABEL_LONG_MODE_WRAP && !fit) {
        if (tb->x1 < ca.x1) tb->x1 = ca.x1;
        if (tb->x2 > ca.x2) tb->x2 = ca.x2;
        if (tb->y2 > ca.y2) tb->y2 = ca.y2;
    }
}

static const char *kind(const lv_obj_t *o)
{
    if (is_label(o)) return "label";
    if (lv_obj_check_type(o, &lv_switch_class)) return "switch";
    if (lv_obj_check_type(o, &lv_buttonmatrix_class)) return "keypad";
    if (lv_obj_check_type(o, &lv_image_class)) return "icon";
    return "object";
}

static bool overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static int issues;
#define GAP 3   // px a text keeps from the side of the label or object beside it
bool textfit_all;   // list every label too (for a look by hand)

static void check_label(lv_obj_t *l, const char *where)
{
    const char *t = lv_label_get_text(l);
    if (!t || !*t || hidden(l)) return;
    lv_area_t tb, ca;
    lv_point_t full, wr;
    text_box(l, &tb, &full, &wr);
    lv_obj_get_content_coords(l, &ca);
    int32_t cw = lv_area_get_width(&ca), ch = lv_area_get_height(&ca);
    char s[64];
    short_text(s, sizeof s, t);
    lv_label_long_mode_t m = lv_label_get_long_mode(l);
    bool fit = lv_obj_get_style_width(l, LV_PART_MAIN) == LV_SIZE_CONTENT;
    if (textfit_all)
        put("%s | \"%s\" | text %dx%d, box %dx%d at %d,%d%s%s\n", where, s, (int)full.x, (int)full.y, (int)cw, (int)ch,
            (int)ca.x1, (int)ca.y1, fit ? " (sized to text)" : "", m == LV_LABEL_LONG_MODE_WRAP ? " (wraps)" : "");
    if (!fit && m != LV_LABEL_LONG_MODE_WRAP && full.x > cw) {
        put("%s | \"%s\" | %d px too wide for its %d px box (x %d)\n", where, s, (int)(full.x - cw), (int)cw, (int)ca.x1);
        issues++;
    } else if (wr.y > ch + 1) {
        put("%s | \"%s\" | wraps to %d px in a %d px tall box\n", where, s, (int)wr.y, (int)ch);
        issues++;
    }
    // Cut by a parent: children are drawn inside each parent's area unless
    // it lets them overflow; a scrolling parent may hold more than it shows.
    // Past a scrolling parent, what it scrolled out of view is not cut by
    // the parents above it (a help text scrolled past the page top).
    bool scrolled = false;
    for (lv_obj_t *p = lv_obj_get_parent(l); p && lv_obj_get_parent(p); p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) continue;
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        // Only lists scroll, and only up and down: text past a side is always cut.
        bool scroll_y = lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(p) & LV_DIR_VER);
        int32_t r = tb.x2 - pa.x2, le = pa.x1 - tb.x1, b = tb.y2 - pa.y2, tp = pa.y1 - tb.y1;
        if (scroll_y) scrolled = true;
        if (r > 0 || le > 0 || (!scrolled && (b > 0 || tp > 0))) {
            put("%s | \"%s\" | cut by its %s at %d,%d %dx%d (right %d, left %d, bottom %d, top %d)\n", where, s, kind(p),
                (int)pa.x1, (int)pa.y1, (int)lv_area_get_width(&pa), (int)lv_area_get_height(&pa),
                (int)r, (int)le, (int)b, (int)tp);
            issues++;
            break;
        }
    }
    // Runs into a neighbour: another child of the same parent.
    lv_obj_t *par = lv_obj_get_parent(l);
    uint32_t n = lv_obj_get_child_count(par);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *o = lv_obj_get_child(par, (int32_t)i);
        if (o == l || hidden(o)) continue;
        lv_area_t ob;
        lv_obj_get_coords(o, &ob);
        if (is_label(o)) {
            const char *u = lv_label_get_text(o);
            if (!u || !*u) continue;
            lv_point_t f2, w2;
            text_box(o, &ob, &f2, &w2);
            if (o < l) continue;   // each pair once
        } else {
            // A plain box with nothing drawn (a layout holder) is not a neighbour.
            if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP &&
                lv_obj_get_style_border_width(o, LV_PART_MAIN) == 0 && lv_obj_get_child_count(o) == 0 &&
                !lv_obj_check_type(o, &lv_image_class))
                continue;
        }
        // Touching counts: under GAP px from the side of a neighbour reads as run together.
        lv_area_t near = { tb.x1 - GAP, tb.y1, tb.x2 + GAP, tb.y2 };
        if (overlaps(&near, &ob)) {
            char s2[64] = "";
            if (is_label(o)) short_text(s2, sizeof s2, lv_label_get_text(o));
            int32_t gap = ob.x1 > tb.x2 ? ob.x1 - tb.x2 - 1 : tb.x1 > ob.x2 ? tb.x1 - ob.x2 - 1 : -1;
            if (gap >= 0)
                put("%s | \"%s\" | %d px from %s \"%s\" at %d,%d %dx%d\n", where, s, (int)gap, kind(o), s2,
                    (int)ob.x1, (int)ob.y1, (int)lv_area_get_width(&ob), (int)lv_area_get_height(&ob));
            else
                put("%s | \"%s\" | runs into %s \"%s\" at %d,%d %dx%d\n", where, s, kind(o), s2,
                    (int)ob.x1, (int)ob.y1, (int)lv_area_get_width(&ob), (int)lv_area_get_height(&ob));
            issues++;
        }
    }
}

static void walk(lv_obj_t *o, const char *where)
{
    if (hidden(o)) return;
    if (is_label(o)) check_label(o, where);
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) walk(lv_obj_get_child(o, (int32_t)i), where);
}

int textfit_check(char *buf, size_t n, const char *where)
{
    out = buf; out_n = n; out_len = 0; issues = 0;
    if (n) buf[0] = 0;
    walk(lv_screen_active(), where);
    walk(lv_layer_top(), where);
    return issues;
}

static bool is_inside(lv_obj_t *o, lv_obj_t *anc)
{
    for (; o; o = lv_obj_get_parent(o))
        if (o == anc) return true;
    return false;
}

static void walk_click(lv_obj_t *o)
{
    if (hidden(o)) return;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_event_count(o) > 0) {
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        lv_point_t p = { (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 };
        if (p.x >= 0 && p.x < 480 && p.y >= 0 && p.y < 272) {
            lv_obj_t *hit = lv_indev_search_obj(lv_layer_top(), &p);
            if (!hit) hit = lv_indev_search_obj(lv_screen_active(), &p);
            if (hit && is_inside(hit, o))
                put("%d,%d,%d,%d;", (int)p.x, (int)p.y, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
        }
    }
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) walk_click(lv_obj_get_child(o, (int32_t)i));
}

void textfit_clickables(char *buf, size_t n)
{
    out = buf; out_n = n; out_len = 0;
    if (n) buf[0] = 0;
    walk_click(lv_layer_top());
    walk_click(lv_screen_active());
}

static const char *first_text(lv_obj_t *o)
{
    if (hidden(o)) return NULL;
    if (is_label(o)) {
        const char *t = lv_label_get_text(o);
        if (t && *t) return t;
    }
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        const char *t = first_text(lv_obj_get_child(o, (int32_t)i));
        if (t) return t;
    }
    return NULL;
}

void textfit_popup_text(char *buf, size_t n)
{
    // The popup is the screen's last child, 480 x 232 under the header (ui.c overlay_new).
    lv_obj_t *scr = lv_screen_active();
    uint32_t k = lv_obj_get_child_count(scr);
    lv_obj_t *o = k ? lv_obj_get_child(scr, (int32_t)k - 1) : NULL;
    const char *t = o && lv_obj_get_width(o) == 480 && lv_obj_get_height(o) == 232 ? first_text(o) : NULL;
    lv_strlcpy(buf, t ? t : "", n);
}
