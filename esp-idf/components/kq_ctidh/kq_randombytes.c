/* highctidh's randombytes, from libsodium's RNG (the ESP32 hardware RNG on the device). */
#include <sodium/randombytes.h>

#include "highctidh/randombytes.h"

void randombytes(void *x, size_t l) { randombytes_buf(x, l); }
