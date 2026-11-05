// SPDX-License-Identifier: AGPL-3.0-only
// The Radio screen, on the lcd task: the joined groups, each on or off the air (Enter toggles),
// what is playing, and how the last burst went. The look follows Group Chat's.
#include "kq_radio_app.h"

#if CONFIG_SPANGAP_LCD
#include <algorithm>
#include <string>
#include <vector>

#include "lcd.h"
#include "kq_view.h"

namespace {

const uint32_t C_BG = 0x101418, C_SUB = 0x9098A0, C_RULE = 0x232B33, C_SEL = 0x1B2A3C, C_ON = 0x36C06A,
               C_AMBER = 0xE8A33A;

const lv_font_t *kFont, *kBold, *kSmall;
lv_obj_t *s_root = nullptr, *s_status = nullptr, *s_counts = nullptr, *s_last = nullptr, *s_list = nullptr;
lv_timer_t* s_timer = nullptr;
uint32_t s_seen = 0;
std::string s_shown;  // what the list was drawn from

lv_obj_t* label(lv_obj_t* parent, const std::string& t, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_text(l, t.c_str());
    return l;
}

void onToggle(lv_event_t* e) {
    auto* g = static_cast<std::string*>(lv_event_get_user_data(e));
    auto on = kqRadioGroups();
    kqRadioSet(*g, std::find(on.begin(), on.end(), *g) == on.end());
    s_shown.clear();
}

void onFree(lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); }

void drawList() {
    auto on = kqRadioGroups();
    std::string key;
    std::vector<KqChat> chats = kqChats();
    for (auto& c : chats) key += c.id + (std::find(on.begin(), on.end(), c.id) != on.end() ? "+" : "-") + "\n";
    if (key == s_shown) return;
    s_shown = key;
    lv_obj_clean(s_list);
    if (chats.empty()) label(s_list, "No groups yet: start or join one in Group Chat.", C_SUB, kSmall);
    for (auto& c : chats) {
        if (!c.joined) continue;
        bool air = std::find(on.begin(), on.end(), c.id) != on.end();
        lv_obj_t* r = lv_button_create(s_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_ver(r, lcdPx(4), 0);
        lv_obj_set_style_pad_hor(r, lcdPx(6), 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(C_RULE), 0);
        for (lv_state_t s : {(lv_state_t)LV_STATE_FOCUSED, (lv_state_t)LV_STATE_PRESSED}) {
            lv_obj_set_style_bg_color(r, lv_color_hex(C_SEL), s);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, s);
        }
        lv_obj_t* n = label(r, c.id, 0xFFFFFF, kFont);
        lv_obj_set_width(n, 1);
        lv_obj_set_flex_grow(n, 1);
        lv_obj_t* a = lv_label_create(r);
        lv_obj_set_style_text_font(a, kBold, 0);
        lv_obj_set_style_text_color(a, lv_color_hex(air ? C_ON : C_SUB), 0);
        lv_label_set_text(a, air ? "ON AIR" : "off");
        auto* id = new std::string(c.id);
        lv_obj_add_event_cb(r, onToggle, LV_EVENT_CLICKED, id);
        lv_obj_add_event_cb(r, onFree, LV_EVENT_DELETE, id);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), r);
    }
}

void refresh(lv_timer_t*) {
    if (!s_root) return;
    if (kqLock() != KqLock::OPEN) {
        lv_label_set_text(s_status, "The chats are locked: open Group Chat to unlock them");
        lv_obj_set_style_text_color(s_status, lv_color_hex(C_AMBER), 0);
        return;
    }
    KqRadio r = kqRadio();
    size_t on = kqRadioGroups().size();
    std::string st = !r.playing.empty() ? LV_SYMBOL_VOLUME_MAX " " + r.playing
                   : on ? "Listening to " + std::to_string(on) + (on == 1 ? " group" : " groups")
                        : "Off the air: pick a group below";
    lv_label_set_text(s_status, st.c_str());
    lv_obj_set_style_text_color(s_status, lv_color_hex(!r.playing.empty() ? C_ON : on ? 0xFFFFFF : C_AMBER), 0);
    std::string counts = "heard " + std::to_string(r.heard) + " · missed " + std::to_string(r.missed);
    if (r.queued) counts += " · " + std::to_string(r.queued) + " waiting";
    lv_label_set_text(s_counts, counts.c_str());
    lv_label_set_text(s_last, r.last.c_str());
    uint32_t g = kqGeneration();
    if (g != s_seen) { s_seen = g; drawList(); }
}

}  // namespace

KqRadioApp::KqRadioApp() : LcdApp({.name = "Radio", .iconBasename = "kq"}) {}

void KqRadioApp::onCreate(lv_obj_t* root) {
    kFont = lcdFont(LcdFace::UI, lcdPx(14));
    kBold = lcdFont(LcdFace::UI_BOLD, lcdPx(12));
    kSmall = lcdFont(LcdFace::UI, lcdPx(11));
    s_root = root;
    lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, lcdPx(6), 0);
    lv_obj_set_style_pad_row(root, lcdPx(3), 0);
    label(root, "Radio", 0xFFFFFF, lcdFont(LcdFace::UI_BOLD, lcdPx(14)));
    s_status = label(root, "", 0xFFFFFF, kFont);
    s_counts = label(root, "", C_SUB, kSmall);
    s_last = label(root, "", C_SUB, kSmall);
    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    s_shown.clear();
    s_seen = 0;
    refresh(nullptr);
    s_timer = lv_timer_create(refresh, 500, nullptr);
}

void KqRadioApp::onShow() {
    if (s_timer) lv_timer_resume(s_timer);
    s_seen = 0;
}

void KqRadioApp::onHide() {
    if (s_timer) lv_timer_pause(s_timer);
}

void KqRadioApp::onClose() {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    s_root = s_status = s_counts = s_last = s_list = nullptr;
    s_shown.clear();
}
#endif
