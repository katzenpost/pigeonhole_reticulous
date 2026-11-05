# kq_ctidh

CTIDH-1024 from [highctidh](https://codeberg.org/vula/highctidh) (in `highctidh/`, unchanged),
for the client's voucher NIKE.

## The field backend

All of CTIDH's time goes into four field functions, `fiat_p1024_mul`, `_square`, `_add` and
`_sub`. `KQ_CTIDH_FIELD` picks who provides them, in the firmware
(`components/kq_ctidh/CMakeLists.txt`) and in the host builds (`test/kq.mk`):

| `KQ_CTIDH_FIELD` | Field | Notes |
|---|---|---|
| `xtensa` (default on the ESP32-S3) | `fp1024_xtensa.S`: MAC16 assembly, product scanning over signed 16-bit digits | firmware only; ~11.4k instructions per multiply; fiat's four compiled as `*_ref` |
| `mont32` (default elsewhere, and on the host) | `fp1024_mont32.c`: 32-bit Montgomery, loops, logic carries | fiat's four compiled as `*_ref`, for the tests; the firmware's `--gc-sections` drops them |
| `fiat` | fiat-crypto's `highctidh/fiat_p1024.c` | 190 KB of straight-line code per multiply on Xtensa |

Firmware: `idf.py -DKQ_CTIDH_FIELD=fiat` (through spangap:
`spangap build -w … -- reticulous/reticulous -DKQ_CTIDH_FIELD=fiat build`). The value stays in the
build's CMake cache until it is set again. With no value the firmware takes `xtensa` when
`IDF_TARGET` is `esp32s3` and `mont32` on other targets (the Linux image), which cannot assemble
it; `xtensa` on another target stops the build. Host: `make -C test/host KQ_CTIDH_FIELD=fiat`.

With mont32, `linker.lf` puts `fp1024_mont32` in IRAM (`noflash`), and with xtensa
`linker_xtensa.lf` does the same for `fp1024_xtensa`, so the field never waits on flash through
the 16 KB instruction cache. It costs internal RAM, which WiFi needs
(`spangap-core/docs/memory.md`). T-Deck image, fiat → mont32:

| | fiat | mont32 |
|---|---|---|
| `.iram0.text` | 116,343 | 117,695 (+1,352) |
| `.dram0.data` | 31,744 | 31,876 (+132: p) |
| `.dram0.bss` | 98,656 | 98,656 |
| D/IRAM remaining (`idf.py size`) | 110,373 | 108,889 (−1,484) |
| heap start (boot-time free internal heap) | 0x3fcc0860 | 0x3fcc0de8 (−1,416) |
| app image | 0x5de420 | 0x580560 (−384 KB) |

xtensa against mont32, in `test_apps/field`'s image: `.iram0.text` −44 B, `.dram0.data` +128 B
(p's signed digits beside p).

## Checks

- Host: `make -C esp-idf/test/host` runs `test_field` (mont32 against `*_ref`, bit for bit)
  with the rest; `INT128=0` builds fiat as on a 32-bit core.
- ESP32-S3, in Espressif's QEMU: `test_apps/field` runs the same comparison on Xtensa.
  Build it with ESP-IDF 5.5, then run its merged flash image:

  ```sh
  idf.py -C esp-idf/components/kq_ctidh/test_apps/field -B <build> -DSDKCONFIG=<build>/sdkconfig build
  (cd <build> && python -m esptool --chip esp32s3 merge_bin --fill-flash-size 2MB -o flash.bin @flash_args)
  qemu-system-xtensa -nographic -machine esp32s3 -accel tcg,one-insn-per-tb=on -drive file=<build>/flash.bin,if=mtd,format=raw
  ```

  QEMU comes from `idf_tools.py install qemu-xtensa`. The run ends with `field: PASS`.
  `one-insn-per-tb` is needed for xtensa: its MAC16 load-and-multiply instructions often load
  one of their own source registers, which the ISA defines, and QEMU 9.2.2 gets that wrong
  between the instructions of one translation block.
- Timing: `kq bench field|ctidh` on a device, `esp-idf/test/bench` on the host.
