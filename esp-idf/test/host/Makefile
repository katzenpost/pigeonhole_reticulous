# Host tests: plain g++ against the same sources the firmware builds.
# MBEDTLS_DIR names an mbedTLS 3.6 install prefix when it is not a system package.
include ../kq.mk

TESTS := test_bacap test_nike test_wire test_kq1 test_voucher test_replica test_member test_client test_runtime test_store test_vault test_field test_qr test_ogg test_fskv

all: $(addprefix $(B)/,$(TESTS)) $(B)/vectors.txt
	@for t in $(TESTS); do $(B)/$$t $(B)/vectors.txt || exit 1; done

$(B)/vectors.txt: gen_vectors.py $(shell find vectors -name '*.json')
	@mkdir -p $(B)
	python3 -I gen_vectors.py $(shell find vectors -name '*.json' | sort) > $@

$(B)/%: %.cpp vectors.h $(KQ_O) $(VOICE_O) $(SODIUM_O) $(CTIDH_O)
	$(CXX) $(CXXFLAGS) $< $(KQ_O) $(VOICE_O) $(SODIUM_O) $(CTIDH_O) -o $@ $(LDLIBS)

# Decodes the voucher QR code with zbarimg (zbar-tools).
$(B)/test_qr: test_qr.cpp vectors.h $(QR)/kq_qr.h $(QR_O) $(KQ_O) $(SODIUM_O) $(CTIDH_O)
	$(CXX) $(CXXFLAGS) $< $(QR_O) $(KQ_O) $(SODIUM_O) $(CTIDH_O) -o $@ $(LDLIBS)

# The modem through a simulated room (fskv_room.c), and its WAV fixtures in vectors/fskv.
$(B)/test_fskv: test_fskv.cpp vectors.h fskv_room.h $(FSKV)/fskv.h $(FSKV_O) $(B)/fskv_room.o $(KQ_O) $(SODIUM_O) $(CTIDH_O)
	$(CXX) $(CXXFLAGS) $< $(FSKV_O) $(B)/fskv_room.o $(KQ_O) $(SODIUM_O) $(CTIDH_O) -o $@ $(LDLIBS)

$(B)/fskv_room.o: fskv_room.c fskv_room.h
	@mkdir -p $(B)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE -c $< -o $@

clean:
	rm -rf build build-*
.PHONY: all clean
