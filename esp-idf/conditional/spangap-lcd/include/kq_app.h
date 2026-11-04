// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 group chat on the screen: the groups, a group's lines with a composer, and the
// dialogs that start, join and add to a group. Compiled only when spangap-lcd is staged.
#pragma once

#include "lcd_app.h"
#include "lvgl.h"

class KqApp : public LcdApp {
public:
    KqApp();
    void onCreate(lv_obj_t* root) override;
    bool onBack() override;
    void onShow() override;
    void onHide() override;
    void onClose() override;
};
