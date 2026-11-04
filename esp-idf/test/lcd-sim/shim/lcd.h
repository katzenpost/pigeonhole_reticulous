// Stand-in for spangap-lcd's lcd.h: the part of it the screens call.
#pragma once
#include "lvgl.h"

typedef void (*lcd_modal_close_cb_t)(void* ud);
void lcdModalTrack(lv_obj_t* overlay, lcd_modal_close_cb_t closeCb = nullptr, void* ud = nullptr);
void lcdModalUntrack(lv_obj_t* overlay);

enum class LcdFace { UI, UI_BOLD, UI_ITALIC, MONO, MONO_BOLD, MONO_ITALIC, SYMBOLS };
const lv_font_t* lcdFont(LcdFace face, int px);
int lcdPx(int px);

lv_group_t* lcdInputGroup(void);
bool lcdKeyboardOnScreen(void);
void lcdKeyboardOpen(lv_obj_t* ta);
void lcdKeyboardClose(void);
void lcdKeyboardAttach(lv_obj_t* ta);
bool lcdCaretActive(int* x, int* y, bool* atTop);
void lcdCaretRelease(void);
