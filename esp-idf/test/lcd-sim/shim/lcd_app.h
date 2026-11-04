// Stand-in for spangap-lcd's lcd_app.h: an app is built into a root and shown.
#pragma once
#include "lcd.h"

class LcdApp {
public:
    struct Config {
        const char* name = "";
        const char* iconBasename = "";
        bool statusBar = true, navBar = false, fullscreen = false;
    };
    explicit LcdApp(const Config& cfg) : m_cfg(cfg) {}
    virtual ~LcdApp() {}
    virtual void onCreate(lv_obj_t* root) = 0;
    virtual void onShow() {}
    virtual void onHide() {}
    virtual bool onBack() { return false; }
    virtual void onClose() {}
    const Config& cfg() const { return m_cfg; }

private:
    Config m_cfg;
};
