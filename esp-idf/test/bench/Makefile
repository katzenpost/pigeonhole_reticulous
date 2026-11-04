# Benchmarks: the client's operations timed on this machine, slowest first, as Markdown, and
# the field operations (mul, square, add, sub) each CTIDH-1024 action makes.
#   make [INT128=0] [KQ_CTIDH_FIELD=mont32|fiat] [MBEDTLS_DIR=<mbedTLS 3.6 prefix>] [BENCH_ARGS=<names>] run
# INT128=0 builds CTIDH's field without __int128, as on the 32-bit device.
# BENCH_ARGS: comma-separated name prefixes to run alone, e.g. field-mul or field-mul-mont32,nike-keypair;
# `$(B)/bench --list` prints the names. Timing runs go through the shared queue, not by hand:
#   bench-runner/submit.sh <task> <commit> [INT128=0|1] [KQ_CTIDH_FIELD=…] ONLY=field-mul
# To add a primitive: one bench("<section>: <what> (<variant>)", [&] { x = op(x, y); }) line in
# bench.cpp, in a section(...) block if it needs setup; chain its result into its next input
# so the compiler cannot hoist or drop the calls.
include ../kq.mk

# Optimized and without the tests' sanitizers, so that the times are of the code.
CXXFLAGS := -std=c++17 -O2 -DNDEBUG -I$(ROOT)/include -I$(SODIUM_SRC)/include -I$(CTIDH)/highctidh $(CTIDH_DEFS) $(MBED_INC) \
	-DKQ_FIELD_NAME='"$(KQ_CTIDH_FIELD)"' $(if $(FIAT_REF),-DKQ_FIELD_REF)
# CTIDH's calls into the field go through counters in bench.cpp.
WRAP := -Wl,--wrap=fiat_p1024_mul,--wrap=fiat_p1024_square,--wrap=fiat_p1024_add,--wrap=fiat_p1024_sub
LDLIBS := $(MBED_LIB) -lmbedcrypto

$(B)/bench: bench.cpp $(KQ_O) $(SODIUM_O) $(CTIDH_O)
	$(CXX) $(CXXFLAGS) $< $(KQ_O) $(SODIUM_O) $(CTIDH_O) -o $@ $(WRAP) $(LDLIBS)

bench-bin: $(B)/bench

# The tables on stdout; the progress on stderr.
run: $(B)/bench
	$(B)/bench $(BENCH_ARGS)

clean:
	rm -rf build build-noint128 build-fiat build-noint128-fiat
.PHONY: bench-bin run clean
