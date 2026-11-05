# The voucher's crypto on the T-Deck's core: test/host/test_voucher.cpp (mint, induct, sealed
# reply, open; both forms; hpqc's vectors) built for the ESP32-S3 with the firmware's CTIDH field,
# xtensa unless KQ_CTIDH_FIELD says otherwise, and run in Espressif's QEMU. Ends "voucher: PASS".
#   make [KQ_CTIDH_FIELD=xtensa|mont32|fiat] run
# Builds in the ESP-IDF container (podman), runs QEMU here; IDF_TOOLS_PATH as for test/qemu.
KQ_CTIDH_FIELD ?= xtensa
IDF_IMAGE ?= docker.io/espressif/idf:v5.5.4
IDF_TOOLS_PATH ?= $(HOME)/.espressif
QEMU ?= $(firstword $(wildcard $(IDF_TOOLS_PATH)/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa) qemu-system-xtensa)
B := build-$(KQ_CTIDH_FIELD)
SHELL := bash
.SHELLFLAGS := -o pipefail -c

CCACHE_DIR ?= $(HOME)/.cache/ccache
build:
	mkdir -p $(B) $(CCACHE_DIR)
	podman run --rm -v $(abspath ../..):/w -v $(CCACHE_DIR):/ccache -e CCACHE_DIR=/ccache -w /w/test/qemu-voucher $(IDF_IMAGE) bash -c '\
	  set -e; idf.py -B $(B) -DSDKCONFIG=$(B)/sdkconfig -DKQ_CTIDH_FIELD=$(KQ_CTIDH_FIELD) reconfigure; \
	  ninja -C $(B) -j1; \
	  ( cd $(B) && esptool.py --chip esp32s3 merge_bin --fill-flash-size 2MB -o flash.bin @flash_args )' >$(B)/build.log 2>&1 || { tail -30 $(B)/build.log; exit 1; }
	@awk '/^ \.text\.fp1024_/ { s = $$1; getline; print "field: " s " at " $$1 ", " $$2 " bytes, " $$3 }' $(B)/voucher_s3.map

# one-insn-per-tb: QEMU 9.2.2 gets MAC16's load-and-multiply wrong inside a translation block
# (components/kq_ctidh/README.md); -m 8M with is_octal: the T-Deck Plus's 8 MB octal PSRAM;
# -no-reboot: the app's restart when done ends QEMU.
run: build
	timeout 7200 $(QEMU) -nographic -machine esp32s3 -m 8M -global ssi_psram.is_octal=true \
	  -accel tcg,one-insn-per-tb=on -no-reboot \
	  -drive file=$(B)/flash.bin,if=mtd,format=raw </dev/null | tee $(B)/qemu.out | grep -v '^[IW] ('
	grep -q '^voucher: PASS' $(B)/qemu.out

clean:
	rm -rf build-*
.PHONY: build run clean
