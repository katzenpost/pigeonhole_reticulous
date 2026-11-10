# reticulous-core (Cargo.toml here), the firmware's one Rust library: no_std, built by cargo and
# linked as a static library, for the ESP32-S3 (xtensa-esp32s3-none-elf, with espup's esp
# toolchain) or for the host (the linux target). README.md has the toolchains.
idf_component_register(INCLUDE_DIRS include REQUIRES kq_ctidh)

# rustup's cargo, also where `spangap shell` installs it (the container's home persists).
find_program(KQ_CARGO cargo HINTS $ENV{CARGO_HOME}/bin $ENV{HOME}/.cargo/bin)
if(NOT KQ_CARGO)
    message(FATAL_ERROR "kq_rust: no cargo; components/kq_rust/README.md has the Rust toolchains")
endif()
set(cargo_dir ${CMAKE_CURRENT_BINARY_DIR}/cargo)
if(IDF_TARGET STREQUAL "esp32s3")
    set(triple xtensa-esp32s3-none-elf)
    # Xtensa is not in upstream Rust: core and alloc are built from the esp toolchain's rust-src.
    set(cargo_args +esp build --target ${triple} -Zbuild-std=core,alloc)
    set(lib ${cargo_dir}/${triple}/release/libreticulous_core.a)
elseif(IDF_TARGET STREQUAL "linux")
    # shortcut: the host's own architecture only; a cross-built Linux station needs --target.
    set(cargo_args build)
    set(lib ${cargo_dir}/release/libreticulous_core.a)
else()
    message(FATAL_ERROR "kq_rust: no Rust target for ${IDF_TARGET}")
endif()

file(GLOB rust_srcs ${COMPONENT_DIR}/src/*.rs)
add_custom_command(OUTPUT ${lib}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${cargo_dir}
        ${KQ_CARGO} ${cargo_args} --release --locked --manifest-path ${COMPONENT_DIR}/Cargo.toml
    DEPENDS ${COMPONENT_DIR}/Cargo.toml ${COMPONENT_DIR}/Cargo.lock ${rust_srcs}
    USES_TERMINAL
    COMMENT "cargo: reticulous-core for ${IDF_TARGET}")
add_custom_target(kq_rust_cargo DEPENDS ${lib})
# highctidh-rs's `external` leaves CTIDH to the final link: kq_ctidh, linked after the library.
add_prebuilt_library(reticulous_core ${lib} REQUIRES kq_ctidh)
add_dependencies(reticulous_core kq_rust_cargo)
target_link_libraries(${COMPONENT_LIB} INTERFACE reticulous_core)
