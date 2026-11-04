# Benchmarks: the client's operations timed on this machine, slowest first, as Markdown.
#   make [INT128=0] [MBEDTLS_DIR=<mbedTLS 3.6 prefix>] run
# INT128=0 builds CTIDH's field without __int128, as on the 32-bit device.
include ../kq.mk

# Optimized and without the tests' sanitizers, so that the times are of the code.
CXXFLAGS := -std=c++17 -O2 -DNDEBUG -I$(ROOT)/include -I$(SODIUM_SRC)/include -I$(CTIDH)/highctidh $(CTIDH_DEFS) $(MBED_INC)
LDLIBS := $(MBED_LIB) -lmbedcrypto

$(B)/bench: bench.cpp $(KQ_O) $(SODIUM_O) $(CTIDH_O)
	$(CXX) $(CXXFLAGS) $< $(KQ_O) $(SODIUM_O) $(CTIDH_O) -o $@ $(LDLIBS)

bench-bin: $(B)/bench

# The tables on stdout; the progress on stderr.
run: $(B)/bench
	$(B)/bench

clean:
	rm -rf build build-noint128
.PHONY: bench-bin run clean
