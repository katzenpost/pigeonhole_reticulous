// SPDX-License-Identifier: AGPL-3.0-only
// The look Group Chat and Radio share: one palette, one header bar, one list row, so the two
// read as siblings. A colour means one thing in both: green listening or ok, red transmitting,
// amber attention, blue focus and "mine".
#pragma once
#include <cstring>
#include <string>

#include "lcd.h"

namespace kqui {

constexpr uint32_t C_BG = 0x101418, C_SUB = 0xA8B0B8, C_ACCENT = 0x2563A0, C_AMBER = 0xE8A33A,
                   C_RULE = 0x232B33, C_OK = 0x36C06A, C_SEL = 0x2D4566, C_BAR = 0x0C1014, C_FIELD = 0x2F3B47,
                   C_NOTE = 0x8FA3B6, C_ACT = 0xCFD8E3, C_DIALOG = 0x20262E, C_ME = 0x7FA6FF, C_TX = 0x7A1E22;
constexpr int PX_BODY = 14, PX_SMALL = 12, PX_HEADER = 26;

// The bar across the top: a row PX_HEADER high with a rule under it.
inline lv_obj_t* headerBar(lv_obj_t* parent) {
    lv_obj_t* h = lv_obj_create(parent);
    lv_obj_remove_style_all(h);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(h, lv_pct(100), lcdPx(PX_HEADER));
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, lcdPx(6), 0);
    lv_obj_set_style_pad_hor(h, lcdPx(6), 0);
    lv_obj_set_style_border_side(h, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(h, 1, 0);
    lv_obj_set_style_border_color(h, lv_color_hex(C_RULE), 0);
    return h;
}

// A list row's look: a rule under it; focused or pressed, a fill and a bar down its left edge.
// Shared styles, made once (a UI zoom change restarts the device).
inline void styleRow(lv_obj_t* r) {
    static lv_style_t row, sel;
    static bool made = false;
    if (!made) {
        made = true;
        lv_style_init(&row);
        lv_style_set_pad_ver(&row, lcdPx(4));
        lv_style_set_pad_hor(&row, lcdPx(8));
        lv_style_set_border_side(&row, LV_BORDER_SIDE_BOTTOM);
        lv_style_set_border_width(&row, 1);
        lv_style_set_border_color(&row, lv_color_hex(C_RULE));
        lv_style_init(&sel);
        lv_style_set_bg_color(&sel, lv_color_hex(C_SEL));
        lv_style_set_bg_opa(&sel, LV_OPA_COVER);
        lv_style_set_border_side(&sel, LV_BORDER_SIDE_LEFT);
        lv_style_set_border_width(&sel, lcdPx(3));
        lv_style_set_border_color(&sel, lv_color_hex(C_ME));
        // The bar takes the place of the rule and of some left padding: the text does not move.
        lv_style_set_pad_left(&sel, lcdPx(8) - lcdPx(3));
        lv_style_set_pad_bottom(&sel, lcdPx(4) + 1);
    }
    lv_obj_add_style(r, &row, 0);
    lv_obj_add_style(r, &sel, LV_STATE_FOCUSED);
    lv_obj_add_style(r, &sel, LV_STATE_PRESSED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
}

// Set only when it changed: setting a label or a colour redraws it even when it is the same.
inline void setText(lv_obj_t* l, const std::string& t) {
    if (strcmp(lv_label_get_text(l), t.c_str()) != 0) lv_label_set_text(l, t.c_str());
}
inline void setTextColor(lv_obj_t* o, uint32_t c) {
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), lv_color_hex(c))) lv_obj_set_style_text_color(o, lv_color_hex(c), 0);
}
inline void setBgColor(lv_obj_t* o, uint32_t c, lv_part_t part = LV_PART_MAIN) {
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, part), lv_color_hex(c))) lv_obj_set_style_bg_color(o, lv_color_hex(c), part);
}

}  // namespace kqui
