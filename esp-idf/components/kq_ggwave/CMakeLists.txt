# Voucher codes by sound: kq_ggwave.h, the C++ face of the Rust ggwave in components/kq_rust
# (the ggwave crate, katzenpost/ggwave-rs). docs/sound-vouchers.md has the whole of it.
idf_component_register(SRCS kq_ggwave.cpp INCLUDE_DIRS . REQUIRES kq_rust)
# -Os: the app slot has little room to spare.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
