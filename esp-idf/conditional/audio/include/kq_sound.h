// SPDX-License-Identifier: AGPL-3.0-only
// Voucher codes by sound (kq_view.h's kqSound*), through the board's audio engine. Compiled only
// when spangap/audio is staged (conditional/audio/), which every T-Deck image is.
#pragma once

#include "service.h"

class KqSoundService : public Service {
public:
    void onInit() override;
};
