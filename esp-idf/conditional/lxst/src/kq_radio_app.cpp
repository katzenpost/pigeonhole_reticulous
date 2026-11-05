// SPDX-License-Identifier: AGPL-3.0-only
// The Radio screen, on the lcd task: the joined groups, each off, on the air, or talked to (Enter),
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
lv_obj_t *s_bar = nullptr, *s_left = nullptr;  // while talking: the time left, draining
const uint32_t C_RED = 0xE5484D;
lv_timer_t* s_timer = nullptr;
uint32_t s_seen = 0;
std::string s_shown;  // what the list was drawn from
std::string s_focus;  // the group last toggled, focused again when the list is redrawn

lv_obj_t* label(lv_obj_t* parent, const std::string& t, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_text(l, t.c_str());
    return l;
}

// Enter goes round: off, on the air (listening), talking to it too, off.
void onToggle(lv_event_t* e) {
    auto* g = static_cast<std::string*>(lv_event_get_user_data(e));
    auto on = kqRadioGroups();
    bool air = std::find(on.begin(), on.end(), *g) != on.end();
    if (!air) kqRadioSet(*g, true);
    else if (kqRadioTalk() != *g) kqRadioSetTalk(*g);
    else kqRadioSet(*g, false);
    s_focus = *g;
    s_shown.clear();
    s_seen = 0;  // the list is drawn again on the next tick
}

void onFree(lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); }

void drawList() {
    auto on = kqRadioGroups();
    std::string talk = kqRadioTalk(), key = talk + "\n";
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
        bool talking = air && talk == c.id;
        lv_obj_set_style_text_color(a, lv_color_hex(talking ? C_AMBER : air ? C_ON : C_SUB), 0);
        lv_label_set_text(a, talking ? "ON AIR · TALK" : air ? "ON AIR" : "off");
        auto* id = new std::string(c.id);
        lv_obj_add_event_cb(r, onToggle, LV_EVENT_CLICKED, id);
        lv_obj_add_event_cb(r, onFree, LV_EVENT_DELETE, id);
        if (lcdInputGroup()) {
            lv_group_add_obj(lcdInputGroup(), r);
            if (c.id == s_focus) lv_group_focus_obj(r);
        }
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
    std::string talk = kqRadioTalk();
    std::string st = r.talking ? LV_SYMBOL_AUDIO " Talking to " + talk
                   : !r.playing.empty() ? LV_SYMBOL_VOLUME_MAX " " + r.playing
                   : on ? "Listening to " + std::to_string(on) + (on == 1 ? " group" : " groups")
                        : "Off the air: pick a group below";
    lv_label_set_text(s_status, st.c_str());
    lv_obj_set_style_text_color(s_status, lv_color_hex(r.talking ? C_AMBER : !r.playing.empty() ? C_ON : on ? 0xFFFFFF : C_AMBER), 0);
    if (!r.talking && r.playing.empty() && !talk.empty())
        lv_label_set_text(s_status, ("Hold Mic or Space to talk to " + talk).c_str());
    // The time left to talk, redrawn every tick (4 a second): a bar draining from full, and seconds.
    bool bar = r.talking && r.talk_max_ms;
    if (bar) {
        uint32_t left = r.cut ? 0 : r.talk_left_ms;
        uint32_t color = left > 30000 ? C_ON : left > 10000 ? C_AMBER : C_RED;
        lv_bar_set_value(s_bar, (int32_t)((uint64_t)left * 1000 / r.talk_max_ms), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(color), LV_PART_INDICATOR);
        std::string t = r.cut ? "time is up: sent; let go of the key"
                              : std::to_string((left + 999) / 1000) + " s left of " + std::to_string(r.talk_max_ms / 1000);
        lv_label_set_text(s_left, t.c_str());
        lv_obj_set_style_text_color(s_left, lv_color_hex(r.cut ? C_RED : color), 0);
    }
    for (lv_obj_t* o : {s_bar, s_left}) {
        if (bar) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
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
    s_bar = lv_bar_create(root);
    lv_obj_set_size(s_bar, lv_pct(100), lcdPx(10));
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_RULE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, lcdPx(3), LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, lcdPx(3), LV_PART_INDICATOR);
    s_left = label(root, "", C_ON, kSmall);
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
    s_timer = lv_timer_create(refresh, 250, nullptr);
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
    s_root = s_status = s_counts = s_last = s_list = s_bar = s_left = nullptr;
    s_shown.clear();
}
#endif
