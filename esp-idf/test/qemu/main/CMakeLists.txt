# KQ_CTIDH_FIELD picks the field as test/kq.mk does: mont32 (with fiat's four functions beside
# it as *_ref) or fiat. The field's sources get components/kq_ctidh's flags; IDF adds the
# T-Deck build's -mlongcalls -ffunction-sections -fdata-sections.
set(CTIDH ${CMAKE_CURRENT_LIST_DIR}/../../../components/kq_ctidh)
set(FIELD_SRCS ${CTIDH}/highctidh/fiat_p1024.c)
if(KQ_CTIDH_FIELD STREQUAL "mont32")
    list(APPEND FIELD_SRCS ${CTIDH}/fp1024_mont32.c)
    set_source_files_properties(${CTIDH}/highctidh/fiat_p1024.c PROPERTIES COMPILE_DEFINITIONS
        "fiat_p1024_mul=fiat_p1024_mul_ref;fiat_p1024_square=fiat_p1024_square_ref;fiat_p1024_add=fiat_p1024_add_ref;fiat_p1024_sub=fiat_p1024_sub_ref")
elseif(NOT KQ_CTIDH_FIELD STREQUAL "fiat" AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    message(FATAL_ERROR "KQ_CTIDH_FIELD is mont32 or fiat, not '${KQ_CTIDH_FIELD}'")
endif()
idf_component_register(SRCS field_count.c ${FIELD_SRCS} INCLUDE_DIRS ${CTIDH}/highctidh)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BITS=1024 CGONUTS HIGHCTIDH_PORTABLE=1
    KQ_FIELD_NAME="${KQ_CTIDH_FIELD}" $<$<STREQUAL:${KQ_CTIDH_FIELD},mont32>:KQ_FIELD_REF>)
set_source_files_properties(${FIELD_SRCS} PROPERTIES COMPILE_OPTIONS
    "-O2;-fwrapv;-Wno-error;-Wno-maybe-uninitialized;-Wno-unused-function;-Wno-unused-variable")
