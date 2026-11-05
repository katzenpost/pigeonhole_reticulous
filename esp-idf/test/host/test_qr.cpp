// SPDX-License-Identifier: AGPL-3.0-only
// The voucher QR code as the T-Deck draws it (3 pixels a module, 4-module quiet zone, dark on
// light) decodes back to exactly the voucher code, with zbarimg.
#include <cstdio>

#include "kq1_engine.h"
#include "kq_qr.h"
#include "vectors.h"

int main(int, char** argv) {
    auto vs = load(argv[1], "voucher/voucher_vectors_v2.json");
    for (auto& v : vs) {
        if (v.name != "mint") continue;
        std::string code = kq1::voucher_code(v.hex("voucher"));
        uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(kKqQrMax)], tmp[sizeof qr];
        check(kqQr(code, qr, tmp), v, "encodes");
        int m = qrcodegen_getSize(qr), px = 3, side = (m + 8) * px;
        check(m == 33, v, "version 4");
        std::string pgm = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : ".") + "/test_qr.pgm";
        FILE* f = fopen(pgm.c_str(), "wb");
        if (!f) return 2;
        fprintf(f, "P5 %d %d 255\n", side, side);
        for (int y = 0; y < side; y++)
            for (int x = 0; x < side; x++) fputc(qrcodegen_getModule(qr, x / px - 4, y / px - 4) ? 0 : 255, f);
        fclose(f);
        std::string cmd = "zbarimg --quiet --raw -Sdisable -Sqrcode.enable " + pgm, got;
        FILE* p = popen(cmd.c_str(), "r");
        char b[256];
        while (p && fgets(b, sizeof b, p)) got += b;
        check(p && pclose(p) == 0, v, "zbarimg decodes");
        check(got == code + "\n", v, "decodes to the voucher code");
        printf("%s -> %s", code.c_str(), got.c_str());
    }
    return finish("qr");
}
