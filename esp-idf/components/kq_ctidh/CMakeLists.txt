# CTIDH-1024 from codeberg.org/vula/highctidh v1.0.2026101300, portable fiat-crypto backend;
# fiat_p1024.h has a path without __int128 for 32-bit targets.
file(GLOB CTIDH_SRCS highctidh/*.c)
idf_component_register(
    SRCS ${CTIDH_SRCS} kq_randombytes.c
    INCLUDE_DIRS highctidh
    PRIV_REQUIRES kq_sodium)
target_compile_definitions(${COMPONENT_LIB} PUBLIC BITS=1024 CGONUTS HIGHCTIDH_PORTABLE=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fwrapv -Wno-unused-function -Wno-unused-variable)
