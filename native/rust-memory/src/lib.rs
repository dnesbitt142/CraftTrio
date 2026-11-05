//! Memory policy for the single-threaded Craft OS/2 bridge engines.
//! Large allocations remain owned by this allocator while idle pages are
//! returned to the native OS. Allocator metadata is never decommitted.

pub mod buffers;
mod reclaim;
pub use reclaim::{ReclaimAllocator, ReclaimStats};
