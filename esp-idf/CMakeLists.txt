include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
file(GLOB KQ_SRCS "src/*.cpp")
idf_component_register(
    SRCS ${KQ_SRCS} ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES ${SPANGAP_REQUIRES} mbedtls kq_sodium kq_ctidh kq_qrcode kq_ggwave esp_timer
)
