# kq_rust

`kq-rust`, the Rust half of Group Chat and Radio: one `#![no_std]` static library, with
`panic = "abort"` and an allocator over ESP-IDF's `heap_caps_malloc`/`heap_caps_free` (PSRAM first,
then internal RAM). The C/C++ shell calls it through `include/kq_rust.h`, and the same code builds
for the host tests and the screen simulator.

So far it holds:

- `src/ggwave.rs`: voucher codes by sound (`docs/sound-vouchers.md`), over the
  [ggwave crate](https://github.com/katzenpost/ggwave-rs). `components/kq_ggwave` is its C++ face.

`Cargo.toml` pins the hushphone crates by git rev, each on its repo's main: ggwave-rs,
pigeonhole_protocols, hpqc and highctidh-rs (the rev hpqc pins). highctidh-rs is built with
`external`, so it compiles no C, and the firmware's link takes CTIDH from `components/kq_ctidh`.

## Toolchains

**The host** (`test/host`, `test/lcd-sim`, and an ESP-IDF `linux` build) needs stable Rust 1.89 or
later, from [rustup](https://rustup.rs). `test/kq.mk` runs cargo into `build/rust`, and the host tests
run `cargo test` as well.

**The ESP32-S3** needs the `esp` toolchain from [espup](https://github.com/esp-rs/espup). Xtensa is not
in upstream Rust, so `core` and `alloc` are built from that toolchain's `rust-src`:

```sh
cargo install espup --locked
espup install --targets esp32s3
```

The component runs
`cargo +esp build --target xtensa-esp32s3-none-elf -Zbuild-std=core,alloc --release`, and ESP-IDF's
GCC links the `.a`. Nothing here needs espup's `export-esp.sh`, its GCC or its libclang.

**In the spangap build container:** the image (`spangap/build-system/Dockerfile`, in the spangap
repo) has no Rust yet. The container's home is the persistent `~/.spangap` mount, so a toolchain
installed from `spangap shell` stays installed when the image is rebuilt, and CMake finds cargo in
`~/.cargo/bin` without a PATH change:

```sh
spangap shell
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
. ~/.cargo/env
cargo install espup --locked
espup install --targets esp32s3
```

After that, `spangap build` builds the library with the rest of the image.
