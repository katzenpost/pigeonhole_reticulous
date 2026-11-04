# The instructions each CTIDH-1024 field primitive executes per call on the T-Deck's core
# (ESP32-S3), counted in Espressif's QEMU, with its code size, as a Markdown table (README.md).
#   make [KQ_CTIDH_FIELD=mont32|fiat] [BENCH_ARGS=<name prefixes>] run
# BENCH_ARGS: comma-separated name prefixes, as the table's first column prints them, e.g.
# field-mul or field-mul-mont32,field-add. Builds in the ESP-IDF container (podman), runs QEMU here.
KQ_CTIDH_FIELD ?= mont32
IDF_IMAGE ?= docker.io/espressif/idf:v5.5.4
IDF_TOOLS_PATH ?= $(HOME)/.espressif
QEMU ?= $(firstword $(wildcard $(IDF_TOOLS_PATH)/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa) qemu-system-xtensa)
B := build-$(KQ_CTIDH_FIELD)
SHELL := bash
.SHELLFLAGS := -o pipefail -c

# Always: ninja rebuilds only what changed. The container sees esp-idf/ at /w, so its paths, and
# with them the hits in CCACHE_DIR, are the same from any checkout; -j1 as on the shared machine.
CCACHE_DIR ?= $(HOME)/.cache/ccache
build:
	mkdir -p $(B) $(CCACHE_DIR)
	podman run --rm -v $(abspath ../..):/w -v $(CCACHE_DIR):/ccache -e CCACHE_DIR=/ccache -w /w/test/qemu $(IDF_IMAGE) bash -c '\
	  set -e; [ -f $(B)/build.ninja ] || idf.py -B $(B) -DSDKCONFIG=$(B)/sdkconfig -DKQ_CTIDH_FIELD=$(KQ_CTIDH_FIELD) reconfigure; \
	  ninja -C $(B) -j1; \
	  ( cd $(B) && esptool.py --chip esp32s3 merge_bin --fill-flash-size 2MB -o flash.bin @flash_args ); \
	  xtensa-esp32s3-elf-objdump -d $(B)/field_count.elf >$(B)/field_count.dis; \
	  xtensa-esp32s3-elf-nm -S $(B)/field_count.elf >$(B)/field_count.sym' >$(B)/build.log 2>&1 || { tail -30 $(B)/build.log; exit 1; }

# -icount: QEMU's clock, so CCOUNT, advances a fixed 2^7 ns per instruction; -no-reboot: the
# app's restart when done ends QEMU.
run: build
	timeout 300 $(QEMU) -nographic -machine esp32s3 -icount shift=7 -no-reboot \
	  -drive file=$(B)/flash.bin,if=mtd,format=raw </dev/null >$(B)/qemu.out
	python3 table.py $(B)/qemu.out $(B)/field_count.dis $(B)/field_count.sym '$(BENCH_ARGS)'

clean:
	rm -rf build-*
.PHONY: build run clean
