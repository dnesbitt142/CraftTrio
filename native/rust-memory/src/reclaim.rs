//! Whole dead user pages can be decommitted because their backing allocations
//! REMAIN OWNED from System. Ledger and libc metadata are outside those pages.
//! Reuse recommits first; eviction recommits the entire original backing before
//! System is allowed to coalesce or write its free-list metadata.
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::UnsafeCell;
use std::hint::spin_loop;
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};

const PAGE: usize = 4096;
const LARGE: usize = 64 * 1024;
const SLOTS: usize = 128;
const REGISTER: u32 = 0;
const COMMIT: u32 = 1;
const DECOMMIT: u32 = 2;
const UNREGISTER: u32 = 3;

#[cfg(all(target_arch = "wasm32", target_os = "wasi"))]
#[link(wasm_import_module = "wc_host")]
unsafe extern "C" {
    fn memory_pages(operation: u32, address: usize, length: usize) -> u32;
}

fn pages(operation: u32, address: usize, length: usize) -> bool {
    if length == 0 { return true; }
    #[cfg(all(target_arch = "wasm32", target_os = "wasi", not(test)))]
    { unsafe { memory_pages(operation, address, length) == 0 } }
    #[cfg(test)]
    { unsafe { tests::page_operation(operation, address, length) } }
    #[cfg(not(any(test, all(target_arch = "wasm32", target_os = "wasi"))))]
    { let _ = (operation, address, length); false }
}

#[derive(Clone, Copy)]
struct Entry { address: usize, capacity: usize, alignment: usize, committed: usize, live: bool }
impl Entry {
    const EMPTY: Self = Self { address: 0, capacity: 0, alignment: 0, committed: 0, live: false };
}
struct State { entries: [Entry; SLOTS] }
impl State {
    const fn new() -> Self { Self { entries: [Entry::EMPTY; SLOTS] } }
    fn find(&self, address: usize) -> Option<usize> {
        self.entries.iter().position(|e| e.live && e.address == address)
    }
    fn resize_prefix(&mut self, index: usize, wanted: usize) -> bool {
        let old = self.entries[index].committed;
        let address = self.entries[index].address;
        if wanted > old {
            if !pages(COMMIT, address + old, wanted - old) { return false; }
        } else if wanted < old {
            // Failure to reclaim is harmless: keep those pages accessible.
            if !pages(DECOMMIT, address + wanted, old - wanted) { return true; }
        }
        self.entries[index].committed = wanted;
        true
    }
    unsafe fn evict_cached(&mut self) {
        for i in 0..SLOTS {
            let e = self.entries[i];
            if e.address == 0 || e.live { continue; }
            if !self.resize_prefix(i, e.capacity) { continue; }
            if !pages(UNREGISTER, e.address, e.capacity) { continue; }
            if let Ok(layout) = Layout::from_size_align(e.capacity, e.alignment) {
                unsafe { System.dealloc(e.address as *mut u8, layout); }
                self.entries[i] = Entry::EMPTY;
            }
        }
    }
}

/// User-page allocation totals. Only native hosts implementing actual page
/// decommit can interpret these as physical/committed-memory reclamation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReclaimStats {
    pub tracked_blocks: usize,
    pub active_blocks: usize,
    pub cached_blocks: usize,
    pub owned_bytes: usize,
    pub committed_bytes: usize,
    pub decommitted_bytes: usize,
}
pub struct ReclaimAllocator { busy: AtomicBool, state: UnsafeCell<State> }
// System and the native page hook never reenter this global allocator.
unsafe impl Sync for ReclaimAllocator {}
struct Guard<'a> { owner: &'a ReclaimAllocator }
impl std::ops::Deref for Guard<'_> {
    type Target = State;
    fn deref(&self) -> &State { unsafe { &*self.owner.state.get() } }
}
impl std::ops::DerefMut for Guard<'_> {
    fn deref_mut(&mut self) -> &mut State { unsafe { &mut *self.owner.state.get() } }
}
impl Drop for Guard<'_> {
    fn drop(&mut self) { self.owner.busy.store(false, Ordering::Release); }
}
impl Default for ReclaimAllocator { fn default() -> Self { Self::new() } }
impl ReclaimAllocator {
    pub const fn new() -> Self { Self { busy: AtomicBool::new(false), state: UnsafeCell::new(State::new()) } }
    fn lock(&self) -> Guard<'_> {
        while self.busy.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() { spin_loop(); }
        Guard { owner: self }
    }
    pub fn stats(&self) -> ReclaimStats {
        let state = self.lock();
        let mut result = ReclaimStats::default();
        for e in &state.entries {
            if e.address == 0 { continue; }
            result.tracked_blocks += 1;
            result.active_blocks += usize::from(e.live);
            result.cached_blocks += usize::from(!e.live);
            result.owned_bytes += e.capacity;
            result.committed_bytes += e.committed;
            result.decommitted_bytes += e.capacity - e.committed;
        }
        result
    }
    /// Best effort: recommit and return cached backings to System.
    pub fn trim(&self) { unsafe { self.lock().evict_cached(); } }
}
fn round_pages(size: usize) -> Option<usize> { size.checked_add(PAGE - 1).map(|n| n & !(PAGE - 1)) }

unsafe impl GlobalAlloc for ReclaimAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.size() < LARGE { return unsafe { System.alloc(layout) }; }
        let Some(capacity) = round_pages(layout.size()) else { return ptr::null_mut(); };
        let alignment = layout.align().max(PAGE);
        let Ok(backing) = Layout::from_size_align(capacity, alignment) else { return ptr::null_mut(); };
        let mut state = self.lock();
        let reusable = state.entries.iter().enumerate()
            .filter(|(_, e)| e.address != 0 && !e.live && e.capacity >= capacity && e.alignment >= layout.align())
            .min_by_key(|(_, e)| e.capacity).map(|(i, _)| i);
        if let Some(i) = reusable {
            if state.resize_prefix(i, capacity) {
                state.entries[i].live = true;
                return state.entries[i].address as *mut u8;
            }
        }
        if state.entries.iter().all(|e| e.address != 0) { unsafe { state.evict_cached(); } }
        let Some(index) = state.entries.iter().position(|e| e.address == 0) else {
            drop(state);
            return unsafe { System.alloc(layout) };
        };
        let mut allocation = unsafe { System.alloc(backing) };
        if allocation.is_null() {
            unsafe { state.evict_cached(); }
            allocation = unsafe { System.alloc(backing) };
        }
        if allocation.is_null() { return allocation; }
        if !pages(REGISTER, allocation as usize, capacity) {
            unsafe { System.dealloc(allocation, backing); }
            drop(state);
            return unsafe { System.alloc(layout) };
        }
        state.entries[index] = Entry { address: allocation as usize, capacity, alignment, committed: capacity, live: true };
        allocation
    }
    unsafe fn dealloc(&self, allocation: *mut u8, layout: Layout) {
        if layout.size() < LARGE { unsafe { System.dealloc(allocation, layout); } return; }
        let mut state = self.lock();
        if let Some(i) = state.find(allocation as usize) {
            let _ = state.resize_prefix(i, 0);
            state.entries[i].live = false;
            return;
        }
        drop(state);
        unsafe { System.dealloc(allocation, layout); }
    }
    unsafe fn realloc(&self, allocation: *mut u8, old: Layout, size: usize) -> *mut u8 {
        if size == 0 { return ptr::null_mut(); }
        if old.size() < LARGE && size < LARGE { return unsafe { System.realloc(allocation, old, size) }; }
        if old.size() >= LARGE && size >= LARGE {
            let mut state = self.lock();
            if let Some(i) = state.find(allocation as usize) {
                if let Some(wanted) = round_pages(size) {
                    if wanted <= state.entries[i].capacity {
                        return if state.resize_prefix(i, wanted) { allocation } else { ptr::null_mut() };
                    }
                }
            }
        }
        let Ok(new_layout) = Layout::from_size_align(size, old.align()) else { return ptr::null_mut(); };
        let replacement = unsafe { self.alloc(new_layout) };
        if !replacement.is_null() {
            unsafe {
                ptr::copy_nonoverlapping(allocation, replacement, old.size().min(size));
                self.dealloc(allocation, old);
            }
        }
        replacement
    }
}

#[cfg(test)]
#[path = "reclaim_tests.rs"]
mod tests;
