set(KQ ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(HOST ${KQ}/test/host)
idf_component_register(
    SRCS voucher_main.cpp ${KQ}/src/kq1_voucher.cpp ${KQ}/src/kq_nike.cpp ${KQ}/src/kq_bacap.cpp
         ${KQ}/src/kq1_wire.cpp ${KQ}/src/kq_crypto.cpp ${KQ}/src/kq_json.cpp
    INCLUDE_DIRS ${KQ}/include ${HOST}
    PRIV_REQUIRES kq_ctidh kq_sodium mbedtls esp_timer esp_psram)
# The voucher vectors, flattened by gen_vectors.py as for the host tests.
set(V1 ${HOST}/vectors/voucher/voucher_vectors.json)
set(V2 ${HOST}/vectors/voucher/voucher_vectors_v2.json)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vectors.txt
    COMMAND python3 -I ${HOST}/gen_vectors.py ${V1} ${V2} > ${CMAKE_CURRENT_BINARY_DIR}/vectors.txt
    DEPENDS ${HOST}/gen_vectors.py ${V1} ${V2})
target_add_binary_data(${COMPONENT_LIB} ${CMAKE_CURRENT_BINARY_DIR}/vectors.txt TEXT)
