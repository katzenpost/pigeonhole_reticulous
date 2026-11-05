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
`spangap build -w … -- reticulous/reticulous -DKQ_CTIDH_FIELD=fiat build`). The value counts for
the configure run it is given to and is then dropped from the CMake cache, so give it to every
build that should use it (`spangap build` configures on every build); a tree left with a value
from before this rule uses it once more. With no value the firmware takes `xtensa` when
`IDF_TARGET` is `esp32s3` and `mont32` on other targets (the Linux image), which cannot assemble
it; `xtensa` on another target stops the build. The build log names the field
(`kq_ctidh: the xtensa CTIDH field`), and an ESP32-S3 build with another field warns. Host:
`make -C test/host KQ_CTIDH_FIELD=fiat`.

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
- The voucher's CTIDH on the ESP32-S3, in QEMU: `make -C esp-idf/test/qemu-voucher run` runs
  `test/host/test_voucher.cpp` (mint, induct, sealed reply and open, both forms, against hpqc's
  vectors) with the field the firmware takes there, its stack in PSRAM as on the device: once
  with a seal's two CTIDH actions in turn, once on both cores.
- Timing: `kq bench field|ctidh|join` on a device, `esp-idf/test/bench` on the host.

## Both cores for a voucher join
A seal makes an ephemeral keypair and its shared secret with the recipient's key: two CTIDH
actions from one private key, neither feeding the other. `kq::nike::both` runs them
(`src/kq_nike.cpp`): `in_turn`, or `at_once`, which on the ESP32-S3 runs the keypair's action on
a task pinned to the other core, its 384 KB stack in PSRAM, and falls back to in turn when that
task cannot be made. Per join step:

| step | side | CTIDH | at once |
|---|---|---|---|
| mint | joiner | the seal keypair: one action | nothing to pair |
| induct | member | the voucher key checked (`validate`); then the seal: the ephemeral keypair ‖ the shared secret (which checks the key again) | the seal's two actions |
| open | joiner | the shared secret with the ephemeral key; with the seal public key not at hand (after a restart, compact form), that key too | those two, then |

The firmware's `s.kq.cores` (default 2) set to 1 runs them in turn. `kq bench join` times the
induct and the open both ways from one seed and checks the outputs match. The two cores share
the caches and the flash/PSRAM bus, and Wi-Fi runs on core 0, so the gain is under 2x: it is for
the device run to measure.
