// SPDX-License-Identifier: AGPL-3.0-only
// The Radio app: which groups are listened to as radio, and what is playing. Listen-only for now.
// A voice build without a screen gets a service that does nothing in its place.
#pragma once

#include "sdkconfig.h"

#if CONFIG_SPANGAP_LCD
#include "lcd_app.h"

class KqRadioApp : public LcdApp {
public:
    KqRadioApp();
    void onCreate(lv_obj_t* root) override;
    void onShow() override;
    void onHide() override;
    void onClose() override;
};
#else
#include "service.h"

class KqRadioApp : public Service {
public:
    void onInit() override {}
};
#endif
