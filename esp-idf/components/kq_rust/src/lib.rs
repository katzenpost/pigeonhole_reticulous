// SPDX-License-Identifier: AGPL-3.0-only
//! kq-rust: the Rust half of the T-Deck apps, one static library behind the C ABI in
//! include/kq_rust.h. The same code builds for the ESP32-S3 and for the host tests.
#![cfg_attr(not(test), no_std)]

extern crate alloc;

mod ggwave;

#[cfg(not(test))]
mod rt {
    use core::alloc::{GlobalAlloc, Layout};
    use core::ffi::c_void;

    unsafe extern "C" {
        fn abort() -> !;
    }

    // No unwinding: a panic resets the device, as abort() does in C.
    #[panic_handler]
    fn panic(_: &core::panic::PanicInfo) -> ! {
        unsafe { abort() }
    }

    #[cfg(target_arch = "xtensa")]
    mod heap {
        use super::c_void;
        const MALLOC_CAP_8BIT: u32 = 1 << 2;
        const MALLOC_CAP_SPIRAM: u32 = 1 << 10;
        unsafe extern "C" {
            fn heap_caps_malloc(size: usize, caps: u32) -> *mut c_void;
            fn heap_caps_aligned_alloc(align: usize, size: usize, caps: u32) -> *mut c_void;
            pub fn heap_caps_free(p: *mut c_void);
        }
        // PSRAM first, then internal RAM, as the firmware's malloc does (SPIRAM_MALLOC_ALWAYSINTERNAL 0).
        pub unsafe fn alloc(size: usize, align: usize) -> *mut c_void {
            let get = |caps| unsafe {
                if align <= 4 { heap_caps_malloc(size, caps) } else { heap_caps_aligned_alloc(align, size, caps) }
            };
            let p = get(MALLOC_CAP_SPIRAM);
            if p.is_null() { get(MALLOC_CAP_8BIT) } else { p }
        }
        pub unsafe fn free(p: *mut c_void) {
            unsafe { heap_caps_free(p) }
        }
    }

    #[cfg(not(target_arch = "xtensa"))]
    mod heap {
        use super::c_void;
        unsafe extern "C" {
            fn malloc(size: usize) -> *mut c_void;
            fn aligned_alloc(align: usize, size: usize) -> *mut c_void;
            pub fn free(p: *mut c_void);
        }
        pub unsafe fn alloc(size: usize, align: usize) -> *mut c_void {
            unsafe { if align <= 16 { malloc(size) } else { aligned_alloc(align, size.next_multiple_of(align)) } }
        }
    }

    struct Heap;

    unsafe impl GlobalAlloc for Heap {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            unsafe { heap::alloc(l.size(), l.align()) as *mut u8 }
        }
        unsafe fn dealloc(&self, p: *mut u8, _: Layout) {
            unsafe { heap::free(p as *mut c_void) }
        }
    }

    #[global_allocator]
    static HEAP: Heap = Heap;
}
