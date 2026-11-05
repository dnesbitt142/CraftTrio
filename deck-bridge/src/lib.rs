#[cfg(target_os = "wasi")]
mod a11y;
#[cfg(target_os = "wasi")]
mod clipboard;
#[cfg(target_os = "wasi")]
mod file_bridge;
#[cfg(target_os = "wasi")]
mod wasi;

// The allocator keeps its page ledger outside reclaimed user allocations.
#[cfg(target_os = "wasi")]
#[global_allocator]
static OS2_ALLOCATOR: craft_memory::ReclaimAllocator = craft_memory::ReclaimAllocator::new();
