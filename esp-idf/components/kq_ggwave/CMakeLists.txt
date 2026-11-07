# ggwave (github.com/ggerganov/ggwave 060aec73dd7123ccac200442f75bdc7369795ffe), MIT: include/,
# src/ and LICENSE as upstream has them but for the lines marked "kq": kMaxRecordedFrames cut from
# 2048 frames to 448 (3.3 MB less PSRAM each to send and to hear), the receiver recording no more
# and the sender refusing a longer message, and prepare() failing when its calloc does.
# kq_ggwave.* is ours; docs/sound-vouchers.md has the whole of it.
idf_component_register(SRCS src/ggwave.cpp kq_ggwave.cpp INCLUDE_DIRS . include)
# -Os and no log: the app slot has little room to spare.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
set_source_files_properties(src/ggwave.cpp PROPERTIES COMPILE_OPTIONS "-w;-DGGWAVE_DISABLE_LOG")
