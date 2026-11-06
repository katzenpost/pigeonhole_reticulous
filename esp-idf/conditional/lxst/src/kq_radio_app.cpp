// SPDX-License-Identifier: AGPL-3.0-only
// The Radio screen, on the lcd task: the joined groups, each off, listened to, or talked to too
// (Enter), and a panel of fixed height for what is happening: talking (a red band), playing, a ping, or
// how the last burst went. P pings the talk group. The look is Group Chat's (kq_theme.h).
#include "kq_radio_app.h"

#if CONFIG_SPANGAP_LCD
#include <algorithm>
#include <string>
#include <vector>

#include "lcd.h"
#include "kq_theme.h"
#include "kq_view.h"

namespace {

using namespace kqui;

const lv_font_t *kFont, *kBold, *kSmall, *kSmallBold;
lv_obj_t *s_root = nullptr, *s_counts = nullptr, *s_list = nullptr;
// The status panel: what is happening, then the time left (a bar draining) or what to do next.
lv_obj_t *s_panel = nullptr, *s_status = nullptr, *s_bar = nullptr, *s_left = nullptr;
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

// P pings the talk group, whichever row has focus.
void onKey(lv_event_t* e) {
    uint32_t k = lv_event_get_key(e);
    if (k == 'p' || k == 'P') kqRadioPing();
}

void drawList() {
    auto on = kqRadioGroups();
    std::string talk = kqRadioTalk(), key = talk + "\n";
    std::vector<KqChat> chats = kqChats();
    for (auto& c : chats) key += c.id + (std::find(on.begin(), on.end(), c.id) != on.end() ? "+" : "-") + "\n";
    if (key == s_shown) return;
    s_shown = key;
    lv_obj_clean(s_list);
    bool any = false;
    for (auto& c : chats) {
        if (!c.joined) continue;
        any = true;
        bool air = std::find(on.begin(), on.end(), c.id) != on.end();
        lv_obj_t* r = lv_button_create(s_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(r, lcdPx(6), 0);
        styleRow(r);
        lv_obj_t* n = label(r, c.id, 0xFFFFFF, kFont);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(n, 1);
        lv_obj_set_flex_grow(n, 1);
        lv_obj_t* a = lv_label_create(r);
        lv_obj_set_style_text_font(a, kSmallBold, 0);
        lv_obj_set_style_text_color(a, lv_color_hex(air ? C_OK : C_SUB), 0);
        lv_label_set_text(a, !air ? "Off" : talk == c.id ? "Listening, talk here" : "Listening");
        auto* id = new std::string(c.id);
        lv_obj_add_event_cb(r, onToggle, LV_EVENT_CLICKED, id);
        lv_obj_add_event_cb(r, onFree, LV_EVENT_DELETE, id);
        lv_obj_add_event_cb(r, onKey, LV_EVENT_KEY, nullptr);
        if (lcdInputGroup()) {
            lv_group_add_obj(lcdInputGroup(), r);
            if (c.id == s_focus) lv_group_focus_obj(r);
        }
    }
    if (!any) label(s_list, "No groups yet: start or join one in Group Chat.", C_SUB, kSmall);
}

void refresh(lv_timer_t*) {
    if (!s_root) return;
    if (kqLock() != KqLock::OPEN) {
        setText(s_status, "The chats are locked");
        setTextColor(s_status, C_AMBER);
        setText(s_left, "Open Group Chat to unlock them.");
        return;
    }
    KqRadio r = kqRadio();
    size_t on = kqRadioGroups().size();
    std::string talk = kqRadioTalk();
    std::string st = r.talking ? "\xE2\x97\x8F Talking to " + talk
                   : !r.playing.empty() ? LV_SYMBOL_VOLUME_MAX " " + r.playing
                   : on ? "Listening to " + std::to_string(on) + (on == 1 ? " group" : " groups")
                        : "Off the air";
    setText(s_status, st);
    setTextColor(s_status, r.talking ? 0xFFFFFF : on || !r.playing.empty() ? C_OK : C_SUB);
    // Transmitting turns the whole panel into a red band, for as long as the key is held.
    setBgColor(s_panel, r.talking ? C_TX : C_BAR);
    // The time left to talk, redrawn every tick (4 a second): a bar draining from full, and seconds.
    bool bar = r.talking && r.talk_max_ms;
    std::string t;
    uint32_t tc = C_SUB;
    if (bar) {
        uint32_t left = r.cut ? 0 : r.talk_left_ms;
        tc = left > 30000 ? 0xFFFFFF : C_AMBER;
        lv_bar_set_value(s_bar, (int32_t)((uint64_t)left * 1000 / r.talk_max_ms), LV_ANIM_OFF);
        setBgColor(s_bar, tc, LV_PART_INDICATOR);
        t = r.cut ? "Time is up: sent. Let go of the key."
                  : std::to_string((left + 999) / 1000) + " s left of " + std::to_string(r.talk_max_ms / 1000);
    } else if (r.talking) {
        tc = 0xFFFFFF;
        t = "Let go of Mic to send.";
    } else if (!r.ping.empty()) {
        tc = 0xFFFFFF;
        t = r.ping;
    } else {
        t = !r.last.empty() ? r.last : !talk.empty() ? "Hold Mic to talk to " + talk
            : on ? "Choose a group to talk to it too." : "Choose a group to listen to it.";
    }
    if (bar) lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    setText(s_left, t);
    setTextColor(s_left, tc);
    // The counts only once there is something to count.
    std::string counts = r.heard || r.missed ? "heard " + std::to_string(r.heard) + " \xC2\xB7 missed " + std::to_string(r.missed) : "";
    if (r.queued) counts += (counts.empty() ? "" : " \xC2\xB7 ") + std::to_string(r.queued) + " waiting";
    setText(s_counts, counts);
    uint32_t g = kqGeneration();
    if (g != s_seen) { s_seen = g; drawList(); }
}

}  // namespace

KqRadioApp::KqRadioApp() : LcdApp({.name = "Radio", .iconBasename = "kq"}) {}

void KqRadioApp::onCreate(lv_obj_t* root) {
    kFont = lcdFont(LcdFace::UI, lcdPx(PX_BODY));
    kBold = lcdFont(LcdFace::UI_BOLD, lcdPx(PX_BODY));
    kSmall = lcdFont(LcdFace::UI, lcdPx(PX_SMALL));
    kSmallBold = lcdFont(LcdFace::UI_BOLD, lcdPx(PX_SMALL));
    s_root = root;
    lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    // The header, as Group Chat's: the title, and on the right the counts.
    lv_obj_t* hdr = headerBar(root);
    lv_obj_t* title = label(hdr, "Radio", 0xFFFFFF, kBold);
    lv_obj_set_width(title, 1);
    lv_obj_set_flex_grow(title, 1);
    s_counts = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_counts, kSmall, 0);
    lv_obj_set_style_text_color(s_counts, lv_color_hex(C_SUB), 0);
    // The panel keeps its height in every state, so the rows below never move.
    s_panel = lv_obj_create(root);
    lv_obj_remove_style_all(s_panel);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_panel, lv_pct(100), lcdPx(54));
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(s_panel, lcdPx(8), 0);
    lv_obj_set_style_pad_row(s_panel, lcdPx(4), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    s_status = label(s_panel, "", 0xFFFFFF, kBold);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);
    s_bar = lv_bar_create(s_panel);
    lv_obj_set_size(s_bar, lv_pct(100), lcdPx(6));
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_BAR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, lcdPx(3), LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, lcdPx(3), LV_PART_INDICATOR);
    s_left = label(s_panel, "", C_SUB, kSmall);
    lv_label_set_long_mode(s_left, LV_LABEL_LONG_MODE_DOTS);
    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_list, lcdPx(6), 0);
    lv_obj_set_style_pad_top(s_list, lcdPx(4), 0);
    // What the keys do, in the place Group Chat keeps its buttons.
    lv_obj_t* hint = lv_obj_create(root);
    lv_obj_remove_style_all(hint);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(hint, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(hint, lcdPx(8), 0);
    lv_obj_set_style_pad_ver(hint, lcdPx(6), 0);
    lv_obj_set_style_border_side(hint, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(hint, 1, 0);
    lv_obj_set_style_border_color(hint, lv_color_hex(C_RULE), 0);
    label(hint, "Enter: off \xE2\x86\x92 listen \xE2\x86\x92 talk here    Mic: hold to talk    P: ping", C_SUB, kSmall);
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
    s_root = s_status = s_counts = s_list = s_bar = s_left = s_panel = nullptr;
    s_shown.clear();
}
#endif
