// SPDX-License-Identifier: AGPL-3.0-only
// test/host/test_voucher.cpp on the ESP32-S3, with the CTIDH field the firmware builds there:
// mint, induct and open in both forms against hpqc's vectors. It runs on a task with its stack
// in PSRAM, as the firmware's kq.ctidh worker does.
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define main test_voucher_main
#include "test_voucher.cpp"
#undef main

extern const char vectors_txt[] asm("_binary_vectors_txt_start");

static void run(void*) {
    g_vectors_text = vectors_txt;
    char none[] = "", *argv[] = {none, none};
    int64_t t0 = esp_timer_get_time();
    int r = test_voucher_main(2, argv);
    printf("voucher: %s (%lld ms)\n", r ? "FAIL" : "PASS", (long long)((esp_timer_get_time() - t0) / 1000));
    fflush(stdout);
    esp_restart();
}

extern "C" void app_main() {
    if (xTaskCreatePinnedToCoreWithCaps(run, "voucher", 384 * 1024, nullptr, 5, nullptr, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        printf("voucher: FAIL (no PSRAM for the task's stack)\n");
        fflush(stdout);
        esp_restart();
    }
}
