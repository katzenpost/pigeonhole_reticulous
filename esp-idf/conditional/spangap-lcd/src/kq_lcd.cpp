// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 group chat on the screen. Everything runs on the lcd task: the screens read the
// chats through kq_view.h (a copy taken under the engine's lock) and send commands back through
// it; a timer redraws whatever is shown when the engine has taken a step. The look follows rrc's.
#include "kq_app.h"

#include <cstdio>
#include <string>
#include <vector>

#include "lcd.h"
#include "lcd_input_box.h"
#include "kq_view.h"

namespace {

const uint32_t C_BG = 0x101418, C_SUB = 0x9098A0, C_ACCENT = 0x2563A0, C_AMBER = 0xE8A33A, C_RULE = 0x232B33,
               C_OK = 0x36C06A, C_SEL = 0x1B2A3C, C_BAR = 0x0C1014, C_FIELD = 0x2F3B47, C_NOTE = 0x8FA3B6,
               C_ACT = 0xCFD8E3, C_DIALOG = 0x20262E, C_ME = 0x7FA6FF;

const lv_font_t *kFont, *kBold, *kItalic, *kSmall, *kBtn, *kBig;
void refreshFont() {
    kFont = lcdFont(LcdFace::UI, lcdPx(14));
    kBold = lcdFont(LcdFace::UI_BOLD, lcdPx(14));
    kItalic = lcdFont(LcdFace::UI_ITALIC, lcdPx(13));
    kSmall = lcdFont(LcdFace::UI, lcdPx(11));
    kBtn = lcdFont(LcdFace::UI_BOLD, lcdPx(12));
    kBig = lcdFont(LcdFace::MONO_BOLD, lcdPx(15));
}

// ---- widgets ----

lv_obj_t* s_layer = nullptr;

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt.c_str());
    return l;
}

lv_obj_t* mkWrap(lv_obj_t* parent, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(parent, txt, color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

lv_obj_t* mkBox(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t* mkRowBox(lv_obj_t* parent, int gap) {
    lv_obj_t* o = mkBox(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

lv_obj_t* mkScreen() {
    lv_obj_t* o = lv_obj_create(s_layer);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(o, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* mkBody(lv_obj_t* screen) {
    lv_obj_t* b = lv_obj_create(screen);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(b, lcdPx(6), 0);
    lv_obj_set_style_pad_ver(b, lcdPx(4), 0);
    lv_obj_set_style_pad_row(b, lcdPx(3), 0);
    return b;
}

lv_obj_t* mkActionBar(lv_obj_t* screen) {
    lv_obj_t* bar = mkRowBox(screen, lcdPx(6));
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, lcdPx(8), 0);
    lv_obj_set_style_pad_ver(bar, lcdPx(5), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_RULE), 0);
    return bar;
}

void focusRing(lv_obj_t* o) {
    lv_obj_set_style_outline_width(o, lcdPx(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(o, lv_color_hex(C_ME), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(o, 1, LV_STATE_FOCUS_KEY);
}

lv_obj_t* mkButton(lv_obj_t* parent, const std::string& text, bool primary, lv_event_cb_t cb) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(b, lcdPx(4), 0);
    lv_obj_set_style_pad_hor(b, lcdPx(10), 0);
    if (primary) {
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_FIELD), 0);
    }
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? 0x1D4F80 : C_SEL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    focusRing(b);
    mkLabel(b, text, primary ? 0xFFFFFF : C_ACT, kBtn);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

lv_obj_t* mkRow(lv_obj_t* parent) {
    lv_obj_t* r = lv_button_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(r, lcdPx(3), 0);
    lv_obj_set_style_pad_hor(r, lcdPx(6), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(C_RULE), 0);
    for (lv_state_t s : {(lv_state_t)LV_STATE_FOCUSED, (lv_state_t)LV_STATE_PRESSED}) {
        lv_obj_set_style_bg_color(r, lv_color_hex(C_SEL), s);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, s);
    }
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), r);
    return r;
}

struct Header {
    lv_obj_t* bar = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* right = nullptr;
};

// The header bar: a back chevron when there is somewhere to go back to, the title, a note.
Header mkHeader(lv_obj_t* screen, lv_event_cb_t onBack) {
    Header h;
    h.bar = mkRowBox(screen, lcdPx(6));
    lv_obj_set_height(h.bar, lcdPx(26));
    lv_obj_set_style_pad_hor(h.bar, lcdPx(6), 0);
    lv_obj_set_style_border_side(h.bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(h.bar, 1, 0);
    lv_obj_set_style_border_color(h.bar, lv_color_hex(C_RULE), 0);
    if (onBack) {
        lv_obj_t* back = mkLabel(h.bar, LV_SYMBOL_LEFT, C_ACT, kFont);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(back, lcdPx(12));
        lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, nullptr);
        focusRing(back);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), back);
    }
    h.title = mkLabel(h.bar, "", 0xFFFFFF, kBold);
    lv_label_set_long_mode(h.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(h.title, 1);
    lv_obj_set_flex_grow(h.title, 1);
    h.right = mkLabel(h.bar, "", C_SUB, kSmall);
    return h;
}

void onFreeString(lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); }
void bindString(lv_obj_t* o, lv_event_cb_t cb, const std::string& s) {
    auto* p = new std::string(s);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, p);
    lv_obj_add_event_cb(o, onFreeString, LV_EVENT_DELETE, p);
}
const std::string& boundString(lv_event_t* e) { return *static_cast<std::string*>(lv_event_get_user_data(e)); }

// Focus is placed from a one-shot timer, after a tap's own focus change has settled; a text
// field gets it only where the board has keys.
lv_obj_t* g_focusTarget = nullptr;
void focusTimerCb(lv_timer_t*) {
    if (g_focusTarget && lv_obj_is_valid(g_focusTarget) && lcdInputGroup()) lv_group_focus_obj(g_focusTarget);
    g_focusTarget = nullptr;
}
void deferFocus(lv_obj_t* o) {
    if (!o) return;
    if (lcdKeyboardOnScreen() && lv_obj_check_type(o, &lv_textarea_class)) return;
    g_focusTarget = o;
    lv_timer_t* t = lv_timer_create(focusTimerCb, 40, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

void styleField(lv_obj_t* f, const char* placeholder) {
    lv_obj_set_style_text_font(f, kFont, 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(f, lv_color_white(), 0);
    lv_obj_set_style_text_color(f, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, lv_color_hex(C_FIELD), 0);
    lv_obj_set_style_radius(f, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(f, lcdPx(2), 0);
    lv_obj_set_style_pad_hor(f, lcdPx(6), 0);
    lv_textarea_set_placeholder_text(f, placeholder);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), f);
}

lv_obj_t* mkField(lv_obj_t* parent, const char* placeholder, int maxLines) {
    lv_obj_t* f = lcdInputBoxCreate(parent, 1, maxLines);
    styleField(f, placeholder);
    return f;
}

// One line taken as typed: the input box would capitalize a voucher code's first letter.
lv_obj_t* mkPlainField(lv_obj_t* parent, const char* placeholder) {
    lv_obj_t* f = lv_textarea_create(parent);
    lv_textarea_set_one_line(f, true);
    styleField(f, placeholder);
    lcdKeyboardAttach(f);
    return f;
}

// ---- state ----

enum Screen { SC_LIST, SC_CHAT };
Screen g_screen = SC_LIST;
std::string g_chat;          // the group on the chat screen
uint32_t g_seen = 0;         // the engine generation last drawn
lv_timer_t* g_timer = nullptr;

lv_obj_t *s_list = nullptr, *s_listBody = nullptr, *s_chatScr = nullptr, *s_log = nullptr, *s_compose = nullptr,
         *s_voucher = nullptr, *s_dialog = nullptr, *s_dialogField = nullptr;
Header g_listHdr, g_chatHdr;

std::string statusOf(const KqChat& c) {
    if (!c.voucher.empty()) return c.voucher == "\xE2\x80\xA6" ? "making your voucher\xE2\x80\xA6" : "waiting to be added";
    if (!c.joined) return "joining\xE2\x80\xA6";
    std::string s = std::to_string(c.members) + (c.members == 1 ? " other member" : " other members");
    if (c.adding) s += " \xC2\xB7 adding one";
    if (c.pending) s += " \xC2\xB7 sending";
    return s;
}

void openChat(const std::string& id);
void showScreen(Screen sc);

// ---- the dialog: one line to type and OK ----

void (*g_dialogOk)(const std::string&) = nullptr;

void closeDialog() {
    if (!s_dialog) return;
    lv_obj_t* o = s_dialog;
    s_dialog = s_dialogField = nullptr;
    lcdModalUntrack(o);
    lv_obj_delete(o);
    // The release of the key that closed it lands on whatever has focus now, so not on
    // whatever the group falls back to.
    lv_obj_t* f = g_screen == SC_CHAT ? s_compose : s_listBody ? lv_obj_get_child(s_listBody, 0) : nullptr;
    if (f && lv_obj_get_group(f)) lv_group_focus_obj(f);
}

void dialogSubmit(lv_event_t*) {
    if (!s_dialogField) return;
    std::string v = lv_textarea_get_text(s_dialogField);
    v.erase(0, v.find_first_not_of(" "));
    v.erase(v.find_last_not_of(" ") + 1);
    if (v.empty()) return;
    auto ok = g_dialogOk;
    lcdKeyboardClose();
    if (ok) ok(v);  // first, so the focus closeDialog gives is on the screen ok shows
    closeDialog();
}

void showDialog(const std::string& title, const std::string& hint, const char* placeholder, const char* verb,
                void (*ok)(const std::string&)) {
    closeDialog();
    g_dialogOk = ok;
    s_dialog = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_dialog);
    lv_obj_set_size(s_dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_50, 0);
    lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_dialog);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(90));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, lcdPx(14));
    lv_obj_set_style_bg_color(box, lv_color_hex(C_DIALOG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, lcdPx(6), 0);
    lv_obj_set_style_pad_all(box, lcdPx(10), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, lcdPx(6), 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    mkLabel(box, title, 0xFFFFFF, kBold);
    if (!hint.empty()) mkWrap(box, hint, C_SUB, kSmall);
    s_dialogField = mkPlainField(box, placeholder);
    lv_obj_set_width(s_dialogField, lv_pct(100));
    lv_obj_add_event_cb(s_dialogField, dialogSubmit, LV_EVENT_READY, nullptr);

    lv_obj_t* btns = mkRowBox(box, lcdPx(8));
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mkButton(btns, "Cancel", false, [](lv_event_t*) { lcdKeyboardClose(); closeDialog(); });
    mkButton(btns, verb, true, dialogSubmit);

    lcdModalTrack(s_dialog, [](void*) { closeDialog(); });
    if (lcdKeyboardOnScreen()) lcdKeyboardOpen(s_dialogField);
    else deferFocus(s_dialogField);
}

// ---- the groups ----

void onChatRow(lv_event_t* e) { openChat(boundString(e)); }

void drawList() {
    if (!s_listBody) return;
    lv_obj_clean(s_listBody);
    auto chats = kqChats();
    lv_label_set_text(g_listHdr.title, "Group chat");
    lv_label_set_text_fmt(g_listHdr.right, "%u", (unsigned)chats.size());
    if (chats.empty()) {
        mkWrap(s_listBody,
               "No groups yet. New group starts one; Join makes a voucher code to give to someone "
               "already in a group.",
               C_SUB, kFont);
        return;
    }
    for (auto& c : chats) {
        lv_obj_t* r = mkRow(s_listBody);
        lv_obj_t* t = mkLabel(r, c.id, 0xFFFFFF, kBold);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(t, lv_pct(100));
        mkLabel(r, statusOf(c), !c.voucher.empty() ? C_AMBER : C_SUB, kSmall);
        bindString(r, onChatRow, c.id);
    }
}

void onNewGroup(lv_event_t*) {
    showDialog("New group", "", "the group's name", "Create",
               [](const std::string& name) { kqCommand("create", name); openChat(name); });
}

void onJoinGroup(lv_event_t*) {
    showDialog("Join a group", "Name it as you like; a member adds you with the voucher code this makes.",
               "the group's name", "Join", [](const std::string& name) { kqCommand("join", name); openChat(name); });
}

void buildList() {
    s_list = mkScreen();
    g_listHdr = mkHeader(s_list, nullptr);
    s_listBody = mkBody(s_list);
    lv_obj_t* bar = mkActionBar(s_list);
    mkButton(bar, "Join", false, onJoinGroup);
    mkButton(bar, "New group", true, onNewGroup);
}

// ---- a group ----

const char* markOf(KqLine::Mark m) {
    switch (m) {
        case KqLine::PENDING: return " \xE2\x80\xA6";
        case KqLine::WRITTEN: return " " LV_SYMBOL_OK;
        case KqLine::READ: return " " LV_SYMBOL_OK LV_SYMBOL_OK;
        default: return "";
    }
}

bool atBottom() { return !s_log || lv_obj_get_scroll_bottom(s_log) <= 4; }

// The voucher, in large mono type, while it waits to be given out.
void drawVoucher(const KqChat& c) {
    if (!s_voucher) return;
    lv_obj_clean(s_voucher);
    if (c.voucher.empty()) { lv_obj_add_flag(s_voucher, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_voucher, LV_OBJ_FLAG_HIDDEN);
    if (c.voucher == "\xE2\x80\xA6") {
        mkWrap(s_voucher, "Making your voucher. This takes a while on this device.", C_AMBER, kSmall);
        return;
    }
    mkWrap(s_voucher, "Your voucher code: a member of the group types it into Add member.", C_AMBER, kSmall);
    mkWrap(s_voucher, c.voucher, 0xFFFFFF, kBig);
}

void drawChat(bool force) {
    if (!s_log || g_chat.empty()) return;
    auto chats = kqChats(g_chat, true);
    lv_label_set_text(g_chatHdr.title, g_chat.c_str());
    if (chats.empty()) { lv_label_set_text(g_chatHdr.right, ""); return; }
    const KqChat& c = chats.front();
    lv_label_set_text(g_chatHdr.right, c.joined ? std::to_string(c.members + 1).append(" in").c_str() : "");
    drawVoucher(c);
    bool bottom = atBottom();
    // shortcut: redraws the last 200 lines on every change; a windowed view that patches
    // only what changed is the upgrade if long groups stutter.
    {
        lv_obj_clean(s_log);
        size_t from = c.lines.size() > 200 ? c.lines.size() - 200 : 0;
        for (size_t i = from; i < c.lines.size(); i++) {
            const KqLine& l = c.lines[i];
            if (l.notice) {
                mkWrap(s_log, "\xE2\x80\x94 " + l.text, C_NOTE, kItalic);
                continue;
            }
            lv_obj_t* row = mkBox(s_log);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
            lv_obj_t* who = mkLabel(row, l.who + markOf(l.mark), l.mine ? C_ME : C_OK, kSmall);
            if (l.mark == KqLine::READ) lv_obj_set_style_text_color(who, lv_color_hex(C_ME), 0);
            mkWrap(row, l.text, 0xFFFFFF, kFont);
        }
    }
    if (bottom || force) lv_obj_scroll_to_y(s_log, LV_COORD_MAX, LV_ANIM_OFF);
}

void onSend(lv_event_t*) {
    if (!s_compose || g_chat.empty()) return;
    std::string t = lcdInputBoxText(s_compose);
    if (t.empty()) return;
    kqCommand("send", g_chat, t);
    lv_textarea_set_text(s_compose, "");
    lcdKeyboardClose();
    deferFocus(s_compose);
}

void onAddMember(lv_event_t*) {
    if (g_chat.empty()) return;
    showDialog("Add a member", "Type the voucher code they were given when they chose Join.", "voucher code", "Add",
               [](const std::string& code) { kqCommand("invite", g_chat, code); });
}

void chatBack(lv_event_t*) { showScreen(SC_LIST); }

void buildChat() {
    s_chatScr = mkScreen();
    g_chatHdr = mkHeader(s_chatScr, chatBack);
    lv_obj_t* add = mkLabel(g_chatHdr.bar, LV_SYMBOL_PLUS, C_ACT, kFont);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(add, lcdPx(10));
    lv_obj_add_event_cb(add, onAddMember, LV_EVENT_CLICKED, nullptr);
    focusRing(add);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), add);

    s_voucher = mkBox(s_chatScr);
    lv_obj_set_width(s_voucher, lv_pct(100));
    lv_obj_set_flex_flow(s_voucher, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_voucher, lcdPx(6), 0);
    lv_obj_set_style_bg_color(s_voucher, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(s_voucher, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_voucher, LV_OBJ_FLAG_HIDDEN);

    s_log = mkBody(s_chatScr);

    lv_obj_t* bar = mkRowBox(s_chatScr, lcdPx(6));
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_RULE), 0);
    lv_obj_set_style_pad_all(bar, lcdPx(4), 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, [](lv_event_t*) { if (s_compose) lcdInputBoxActivate(s_compose); }, LV_EVENT_CLICKED,
                        nullptr);
    s_compose = mkField(bar, "message", 3);
    lv_obj_set_width(s_compose, 1);
    lv_obj_set_flex_grow(s_compose, 1);
    lv_obj_add_flag(s_compose, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_compose, onSend, LV_EVENT_READY, nullptr);
    mkButton(bar, "Send", true, onSend);
}

void showScreen(Screen sc) {
    g_screen = sc;
    if (sc == SC_LIST) {
        g_chat.clear();
        lv_obj_add_flag(s_chatScr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        drawList();
        deferFocus(lv_obj_get_child(s_listBody, 0));
    } else {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_chatScr, LV_OBJ_FLAG_HIDDEN);
        drawChat(true);
        deferFocus(s_compose);
    }
}

void openChat(const std::string& id) {
    g_chat = id;
    showScreen(SC_CHAT);
}

void refreshTimerCb(lv_timer_t*) {
    if (!s_layer) return;
    uint32_t g = kqGeneration();
    if (g == g_seen) return;
    g_seen = g;
    if (g_screen == SC_LIST) drawList();
    else drawChat(false);
}

}  // namespace

KqApp::KqApp() : LcdApp({.name = "Group Chat", .iconBasename = "kq"}) {}

void KqApp::onCreate(lv_obj_t* root) {
    refreshFont();
    s_layer = root;
    lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    buildList();
    buildChat();
    showScreen(SC_LIST);
    g_seen = kqGeneration();
    g_timer = lv_timer_create(refreshTimerCb, 500, nullptr);
}

bool KqApp::onBack() {
    if (!s_layer) return false;
    if (s_dialog) { closeDialog(); return true; }
    if (g_screen == SC_CHAT) { showScreen(SC_LIST); return true; }
    return false;
}

void KqApp::onShow() {
    if (g_timer) lv_timer_resume(g_timer);
    g_seen = 0;  // redraw on the next tick
}

void KqApp::onHide() {
    if (g_timer) lv_timer_pause(g_timer);
}

void KqApp::onClose() {
    if (g_timer) { lv_timer_delete(g_timer); g_timer = nullptr; }
    s_layer = s_list = s_listBody = s_chatScr = s_log = s_compose = s_voucher = s_dialog = s_dialogField = nullptr;
    g_listHdr = Header{};
    g_chatHdr = Header{};
    g_focusTarget = nullptr;
    g_chat.clear();
    g_screen = SC_LIST;
}
