// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 group chat on the screen. Everything runs on the lcd task: the screens read the
// chats through kq_view.h (a copy taken under the engine's lock) and send commands back through
// it; a timer redraws whatever is shown when the engine has taken a step. The look follows rrc's.
#include "kq_app.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "sdkconfig.h"
#include "lcd.h"
#include "lcd_input_box.h"
#include "kq1_engine.h"
#include "kq_qr.h"
#include "kq_theme.h"
#include "kq_view.h"

namespace {

using namespace kqui;

const lv_font_t *kFont, *kBold, *kSmall, *kBtn, *kBig;
void refreshFont() {
    kFont = lcdFont(LcdFace::UI, lcdPx(PX_BODY));
    kBold = lcdFont(LcdFace::UI_BOLD, lcdPx(PX_BODY));
    kSmall = lcdFont(LcdFace::UI, lcdPx(PX_SMALL));
    kBtn = lcdFont(LcdFace::UI_BOLD, lcdPx(PX_SMALL));
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

// FOCUSED, not only FOCUS_KEY: focus placed by a click or by the app shows too.
void focusRing(lv_obj_t* o) {
    lv_obj_set_style_outline_width(o, lcdPx(2), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(o, lv_color_hex(C_ME), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(o, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(o, 1, LV_STATE_FOCUSED);
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
    styleRow(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), r);
    return r;
}

struct Header {
    lv_obj_t* bar = nullptr;
    lv_obj_t* back = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* right = nullptr;
};

// The header bar: a back chevron when there is somewhere to go back to, the title, a note.
Header mkHeader(lv_obj_t* screen, lv_event_cb_t onBack) {
    Header h;
    h.bar = headerBar(screen);
    if (onBack) {
        lv_obj_t* back = h.back = mkLabel(h.bar, LV_SYMBOL_LEFT, C_ACT, kFont);
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
    focusRing(f);
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
    lv_obj_set_scrollbar_mode(f, LV_SCROLLBAR_MODE_OFF);  // it would draw over the text
    styleField(f, placeholder);
    lcdKeyboardAttach(f);
    return f;
}

// ---- state ----

enum Screen { SC_LIST, SC_CHAT, SC_LOCK };
Screen g_screen = SC_LIST;
std::string g_chat;          // the group on the chat screen
uint32_t g_seen = 0;         // the engine generation last drawn
lv_timer_t* g_timer = nullptr;

lv_obj_t *s_list = nullptr, *s_listBody = nullptr, *s_chatScr = nullptr, *s_log = nullptr, *s_compose = nullptr,
         *s_voucher = nullptr, *s_dialog = nullptr, *s_dialogField = nullptr, *s_dialogNote = nullptr,
         *s_dialogOk = nullptr, *s_composeBar = nullptr;
bool g_listening = false;    // the dialog listens for a voucher code by sound
std::string g_soundShown;    // what of kqSound() was last drawn
bool g_sendingShown = false;
Header g_listHdr, g_chatHdr;

std::string statusOf(const KqChat& c) {
    if (!c.voucher.empty()) return c.voucher == "\xE2\x80\xA6" ? "making your voucher\xE2\x80\xA6" : "waiting to be added";
    if (!c.joined) return "joining\xE2\x80\xA6";
    std::string s = !c.members ? "only you" : std::to_string(c.members) + (c.members == 1 ? " other member" : " other members");
    if (c.adding) s += " \xC2\xB7 adding one";
    if (c.pending) s += " \xC2\xB7 sending";
    return s;
}

// When a line was said: HH:MM today, else the day ("4 Nov", and the time with full); nothing
// while the clock has not been set.
std::string timeOf(int64_t t, bool full) {
    if (t < 1704067200) return "";  // before 2024: the clock is not set
    time_t at = (time_t)t, now = time(nullptr);
    struct tm a, n;
    localtime_r(&at, &a);
    localtime_r(&now, &n);
    bool today = a.tm_year == n.tm_year && a.tm_yday == n.tm_yday;
    char b[24];
    strftime(b, sizeof b, today ? "%H:%M" : full ? "%e %b %H:%M" : "%e %b", &a);
    return b + (b[0] == ' ');
}

const char* markOf(KqLine::Mark m);
void openChat(const std::string& id);
void showScreen(Screen sc);

// ---- the dialog: one line to type and OK ----

void (*g_dialogOk)(const std::string&) = nullptr;

void closeDialog() {
    if (!s_dialog) return;
    lv_obj_t* o = s_dialog;
    s_dialog = s_dialogField = s_dialogNote = s_dialogOk = nullptr;
#if CONFIG_STRADDLE_AUDIO
    if (g_listening) kqSoundStop();
#endif
    g_listening = false;
    lcdModalUntrack(o);
    lv_obj_delete(o);
    // The release of the key that closed it lands on whatever has focus now, so not on
    // whatever the group falls back to.
    lv_obj_t* f = g_screen != SC_CHAT ? (s_listBody ? lv_obj_get_child(s_listBody, 0) : nullptr)
                 : lv_obj_has_flag(s_composeBar, LV_OBJ_FLAG_HIDDEN) ? g_chatHdr.back : s_compose;
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

void onListen(lv_event_t*);

void showDialog(const std::string& title, const std::string& hint, const char* placeholder, const char* verb,
                void (*ok)(const std::string&), bool listen = false) {
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
    if (listen) {
        s_dialogNote = mkWrap(box, "", C_SUB, kSmall);
        lv_obj_add_flag(s_dialogNote, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* btns = mkRowBox(box, lcdPx(8));
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (listen) mkButton(btns, "Listen", false, onListen);
    mkButton(btns, "Cancel", false, [](lv_event_t*) { lcdKeyboardClose(); closeDialog(); });
    s_dialogOk = mkButton(btns, verb, true, dialogSubmit);

    lcdModalTrack(s_dialog, [](void*) { closeDialog(); });
    if (lcdKeyboardOnScreen()) lcdKeyboardOpen(s_dialogField);
    else deferFocus(s_dialogField);
}

// ---- the groups ----

void onChatRow(lv_event_t* e) { openChat(boundString(e)); }

void drawList() {
    if (!s_listBody) return;
    // The rows are made again: the keys keep their place on the same row, and focus left on
    // something no longer shown comes to the first.
    lv_group_t* grp = lcdInputGroup();
    lv_obj_t* f = grp ? lv_group_get_focused(grp) : nullptr;
    int at = f && lv_obj_get_parent(f) == s_listBody ? (int)lv_obj_get_index(f) : f && lv_obj_is_visible(f) ? -1 : 0;
    lv_obj_clean(s_listBody);
    auto chats = kqChats();
    lv_label_set_text(g_listHdr.title, "Group chat");
    if (chats.empty()) {
        mkWrap(s_listBody,
               "No groups yet. New group starts one; Join makes a voucher code to give to someone "
               "already in a group.",
               C_SUB, kFont);
        return;
    }
    for (auto& c : chats) {
        // The name and when the latest message came; under it that message, or how the group is.
        lv_obj_t* r = mkRow(s_listBody);
        lv_obj_t* top = mkRowBox(r, lcdPx(6));
        lv_obj_t* t = mkLabel(top, c.id, 0xFFFFFF, kBold);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(t, 1);
        lv_obj_set_flex_grow(t, 1);
        bool said = c.joined && !c.adding && !c.last.text.empty();
        if (said) {
            std::string when = timeOf(c.last.time, false);
            if (c.last.mine) when = std::string(markOf(c.last.mark)) + " " + when;
            mkLabel(top, when, c.last.mine && c.last.mark == KqLine::READ ? C_ME : C_SUB, kSmall);
        }
        lv_obj_t* st = mkLabel(r, said ? (c.last.mine ? "you" : c.last.who) + ": " + c.last.text : statusOf(c),
                               !c.voucher.empty() ? C_AMBER : C_SUB, kSmall);
        lv_label_set_long_mode(st, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(st, lv_pct(100));
        bindString(r, onChatRow, c.id);
    }
    if (at < 0 || !grp) return;
    uint32_t n = lv_obj_get_child_count(s_listBody);
    lv_obj_t* r = n ? lv_obj_get_child(s_listBody, std::min<int>(at, (int)n - 1)) : nullptr;
    if (r && lv_obj_get_group(r)) lv_group_focus_obj(r);
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

// lv_canvas_set_draw_buf drops any image cache entry at the address, so a reused one is not stale.
void freeQr(lv_event_t* e) { lv_draw_buf_destroy((lv_draw_buf_t*)lv_event_get_user_data(e)); }

// The QR code of text, px pixels a module: dark on light in either theme, with a 4-module quiet
// zone. Buffers come from LVGL's heap, in PSRAM on the device.
lv_obj_t* mkQr(lv_obj_t* parent, const std::string& text, int px) {
    size_t n = qrcodegen_BUFFER_LEN_FOR_VERSION(kKqQrMax);
    auto* qr = (uint8_t*)lv_malloc(2 * n);
    if (!qr) return nullptr;
    lv_draw_buf_t* buf = nullptr;
    if (kqQr(text, qr, qr + n)) {
        int side = (qrcodegen_getSize(qr) + 8) * px;
        buf = lv_draw_buf_create(side, side, LV_COLOR_FORMAT_I1, LV_STRIDE_AUTO);
        if (buf) {
            lv_draw_buf_clear(buf, nullptr);
            lv_draw_buf_set_palette(buf, 0, lv_color32_make(0xFF, 0xFF, 0xFF, 0xFF));
            lv_draw_buf_set_palette(buf, 1, lv_color32_make(0, 0, 0, 0xFF));
            uint8_t* d = buf->data + 8;  // past the two palette entries
            for (int y = 0; y < side; y++)
                for (int x = 0; x < side; x++)
                    if (qrcodegen_getModule(qr, x / px - 4, y / px - 4))
                        d[y * buf->header.stride + x / 8] |= 0x80 >> (x % 8);
        }
    }
    lv_free(qr);
    if (!buf) return nullptr;
    lv_obj_t* cv = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(cv, buf);
    lv_obj_add_event_cb(cv, freeQr, LV_EVENT_DELETE, buf);
    return cv;
}

// The voucher, as a QR code and in large mono type beside it, while it waits to be given out.
void drawVoucher(const KqChat& c) {
    if (!s_voucher) return;
    lv_obj_clean(s_voucher);
    if (c.voucher.empty()) { lv_obj_add_flag(s_voucher, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_voucher, LV_OBJ_FLAG_HIDDEN);
    if (c.voucher == "\xE2\x80\xA6") {
        mkWrap(s_voucher, "Making your voucher. This takes a while on this device.", C_AMBER, kSmall);
        return;
    }
    lv_obj_t* row = mkRowBox(s_voucher, lcdPx(6));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    mkQr(row, c.voucher, lcdPx(3));
    lv_obj_t* col = mkBox(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, lcdPx(4), 0);
    std::string grouped;  // in fours, to read out and to type; Add member takes it with the spaces
    for (size_t i = 0; i < c.voucher.size(); i += 4) grouped += (i ? " " : "") + c.voucher.substr(i, 4);
#if CONFIG_STRADDLE_AUDIO
    mkWrap(col, "Your voucher code: a member of the group scans it, types it into Add member, or hears it.",
           C_AMBER, kSmall);
    mkWrap(col, grouped, 0xFFFFFF, kBig);
    lv_obj_t* sr = mkRowBox(col, lcdPx(6));
    lv_obj_t* b = mkButton(sr, g_sendingShown ? "Sending\xE2\x80\xA6" : "Send by sound", false, nullptr);
    bindString(b, [](lv_event_t* e) { kqSoundSend(boundString(e)); }, c.voucher);
    mkLabel(sr, "check " + kq1::voucher_check(c.voucher), C_AMBER, kBold);
#else
    mkWrap(col, "Your voucher code: a member of the group scans it or types it into Add member.", C_AMBER,
           kSmall);
    mkWrap(col, grouped, 0xFFFFFF, kBig);
#endif
}

void drawChat(bool force) {
    if (!s_log || g_chat.empty()) return;
    auto chats = kqChats(g_chat, true);
    lv_label_set_text(g_chatHdr.title, g_chat.c_str());
    if (chats.empty()) { lv_label_set_text(g_chatHdr.right, ""); return; }
    const KqChat& c = chats.front();
    unsigned n = c.members + 1;
    lv_label_set_text(g_chatHdr.right, c.joined ? (std::to_string(n) + (n == 1 ? " member" : " members")).c_str() : "");
    drawVoucher(c);
    // Nothing to send to until the group is joined: the voucher gets the room instead.
    if (c.joined == lv_obj_has_flag(s_composeBar, LV_OBJ_FLAG_HIDDEN)) {
        if (c.joined) {
            lv_obj_remove_flag(s_composeBar, LV_OBJ_FLAG_HIDDEN);
            deferFocus(s_compose);
        } else {
            lv_obj_add_flag(s_composeBar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    bool bottom = atBottom();
    // shortcut: redraws the last 200 lines on every change; a windowed view that patches
    // only what changed is the upgrade if long groups stutter.
    {
        lv_obj_clean(s_log);
        size_t from = c.lines.size() > 200 ? c.lines.size() - 200 : 0;
        const KqLine* prev = nullptr;  // the message just above, with no notice between
        std::string prevWhen;
        for (size_t i = from; i < c.lines.size(); i++) {
            const KqLine& l = c.lines[i];
            if (l.notice) {
                mkWrap(s_log, "\xE2\x80\x94 " + l.text, C_NOTE, kSmall);
                prev = nullptr;
                continue;
            }
            // The sender, the mark and the time once for a run of messages that share them.
            std::string when = timeOf(l.time, true);
            if (!prev || prev->mine != l.mine || prev->who != l.who || prev->mark != l.mark || prevWhen != when) {
                lv_obj_t* hd = mkRowBox(s_log, lcdPx(6));
                lv_obj_set_style_pad_top(hd, lcdPx(4), 0);
                lv_obj_t* who = mkLabel(hd, (l.mine ? std::string("you") : l.who) + markOf(l.mark), l.mine ? C_ME : C_OK, kBtn);
                lv_obj_set_width(who, 1);
                lv_obj_set_flex_grow(who, 1);
                if (!when.empty()) mkLabel(hd, when, C_SUB, kSmall);
            }
            mkWrap(s_log, l.text, 0xFFFFFF, kFont);
            prev = &l;
            prevWhen = when;
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
#if CONFIG_STRADDLE_AUDIO
    showDialog("Add a member",
               "Type the voucher code they were given when they chose Join, or choose Listen while their device "
               "sends it by sound.",
               "voucher code", "Add", [](const std::string& code) { kqCommand("invite", g_chat, code); }, true);
#else
    showDialog("Add a member", "Type the voucher code they were given when they chose Join.", "voucher code", "Add",
               [](const std::string& code) { kqCommand("invite", g_chat, code); });
#endif
}

#if CONFIG_STRADDLE_AUDIO
void onListen(lv_event_t*) {
    g_listening = true;
    g_soundShown.clear();
    kqSoundListen();
}

// What the sound is doing, where it shows: the Add member dialog while it listens, and the
// voucher's Send by sound button while it plays.
void drawSound() {
    KqSound s = kqSound();
    bool sending = s.state == KqSound::SENDING;
    if (sending != g_sendingShown) {
        g_sendingShown = sending;
        if (g_screen == SC_CHAT) drawChat(false);
    }
    std::string key = std::to_string(s.state) + s.code + std::to_string(s.left_ms / 1000);
    if (!s_dialogNote || !g_listening || key == g_soundShown) return;
    g_soundShown = key;
    std::string t;
    uint32_t color = C_SUB;
    if (s.state == KqSound::LISTENING) {
        t = "Listening\xE2\x80\xA6 " + std::to_string((s.left_ms + 999) / 1000) +
            " s. Hold this device 10\xE2\x80\x93" "30 cm from theirs while they choose Send by sound.";
    } else if (s.state == KqSound::HEARD) {
        lv_textarea_set_text(s_dialogField, s.code.c_str());
        t = "Heard a voucher code, check " + kq1::voucher_check(s.code) + ". If their screen shows the same, choose Add.";
        color = C_OK;
        deferFocus(s_dialogOk);
    } else if (s.state == KqSound::FAILED) {
        t = "Not heard: " + s.why + ". Hold the devices closer and choose Listen again.";
        color = C_AMBER;
    }
    lv_label_set_text(s_dialogNote, t.c_str());
    lv_obj_set_style_text_color(s_dialogNote, lv_color_hex(color), 0);
    if (t.empty()) lv_obj_add_flag(s_dialogNote, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_dialogNote, LV_OBJ_FLAG_HIDDEN);
}
#else
void onListen(lv_event_t*) {}
#endif

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

    lv_obj_t* bar = s_composeBar = mkRowBox(s_chatScr, lcdPx(6));
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

// ---- the lock: the first start's choice, and the passphrase after every start ----

lv_obj_t *s_lockScr = nullptr, *s_lockBody = nullptr, *s_pass = nullptr, *s_pass2 = nullptr, *s_lockErr = nullptr;
KqLock g_lockShown = KqLock::OPEN;
std::string g_lockWhy;

// The passphrase out of a field, and the field emptied.
std::string takePass(lv_obj_t* f) {
    if (!f) return "";
    std::string p = lv_textarea_get_text(f);
    lv_textarea_set_text(f, "");
    return p;
}

void lockError(const char* text) {
    if (!s_lockErr) return;
    lv_label_set_text(s_lockErr, text);
    lv_obj_remove_flag(s_lockErr, LV_OBJ_FLAG_HIDDEN);
}

void onUsePassphrase(lv_event_t*) {
    std::string a = takePass(s_pass), b = takePass(s_pass2);
    if (a.empty()) lockError("Type a passphrase, or choose No passphrase.");
    else if (a != b) lockError("The two do not match. Type it twice again.");
    else kqSetup(a);
    std::fill(a.begin(), a.end(), '\0');
    std::fill(b.begin(), b.end(), '\0');
    deferFocus(s_pass);
}

void onNoPassphrase(lv_event_t*) { kqSetup(""); }

void onUnlock(lv_event_t*) {
    std::string a = takePass(s_pass);
    if (!a.empty()) kqUnlock(a);
    std::fill(a.begin(), a.end(), '\0');
}

lv_obj_t* mkPassField(lv_obj_t* parent, const char* placeholder, lv_event_cb_t onEnter) {
    lv_obj_t* f = mkPlainField(parent, placeholder);
    lv_textarea_set_password_mode(f, true);
    lv_obj_set_width(f, lv_pct(100));
    lv_obj_add_event_cb(f, onEnter, LV_EVENT_READY, nullptr);
    return f;
}

void drawLock() {
    std::string why;
    KqLock l = kqLock(&why);
    g_lockShown = l;
    g_lockWhy = why;
    lv_obj_clean(s_lockBody);
    s_pass = s_pass2 = s_lockErr = nullptr;
    lv_obj_t* focus = nullptr;
    switch (l) {
        case KqLock::STARTING: mkWrap(s_lockBody, "Starting\xE2\x80\xA6", C_SUB, kFont); break;
        case KqLock::UNLOCKING: mkWrap(s_lockBody, "Opening your chats. This takes a few seconds.", C_SUB, kFont); break;
        case KqLock::LOST:
            mkWrap(s_lockBody, "Your chats cannot be opened on this device.", 0xFFFFFF, kBold);
            mkWrap(s_lockBody, why, C_AMBER, kSmall);
            break;
        case KqLock::UNDECIDED: {
            mkWrap(s_lockBody, "Protect your chats with a passphrase?", 0xFFFFFF, kBold);
            mkWrap(s_lockBody,
                   "Without one, whoever holds this device can read your chats. With one, you type it at "
                   "each start.",
                   C_SUB, kSmall);
            if (!why.empty()) mkWrap(s_lockBody, why, C_AMBER, kSmall);
            s_pass = mkPassField(s_lockBody, "passphrase", [](lv_event_t*) { deferFocus(s_pass2); });
            s_pass2 = mkPassField(s_lockBody, "the same again", onUsePassphrase);
            s_lockErr = mkWrap(s_lockBody, "", C_AMBER, kSmall);
            lv_obj_add_flag(s_lockErr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t* btns = mkRowBox(s_lockBody, lcdPx(8));
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            mkButton(btns, "No passphrase", false, onNoPassphrase);
            mkButton(btns, "Use passphrase", true, onUsePassphrase);
            focus = s_pass;
            break;
        }
        case KqLock::LOCKED:
            mkWrap(s_lockBody, "Your chats are locked. Type your passphrase to open them.", 0xFFFFFF, kFont);
            s_pass = mkPassField(s_lockBody, "passphrase", onUnlock);
            if (!why.empty()) mkWrap(s_lockBody, why, C_AMBER, kSmall);
            {
                lv_obj_t* btns = mkRowBox(s_lockBody, lcdPx(8));
                lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                mkButton(btns, "Unlock", true, onUnlock);
            }
            focus = s_pass;
            break;
        case KqLock::OPEN: break;
    }
    deferFocus(focus);
}

void buildLock() {
    s_lockScr = mkScreen();
    Header h = mkHeader(s_lockScr, nullptr);
    lv_label_set_text(h.title, "Group chat");
    s_lockBody = mkBody(s_lockScr);
    lv_obj_set_style_pad_row(s_lockBody, lcdPx(6), 0);
    lv_obj_add_flag(s_lockScr, LV_OBJ_FLAG_HIDDEN);
}

void showScreen(Screen sc) {
    g_screen = sc;
    if (sc == SC_LOCK) {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chatScr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_lockScr, LV_OBJ_FLAG_HIDDEN);
        drawLock();
        return;
    }
    lv_obj_add_flag(s_lockScr, LV_OBJ_FLAG_HIDDEN);
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
        deferFocus(lv_obj_has_flag(s_composeBar, LV_OBJ_FLAG_HIDDEN) ? g_chatHdr.back : s_compose);
    }
}

void openChat(const std::string& id) {
    g_chat = id;
    showScreen(SC_CHAT);
}

void refreshTimerCb(lv_timer_t*) {
    if (!s_layer) return;
    std::string why;
    KqLock l = kqLock(&why);
    if (l != KqLock::OPEN) {
        if (g_screen != SC_LOCK || l != g_lockShown || why != g_lockWhy) showScreen(SC_LOCK);
        return;
    }
    if (g_screen == SC_LOCK) { showScreen(SC_LIST); return; }
#if CONFIG_STRADDLE_AUDIO
    drawSound();
#endif
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
    buildLock();
    showScreen(kqLock() == KqLock::OPEN ? SC_LIST : SC_LOCK);
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
#if CONFIG_STRADDLE_AUDIO
    if (g_listening) kqSoundStop();
#endif
    s_layer = s_list = s_listBody = s_chatScr = s_log = s_compose = s_voucher = s_dialog = s_dialogField = nullptr;
    s_dialogNote = s_dialogOk = s_composeBar = nullptr;
    g_listening = g_sendingShown = false;
    g_soundShown.clear();
    s_lockScr = s_lockBody = s_pass = s_pass2 = s_lockErr = nullptr;
    g_lockShown = KqLock::OPEN;
    g_listHdr = Header{};
    g_chatHdr = Header{};
    g_focusTarget = nullptr;
    g_chat.clear();
    g_screen = SC_LIST;
}
