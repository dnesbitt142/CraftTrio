use super::*;
use std::sync::atomic::{AtomicUsize, Ordering};

static FAIL: AtomicUsize = AtomicUsize::new(usize::MAX);
static COMMITS: AtomicUsize = AtomicUsize::new(0);
static DECOMMITS: AtomicUsize = AtomicUsize::new(0);
#[cfg(unix)]
unsafe extern "C" {
    fn mprotect(address: *mut std::ffi::c_void, length: usize, protection: i32) -> i32;
    fn madvise(address: *mut std::ffi::c_void, length: usize, advice: i32) -> i32;
}
pub unsafe fn page_operation(operation: u32, address: usize, length: usize) -> bool {
    assert_eq!(address % PAGE, 0);
    assert_eq!(length % PAGE, 0);
    assert!(length > 0);
    if FAIL.load(Ordering::Relaxed) == operation as usize { return false; }
    #[cfg(unix)]
    match operation {
        COMMIT => {
            if unsafe { mprotect(address as *mut _, length, 3) } != 0 { return false; }
            unsafe { ptr::write_bytes(address as *mut u8, 0, length); }
            COMMITS.fetch_add(1, Ordering::Relaxed);
        }
        DECOMMIT => {
            if unsafe { madvise(address as *mut _, length, 4) } != 0 { return false; }
            if unsafe { mprotect(address as *mut _, length, 0) } != 0 { return false; }
            DECOMMITS.fetch_add(1, Ordering::Relaxed);
        }
        _ => {}
    }
    true
}

#[test]
fn protected_page_lifecycle_and_failures() {
    let allocator = ReclaimAllocator::new();
    let layout = Layout::from_size_align(131073, 16).unwrap();
    unsafe {
        let p = allocator.alloc(layout);
        assert!(!p.is_null());
        assert_eq!(p as usize % PAGE, 0);
        ptr::write_bytes(p, 0x5a, layout.size());
        assert_eq!(allocator.stats().committed_bytes, 135168);
        allocator.dealloc(p, layout);
        assert_eq!(allocator.stats().decommitted_bytes, 135168);
        let small = Layout::from_size_align(65537, 16).unwrap();
        let q = allocator.alloc_zeroed(small);
        assert_eq!(p, q);
        assert!((0..small.size()).all(|i| *q.add(i) == 0));
        assert_eq!(allocator.stats().committed_bytes, 69632);
        ptr::write_bytes(q, 0x33, small.size());
        let grown = allocator.realloc(q, small, 120000);
        assert_eq!(q, grown);
        assert!((0..small.size()).all(|i| *grown.add(i) == 0x33));
        ptr::write_bytes(grown, 0x77, 120000);
        let grown_layout = Layout::from_size_align(120000, 16).unwrap();
        let shrunk = allocator.realloc(grown, grown_layout, 70000);
        assert_eq!(grown, shrunk);
        assert_eq!(*shrunk.add(69999), 0x77);
        assert_eq!(allocator.stats().committed_bytes, 73728);
        FAIL.store(DECOMMIT as usize, Ordering::Relaxed);
        let shrunk_layout = Layout::from_size_align(70000, 16).unwrap();
        allocator.dealloc(shrunk, shrunk_layout);
        assert_eq!(allocator.stats().committed_bytes, 73728);
        FAIL.store(usize::MAX, Ordering::Relaxed);
        let r = allocator.alloc(small);
        assert_eq!(r, p);
        ptr::write_bytes(r, 0x44, small.size());
        let tiny = allocator.realloc(r, small, 31);
        assert!(!tiny.is_null());
        assert!((0..31).all(|i| *tiny.add(i) == 0x44));
        allocator.dealloc(tiny, Layout::from_size_align(31, 16).unwrap());
        assert_eq!(allocator.stats().committed_bytes, 0);
        allocator.trim();
        assert_eq!(allocator.stats().tracked_blocks, 0);
        FAIL.store(REGISTER as usize, Ordering::Relaxed);
        let fallback = allocator.alloc(layout);
        assert!(!fallback.is_null());
        ptr::write_bytes(fallback, 0x22, layout.size());
        assert_eq!(allocator.stats().tracked_blocks, 0);
        allocator.dealloc(fallback, layout);
        FAIL.store(usize::MAX, Ordering::Relaxed);

        // Larger alignment, a full ledger, and a normal-System fallback must
        // preserve ownership and exactly balance every original layout.
        let aligned = Layout::from_size_align(65536, 65536).unwrap();
        let high = allocator.alloc(aligned);
        assert!(!high.is_null());
        assert_eq!(high as usize % 65536, 0);
        ptr::write_bytes(high, 0x61, aligned.size());
        allocator.dealloc(high, aligned);
        allocator.trim();
        let normal = Layout::from_size_align(65536, 8).unwrap();
        let mut allocations = [ptr::null_mut(); SLOTS + 1];
        for (i, slot) in allocations.iter_mut().enumerate() {
            *slot = allocator.alloc(normal);
            assert!(!slot.is_null());
            ptr::write_bytes(*slot, i as u8, normal.size());
        }
        assert_eq!(allocator.stats().active_blocks, SLOTS);
        for (i, slot) in allocations.iter().enumerate() {
            assert_eq!(**slot, i as u8);
            assert_eq!(*(*slot).add(normal.size() - 1), i as u8);
            allocator.dealloc(*slot, normal);
        }
        assert_eq!(allocator.stats().cached_blocks, SLOTS);
        // All cached slots can be safely recommitted/evicted to serve a new
        // shape; no freed pages are passed straight to libc's free lists.
        let bigger = Layout::from_size_align(262144, 8).unwrap();
        let large = allocator.alloc(bigger);
        assert!(!large.is_null());
        ptr::write_bytes(large, 0x9c, bigger.size());
        assert_eq!(allocator.stats().active_blocks, 1);
        allocator.dealloc(large, bigger);
        FAIL.store(COMMIT as usize, Ordering::Relaxed);
        allocator.trim();
        assert!(allocator.stats().cached_blocks > 0);
        FAIL.store(usize::MAX, Ordering::Relaxed);
        allocator.trim();
        assert_eq!(allocator.stats().tracked_blocks, 0);
    }
    assert!(COMMITS.load(Ordering::Relaxed) > 0);
    assert!(DECOMMITS.load(Ordering::Relaxed) > 0);
}
