// SPDX-License-Identifier: AGPL-3.0-only
// The voucher code as the QR code hushphone shows too: the text in byte mode, level M, the
// smallest version that holds it. qr and tmp hold qrcodegen_BUFFER_LEN_FOR_VERSION(kKqQrMax).
#pragma once
#include <cstring>
#include <string>

#include "qrcodegen.h"

constexpr int kKqQrMax = 10;  // a 44-character voucher code needs version 4

inline bool kqQr(const std::string& text, uint8_t qr[], uint8_t tmp[]) {
    if (text.size() > qrcodegen_BUFFER_LEN_FOR_VERSION(kKqQrMax)) return false;
    memcpy(tmp, text.data(), text.size());
    return qrcodegen_encodeBinary(tmp, text.size(), qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, kKqQrMax,
                                  qrcodegen_Mask_AUTO, false);
}
