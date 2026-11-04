# Host tests: plain g++ against the same sources the firmware builds.
# MBEDTLS_DIR names an mbedTLS 3.6 install prefix when it is not a system package.
include ../kq.mk

TESTS := test_bacap test_nike test_wire test_kq1 test_voucher test_replica test_member test_client test_runtime test_store

all: $(addprefix $(B)/,$(TESTS)) $(B)/vectors.txt
	@for t in $(TESTS); do $(B)/$$t $(B)/vectors.txt || exit 1; done

$(B)/vectors.txt: gen_vectors.py $(shell find vectors -name '*.json')
	@mkdir -p $(B)
	python3 -I gen_vectors.py $(shell find vectors -name '*.json' | sort) > $@

$(B)/%: %.cpp vectors.h $(KQ_O) $(SODIUM_O) $(CTIDH_O)
	$(CXX) $(CXXFLAGS) $< $(KQ_O) $(SODIUM_O) $(CTIDH_O) -o $@ $(LDLIBS)

clean:
	rm -rf build build-noint128
.PHONY: all clean
