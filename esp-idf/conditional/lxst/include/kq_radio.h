// SPDX-License-Identifier: AGPL-3.0-only
// The radio: voice bursts heard in radio groups, played through the lxst session one at a time,
// first come first played. A burst is held in PSRAM until it has played, and is never written
// anywhere. Compiled only when reticulous/lxst is staged (conditional/lxst/).
#pragma once

#include "service.h"

class KqRadioService : public Service {
public:
    void onInit() override;
};
