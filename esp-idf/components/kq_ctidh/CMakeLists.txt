# CTIDH-1024 from codeberg.org/vula/highctidh v1.0.2026101300, portable fiat-crypto backend;
# fiat_p1024.h has a path without __int128 for 32-bit targets.
# KQ_CTIDH_FIELD (as in test/kq.mk, e.g. `idf.py -DKQ_CTIDH_FIELD=fiat`) picks the field:
# mont32 (fp1024_mont32.c, in IRAM, with fiat's four functions renamed *_ref) or fiat.
if(NOT KQ_CTIDH_FIELD)
    set(KQ_CTIDH_FIELD mont32)
endif()
file(GLOB CTIDH_SRCS highctidh/*.c)
if(KQ_CTIDH_FIELD STREQUAL "mont32")
    list(APPEND CTIDH_SRCS fp1024_mont32.c)
    set(CTIDH_LF linker.lf)
elseif(NOT KQ_CTIDH_FIELD STREQUAL "fiat")
    message(FATAL_ERROR "KQ_CTIDH_FIELD is mont32 or fiat, not ${KQ_CTIDH_FIELD}")
endif()
idf_component_register(
    SRCS ${CTIDH_SRCS} kq_randombytes.c
    INCLUDE_DIRS highctidh
    LDFRAGMENTS ${CTIDH_LF}
    PRIV_REQUIRES kq_sodium)
target_compile_definitions(${COMPONENT_LIB} PUBLIC BITS=1024 CGONUTS HIGHCTIDH_PORTABLE=1)
# Vendored code: its warnings are upstream's, and GCC's maybe-uninitialized misfires in poly.c.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fwrapv -Wno-error -Wno-maybe-uninitialized -Wno-unused-function -Wno-unused-variable)
if(KQ_CTIDH_FIELD STREQUAL "mont32")
    set_source_files_properties(highctidh/fiat_p1024.c PROPERTIES COMPILE_DEFINITIONS
        "fiat_p1024_mul=fiat_p1024_mul_ref;fiat_p1024_square=fiat_p1024_square_ref;fiat_p1024_add=fiat_p1024_add_ref;fiat_p1024_sub=fiat_p1024_sub_ref")
endif()
