# The client's sources, libsodium and CTIDH as host objects, for the host tests and the screen
# simulator. Included from a directory two below esp-idf/.
ROOT := ../..
SODIUM := $(ROOT)/components/kq_sodium
SODIUM_SRC := $(SODIUM)/libsodium/src/libsodium
SODIUM_C := $(patsubst %,$(SODIUM_SRC)/%,$(shell sed -n 's|.*"$${SRC}/\(.*\.c\)".*|\1|p' $(SODIUM)/CMakeLists.txt)) \
	$(SODIUM_SRC)/randombytes/internal/randombytes_internal_random.c \
	$(SODIUM_SRC)/randombytes/sysrandom/randombytes_sysrandom.c
B := $(if $(filter 0,$(INT128)),build-noint128,build)
SODIUM_O := $(patsubst $(SODIUM_SRC)/%.c,$(B)/sodium/%.o,$(SODIUM_C))
CTIDH := $(ROOT)/components/kq_ctidh
CTIDH_C := $(wildcard $(CTIDH)/highctidh/*.c) $(CTIDH)/kq_randombytes.c
CTIDH_O := $(patsubst $(CTIDH)/%.c,$(B)/ctidh/%.o,$(CTIDH_C))
CTIDH_DEFS := -DBITS=1024 -DCGONUTS -DHIGHCTIDH_PORTABLE=1
# INT128=0 builds the fiat field without __int128, as on 32-bit targets.
ifeq ($(INT128),0)
CTIDH_DEFS += -DFIAT_P1024_NO_INT128
endif
KQ_SRC := $(filter-out $(ROOT)/src/kq_service.cpp,$(wildcard $(ROOT)/src/*.cpp))
KQ_O := $(patsubst $(ROOT)/src/%.cpp,$(B)/kq/%.o,$(KQ_SRC))

ifdef MBEDTLS_DIR
MBED_INC := -I$(MBEDTLS_DIR)/include
MBED_LIB := -L$(MBEDTLS_DIR)/lib
endif

CFLAGS := -O2 -g -DCONFIGURED -DNATIVE_LITTLE_ENDIAN -DHAVE_WEAK_SYMBOLS -D__STDC_LIMIT_MACROS \
	-D__STDC_CONSTANT_MACROS -I$(SODIUM_SRC)/include -I$(SODIUM_SRC)/include/sodium -w
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	-I$(ROOT)/include -I$(SODIUM_SRC)/include -I$(CTIDH)/highctidh $(CTIDH_DEFS) $(MBED_INC)
LDLIBS := $(MBED_LIB) -lmbedcrypto -fsanitize=address,undefined

$(B)/sodium/%.o: $(SODIUM_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/ctidh/%.o: $(CTIDH)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -fwrapv $(CTIDH_DEFS) -I$(CTIDH)/highctidh -I$(SODIUM_SRC)/include -w -c $< -o $@

$(B)/kq/%.o: $(ROOT)/src/%.cpp $(wildcard $(ROOT)/include/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
