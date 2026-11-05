# Field instruction counts on the T-Deck's core, in QEMU

The instructions each CTIDH-1024 field primitive executes per call on the ESP32-S3 (the LILYGO
T-Deck Plus's Xtensa LX7), and its code size, built with the T-Deck's flags and run in
Espressif's QEMU. The host benchmark (`test/bench`) is the wrong yardstick for choices in the
field: a host is a different core with different costs. These counts are exact and come out
the same on every run, so one change to a primitive shows as a number going up or down.

```
make -C esp-idf/test/qemu run [KQ_CTIDH_FIELD=mont32|fiat] [BENCH_ARGS=<name prefixes>]
```

```
| primitive | instructions/call | bytes |
|---|---:|---:|
| field-mul-mont32 | 49330 | 511 |
| field-square-mont32 | 39423 | 655 |
| field-add-mont32 | 1925 | 242 |
| field-sub-mont32 | 1627 | 194 |
| field-mul-fiat-ref | 82045 | 193428 |
| field-square-fiat-ref | 78585–78619 | 185812 |
| field-add-fiat-ref | 847–871 | 2595 |
| field-sub-fiat-ref | 719 | 2074 |

Outputs bit-exact against *_ref: 4 of 4 primitives, 16 chained calls each.
```

- `KQ_CTIDH_FIELD` picks the field as `test/kq.mk` does: `mont32` (`components/kq_ctidh/fp1024_mont32.c`,
  with fiat's four functions built beside it as `*_ref`) or `fiat` (fiat-crypto's alone).
- `BENCH_ARGS`: comma-separated prefixes of the names in the first column, e.g. `field-mul` or
  `field-mul-mont32,field-add`; a prefix that matches nothing fails and lists the names. Every
  run counts all primitives (about a second in QEMU); the filter picks the rows.
- Needs podman with `docker.io/espressif/idf:v5.5.4` (the build) and Espressif's QEMU on the
  host (the run): `IDF_TOOLS_PATH=<dir>` where `idf_tools.py install qemu-xtensa` put it, or
  `QEMU=<path to qemu-system-xtensa>`. On the shared machine, prefix it with `bench-runner/build.sh`.
- The first build takes about two minutes at `-j1`; from a fresh checkout with a warm
  `CCACHE_DIR` (default `~/.cache/ccache`) about 30 s; after a change to a primitive a few seconds.
  The counts do not depend on the host's load, so they need no quiet machine.
- `build-<field>/qemu.out` is QEMU's whole output; `build-<field>/field_count.dis` the disassembly.

## What the columns are

**instructions/call**: every instruction from the primitive's `entry` through its `retw`, with
everything it calls (ROM's `memcpy` too), on fully reduced inputs, each call's result the next
call's input, over 16 calls. A range means the count depends on the data: fiat's code on this
core compiles some carries to branches. The outputs of every primitive that has a `*_ref` are
compared with the `*_ref`'s on the same inputs in the same run, and a difference fails the run.

**bytes**: the primitive's function and every function it calls in the app, with the literals
they load (`l32r`); not ROM's `memcpy`.

## How it counts

QEMU runs with `-icount shift=7`: its virtual clock advances 2^7 ns per instruction executed,
and CCOUNT, which QEMU ticks at 40 MHz from that clock, by 5.12 per instruction. The app
(`main/field_count.c`) reads CCOUNT before and after a call with every interrupt masked, and
divides by the ticks per instruction, which it measures on a block of 2^20 + 2 instructions and
then checks on a straight-line block of 1003, which must count exactly. A call of an empty
function is the baseline.

QEMU does not stall the second core as IDF's single-core start asks: it spins in ROM, waiting
for a start address, and under `-icount` its instructions advance the same clock. The app gives
it one that waits for an interrupt that never comes.

Cross-check against the disassembly, `field-add-mont32`: `fiat_p1024_add` is 667 instructions
(27 around a 20-instruction loop run 32 times), `reduce_once` 973 (13 around loops of 21 and 9,
32 times each), and each of its three `memcpy`s of 128 aligned bytes 95 in ROM (the jump-table
stub's 2, then 8, a 10-instruction loop run 8 times, and 5): 667 + 973 + 3 × 95 = 1925, as
counted. `field-sub-mont32` likewise: 30 around loops of 21 and 20, so 1342, + 3 × 95 = 1627.

## What QEMU does not model

These are instructions, not time. QEMU does not model the instruction or data cache, so not
their misses; not the wait states of flash and PSRAM (fiat's multiply, 190 KB of code against
a 16 KB instruction cache, streams from flash on every call on the device); and not the
pipeline, so not the cycles an instruction takes: a multiply, a load or a taken branch counts
one like an add. Device timing needs the board.

## Adding a primitive

Declare it in `main/field_count.c` and add a row to `prims[]`: its name (the table's first
column), the function, and its `*_ref` if it has one. A primitive is called as `f(x, x, y)`; a
unary one (`out, a`) is called through the same pointer type, its `y` unused. If it is in a
source the build does not have yet (e.g. `components/kq_ctidh/fp1024_xtensa.S`), add the file to
`FIELD_SRCS` in `main/CMakeLists.txt`.
