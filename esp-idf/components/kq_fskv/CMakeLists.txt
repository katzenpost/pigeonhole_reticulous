# The acoustic voucher modem (fskv.h).
idf_component_register(SRCS fskv.c INCLUDE_DIRS .)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
