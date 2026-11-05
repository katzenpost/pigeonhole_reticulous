# test/host/test_voucher.cpp on the ESP32-S3 with the firmware's CTIDH field, in QEMU (Makefile).
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voucher_s3)
