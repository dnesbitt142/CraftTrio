//! Independent protected-page stress checks for the production allocator.
//! Build this as a test binary and filter `independent_` to avoid running the
//! allocator author's separate global fault-injection test concurrently.
#[path = "../../native/rust-memory/src/reclaim.rs"]
mod reclaim;

use reclaim::ReclaimAllocator;
use std::alloc::{GlobalAlloc, Layout};
use std::ptr;
use std::sync::Arc;

struct Allocation {
    pointer: *mut u8,
    layout: Layout,
    marker: u8,
}

fn next(state: &mut u64) -> u64 {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    *state
}

unsafe fn verify(allocation: &Allocation) {
    let size = allocation.layout.size();
    assert_eq!(allocation.pointer as usize % allocation.layout.align(), 0);
    assert_eq!(unsafe { *allocation.pointer }, allocation.marker);
    assert_eq!(unsafe { *allocation.pointer.add(size - 1) }, allocation.marker);
    for page in (0..size).step_by(4096) {
        assert_eq!(unsafe { *allocation.pointer.add(page) }, allocation.marker);
        if page > 0 {
            assert_eq!(unsafe { *allocation.pointer.add(page - 1) }, allocation.marker);
        }
    }
}

fn exercise(allocator: &ReclaimAllocator, mut state: u64, steps: usize) {
    let sizes = [1, 31, 4095, 4096, 65535, 65536, 65537, 70000, 131073, 262143];
    let alignments = [1, 8, 16, 256, 4096, 65536];
    let mut slots: Vec<Option<Allocation>> = (0..16).map(|_| None).collect();
    for step in 0..steps {
        let selected = (next(&mut state) as usize) % slots.len();
        // Each thread owns its live blocks, while sharing the allocator ledger.
        // Inspect neighboring blocks before and after every shape transition.
        for item in slots.iter().flatten() {
            unsafe { verify(item) };
        }
        if let Some(mut item) = slots[selected].take() {
            if next(&mut state) % 3 == 0 {
                unsafe { allocator.dealloc(item.pointer, item.layout) };
            } else {
                let size = sizes[(next(&mut state) as usize) % sizes.len()];
                let grown = unsafe { allocator.realloc(item.pointer, item.layout, size) };
                if !grown.is_null() {
                    // Every byte in the preserved prefix, not just boundary
                    // samples, must survive grow/shrink and threshold crossings.
                    for i in 0..item.layout.size().min(size) {
                        assert_eq!(unsafe { *grown.add(i) }, item.marker);
                    }
                    item.pointer = grown;
                    item.layout = Layout::from_size_align(size, item.layout.align()).unwrap();
                } else {
                    // A failed allocation must leave ownership/content intact.
                    unsafe { verify(&item) };
                }
                item.marker = (step as u8).wrapping_add((state >> 32) as u8);
                unsafe { ptr::write_bytes(item.pointer, item.marker, item.layout.size()) };
                slots[selected] = Some(item);
            }
        } else {
            let size = sizes[(next(&mut state) as usize) % sizes.len()];
            let alignment = alignments[(next(&mut state) as usize) % alignments.len()];
            let layout = Layout::from_size_align(size, alignment).unwrap();
            let zeroed = step % 5 == 0;
            let pointer = unsafe {
                if zeroed { allocator.alloc_zeroed(layout) } else { allocator.alloc(layout) }
            };
            assert!(!pointer.is_null());
            if zeroed {
                for i in 0..size { assert_eq!(unsafe { *pointer.add(i) }, 0); }
            }
            let marker = (state as u8).wrapping_add(step as u8);
            unsafe { ptr::write_bytes(pointer, marker, size) };
            slots[selected] = Some(Allocation { pointer, layout, marker });
        }
    }
    for item in slots.into_iter().flatten() {
        unsafe {
            verify(&item);
            allocator.dealloc(item.pointer, item.layout);
        }
    }
}

#[test]
fn independent_randomized_alignment_and_realloc_lifecycle() {
    let allocator = ReclaimAllocator::new();
    exercise(&allocator, 0x4d454d4f52593231, 700);
    assert_eq!(allocator.stats().active_blocks, 0);
    assert!(allocator.stats().decommitted_bytes > 0);
    allocator.trim();
    assert_eq!(allocator.stats().tracked_blocks, 0);
}

#[test]
fn independent_shared_allocator_multithread_lifecycle() {
    let allocator = Arc::new(ReclaimAllocator::new());
    let workers: Vec<_> = (0..4).map(|worker| {
        let allocator = allocator.clone();
        std::thread::spawn(move || {
            exercise(&allocator, 0x43524146544d454d ^ (worker + 1) * 0x11111111, 350);
        })
    }).collect();
    for worker in workers { worker.join().unwrap(); }
    assert_eq!(allocator.stats().active_blocks, 0);
    allocator.trim();
    assert_eq!(allocator.stats().tracked_blocks, 0);
}
