// SPDX-License-Identifier: AGPL-3.0-only
// The T-Deck's screen on the desktop: KqApp (or KqRadioApp, --app radio) on a 320x240 LVGL display drawn in memory, below a
// status bar, with the board's keypad and pointer fed from commands on standard input:
//   type <text> | key enter|esc|tab|backtab|up|down|left|right|bs | click <x> <y> | back
//   shot <file.png> | sleep <s> | until <text> <s> | dump | quit
// The chats are saved in --state, which must exist, and loaded from it at start.
// `until` waits until the chats (as `dump` prints them) contain the text.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "kq_app.h"
#include "kq_radio_app.h"
#include "kq_view.h"
#include "lcd.h"
#include "lcd_internal.h"
#include "sim.h"

static constexpr int W = 320, H = 240, kStatusH = 24;
static std::string g_fonts;
static lv_group_t* g_group;

// ---- the spangap-lcd calls the screens make ----

static std::map<int, lv_font_t*> s_fonts;
const lv_font_t* lcdFont(LcdFace face, int px) {
    int key = (int)face << 16 | px;
    auto it = s_fonts.find(key);
    if (it != s_fonts.end()) return it->second;
    const char* file = "ui.ttf";
    lv_freetype_font_style_t st = LV_FREETYPE_FONT_STYLE_NORMAL;
    switch (face) {
        case LcdFace::UI_BOLD: file = "ui-semibold.ttf"; break;
        case LcdFace::UI_ITALIC: st = LV_FREETYPE_FONT_STYLE_ITALIC; break;
        case LcdFace::MONO: file = "mono.ttf"; break;
        case LcdFace::MONO_BOLD: file = "mono.ttf"; st = LV_FREETYPE_FONT_STYLE_BOLD; break;
        case LcdFace::MONO_ITALIC: file = "mono.ttf"; st = LV_FREETYPE_FONT_STYLE_ITALIC; break;
        case LcdFace::SYMBOLS: file = "symbols.ttf"; break;
        default: break;
    }
    std::string path = g_fonts + "/" + file;
    lv_font_t* f = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, px, st);
    if (!f) { fprintf(stderr, "sim: no font %s\n", path.c_str()); exit(1); }
    if (face != LcdFace::SYMBOLS) f->fallback = lcdFont(LcdFace::SYMBOLS, px);
    s_fonts[key] = f;
    return f;
}
int lcdPx(int px) { return px; }
lv_group_t* lcdInputGroup(void) { return g_group; }
bool lcdKeyboardOnScreen(void) { return false; }  // the T-Deck has keys
void lcdKeyboardOpen(lv_obj_t*) {}
void lcdKeyboardClose(void) {}
void lcdKeyboardAttach(lv_obj_t*) {}
lv_obj_t* lcdKeyboardTarget(void) { return nullptr; }
int lcdKeyboardRoomFor(lv_obj_t*) { return H - kStatusH; }
void lcdPointerHide(void) {}
void lcdModalTrack(lv_obj_t*, lcd_modal_close_cb_t, void*) {}
void lcdModalUntrack(lv_obj_t*) {}

// ---- display and input ----

static uint16_t s_fb[W * H];
static void flush(lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); }

struct Ev {
    bool pointer;
    uint32_t key;
    int x, y;
};
static std::deque<Ev> s_keys, s_taps;
static bool s_keyDown, s_tapDown;
static uint32_t s_lastKey;
static lv_point_t s_lastTap;

static void readKeys(lv_indev_t*, lv_indev_data_t* d) {
    if (s_keyDown) { d->key = s_lastKey; d->state = LV_INDEV_STATE_RELEASED; s_keyDown = false; return; }
    if (s_keys.empty()) { d->key = s_lastKey; d->state = LV_INDEV_STATE_RELEASED; return; }
    s_lastKey = s_keys.front().key;
    s_keys.pop_front();
    d->key = s_lastKey;
    d->state = LV_INDEV_STATE_PRESSED;
    s_keyDown = true;
}

static void readPointer(lv_indev_t*, lv_indev_data_t* d) {
    if (s_tapDown) { d->point = s_lastTap; d->state = LV_INDEV_STATE_RELEASED; s_tapDown = false; return; }
    d->point = s_lastTap;
    if (s_taps.empty()) { d->state = LV_INDEV_STATE_RELEASED; return; }
    s_lastTap = {(int32_t)s_taps.front().x, (int32_t)s_taps.front().y};
    s_taps.pop_front();
    d->point = s_lastTap;
    d->state = LV_INDEV_STATE_PRESSED;
    s_tapDown = true;
}

static void shot(const std::string& file) {
    lv_refr_now(nullptr);
    std::string cmd = "magick -size 320x240 -depth 8 rgb:- -filter point -resize 200% '" + file + "'";
    FILE* p = popen(cmd.c_str(), "w");
    if (!p) return;
    for (int i = 0; i < W * H; i++) {
        uint16_t c = s_fb[i];
        uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3 | (c >> 13)), (uint8_t)(((c >> 5) & 63) << 2 | ((c >> 9) & 3)),
                          (uint8_t)((c & 31) << 3 | ((c >> 2) & 7))};
        fwrite(rgb, 1, 3, p);
    }
    pclose(p);
}

// The status bar the shell keeps above an app: here only the clock.
static lv_obj_t* s_clock;
static void clockTick(lv_timer_t*) {
    time_t t = time(nullptr);
    char b[8];
    strftime(b, sizeof b, "%H:%M", localtime(&t));
    lv_label_set_text(s_clock, b);
}

static std::string dump() {
    static const char* marks[] = {"", " \xE2\x80\xA6", " \xE2\x9C\x93", " \xE2\x9C\x93\xE2\x9C\x93"};
    std::string s;
    for (auto& c : kqChats("", true)) {
        s += c.id + ": " + (c.joined ? "joined" : "not joined") + ", " + std::to_string(c.members) + " others";
        if (!c.voucher.empty()) s += ", voucher " + c.voucher;
        s += "\n";
        for (auto& l : c.lines)
            s += l.notice ? "  - " + l.text + "\n" : "  " + l.who + marks[l.mark] + ": " + l.text + "\n";
    }
    return s;
}

static std::mutex s_inMu;
static std::deque<std::string> s_in;

int main(int argc, char** argv) {
    std::string socketPath, name = "tdeck", stateDir, appName = "chat";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--socket") socketPath = argv[i + 1];
        else if (a == "--name") name = argv[i + 1];
        else if (a == "--fonts") g_fonts = argv[i + 1];
        else if (a == "--state") stateDir = argv[i + 1];
        else if (a == "--app") appName = argv[i + 1];
    }
    if (socketPath.empty() || g_fonts.empty() || stateDir.empty()) {
        fprintf(stderr, "usage: kq-sim --socket <relay socket> --fonts <spangap-lcd fonts dir> --state <dir> [--name <name>] [--app chat|radio]\n");
        return 2;
    }
    simClientStart(socketPath, name, 1549, stateDir);

    lv_init();
    lv_tick_set_cb([]() -> uint32_t {
        return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    });
    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_fb, nullptr, sizeof s_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush);
    lv_display_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                                                     lv_palette_main(LV_PALETTE_RED), true, lcdFont(LcdFace::UI, 14)));
    g_group = lv_group_create();
    lv_group_set_default(g_group);
    lv_indev_t* kp = lv_indev_create();
    lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kp, readKeys);
    lv_indev_set_group(kp, g_group);
    lv_indev_t* ptr = lv_indev_create();
    lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ptr, readPointer);

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, W, kStatusH);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    s_clock = lv_label_create(bar);
    lv_obj_set_style_text_font(s_clock, lcdFont(LcdFace::UI_BOLD, 13), 0);
    lv_obj_set_style_text_color(s_clock, lv_color_white(), 0);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 8, 0);
    clockTick(nullptr);
    lv_timer_create(clockTick, 10000, nullptr);
    lv_obj_t* root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, kStatusH);
    lv_obj_set_size(root, W, H - kStatusH);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    KqApp chat;
    KqRadioApp radio;
    LcdApp& app = appName == "radio" ? static_cast<LcdApp&>(radio) : static_cast<LcdApp&>(chat);
    app.onCreate(root);
    app.onShow();

    std::thread([] {
        std::string l;
        while (std::getline(std::cin, l)) { std::lock_guard<std::mutex> g(s_inMu); s_in.push_back(l); }
        std::lock_guard<std::mutex> g(s_inMu);
        s_in.push_back("quit");
    }).detach();

    static const std::map<std::string, uint32_t> keys = {
        {"enter", LV_KEY_ENTER}, {"esc", LV_KEY_ESC},   {"tab", LV_KEY_NEXT},   {"backtab", LV_KEY_PREV},
        {"up", LV_KEY_UP},       {"down", LV_KEY_DOWN}, {"left", LV_KEY_LEFT}, {"right", LV_KEY_RIGHT},
        {"bs", LV_KEY_BACKSPACE}};
    auto clock = [] { return std::chrono::steady_clock::now(); };
    auto busyUntil = clock();
    std::string untilText;
    for (;;) {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!s_keys.empty() || !s_taps.empty() || s_keyDown || s_tapDown) continue;
        if (clock() < busyUntil) {
            if (untilText.empty() || dump().find(untilText) == std::string::npos) continue;
            printf("until: found\n");
            fflush(stdout);
        } else if (!untilText.empty()) {
            printf("until: timed out\n%s", dump().c_str());
            fflush(stdout);
        }
        untilText.clear();
        busyUntil = clock();
        std::string line;
        {
            std::lock_guard<std::mutex> g(s_inMu);
            if (s_in.empty()) continue;
            line = s_in.front();
            s_in.pop_front();
        }
        size_t sp = line.find(' ');
        std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (cmd == "type") {
            for (unsigned char c : arg) s_keys.push_back({false, c, 0, 0});
        } else if (cmd == "key" && keys.count(arg)) {
            s_keys.push_back({false, keys.at(arg), 0, 0});
        } else if (cmd == "click") {
            int x = 0, y = 0;
            sscanf(arg.c_str(), "%d %d", &x, &y);
            s_taps.push_back({true, 0, x, y});
        } else if (cmd == "back") {
            app.onBack();
        } else if (cmd == "shot") {
            shot(arg);
            printf("shot: %s\n", arg.c_str());
        } else if (cmd == "sleep") {
            busyUntil = clock() + std::chrono::milliseconds((int)(atof(arg.c_str()) * 1000));
        } else if (cmd == "until") {
            size_t k = arg.rfind(' ');
            untilText = arg.substr(0, k);
            busyUntil = clock() + std::chrono::seconds(atoi(arg.substr(k + 1).c_str()));
        } else if (cmd == "dump") {
            printf("channel %s; saved %s\n%s", simChannelUp() ? "up" : "down", simSaved().c_str(), dump().c_str());
        } else if (cmd == "quit") {
            return 0;
        } else if (!cmd.empty()) {
            printf("unknown: %s\n", line.c_str());
        }
        fflush(stdout);
    }
}
