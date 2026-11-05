# CTIDH-1024 from codeberg.org/vula/highctidh v1.0.2026101300, portable fiat-crypto backend;
# fiat_p1024.h has a path without __int128 for 32-bit targets.
# KQ_CTIDH_FIELD (as in test/kq.mk, e.g. `idf.py -DKQ_CTIDH_FIELD=fiat`) picks the field:
# xtensa (fp1024_xtensa.S, MAC16 assembly, the default on the ESP32-S3), mont32
# (fp1024_mont32.c, the default on other targets) or fiat. xtensa and mont32 run from IRAM and
# keep fiat's four functions beside them, renamed *_ref.
# The value counts for the configure run it is given to: kept in the CMake cache, a test's field
# would stay in every later build of the tree, T-Deck images too.
set(field "${KQ_CTIDH_FIELD}")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    unset(KQ_CTIDH_FIELD CACHE)
endif()
if(NOT field)
    if(IDF_TARGET STREQUAL "esp32s3")
        set(field xtensa)
    else()
        set(field mont32)
    endif()
endif()
file(GLOB CTIDH_SRCS highctidh/*.c)
if(field STREQUAL "mont32")
    list(APPEND CTIDH_SRCS fp1024_mont32.c)
    set(CTIDH_LF linker.lf)
elseif(field STREQUAL "xtensa")
    if(NOT IDF_TARGET STREQUAL "esp32s3")
        message(FATAL_ERROR "KQ_CTIDH_FIELD=xtensa is assembly for the ESP32-S3, not ${IDF_TARGET}")
    endif()
    list(APPEND CTIDH_SRCS fp1024_xtensa.S)
    set(CTIDH_LF linker_xtensa.lf)
elseif(NOT field STREQUAL "fiat")
    message(FATAL_ERROR "KQ_CTIDH_FIELD is xtensa, mont32 or fiat, not ${field}")
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(IDF_TARGET STREQUAL "esp32s3" AND NOT field STREQUAL "xtensa")
        message(WARNING "kq_ctidh: the ${field} CTIDH field on the ESP32-S3, not xtensa")
    else()
        message(STATUS "kq_ctidh: the ${field} CTIDH field")
    endif()
endif()
idf_component_register(
    SRCS ${CTIDH_SRCS} kq_randombytes.c
    INCLUDE_DIRS highctidh
    LDFRAGMENTS ${CTIDH_LF}
    PRIV_REQUIRES kq_sodium)
target_compile_definitions(${COMPONENT_LIB} PUBLIC BITS=1024 CGONUTS HIGHCTIDH_PORTABLE=1)
# Vendored code: its warnings are upstream's, and GCC's maybe-uninitialized misfires in poly.c.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fwrapv -Wno-error -Wno-maybe-uninitialized -Wno-unused-function -Wno-unused-variable)
if(NOT field STREQUAL "fiat")
    set_source_files_properties(highctidh/fiat_p1024.c PROPERTIES COMPILE_DEFINITIONS
        "fiat_p1024_mul=fiat_p1024_mul_ref;fiat_p1024_square=fiat_p1024_square_ref;fiat_p1024_add=fiat_p1024_add_ref;fiat_p1024_sub=fiat_p1024_sub_ref")
endif()
