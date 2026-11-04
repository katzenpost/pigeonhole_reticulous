// Stand-in for spangap-lcd's lcd_internal.h: what lcd_input_box.cpp calls.
#pragma once
#include "lvgl.h"
void lcdPointerHide(void);
lv_obj_t* lcdKeyboardTarget(void);
int lcdKeyboardRoomFor(lv_obj_t* o);
