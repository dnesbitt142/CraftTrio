// Allocation/byte-equivalence probe for the old and updated GridCraft PDF writer.
// MIT OR Apache-2.0. This diagnostic is outside production crates: unsafe is
// restricted to forwarding the Rust GlobalAlloc interface to System.
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::hint::black_box;
use std::path::Path;

struct Tracking;
static LIVE: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);
fn added(size: usize) {
    let live = LIVE.fetch_add(size, Ordering::SeqCst) + size;
    PEAK.fetch_max(live, Ordering::SeqCst);
}
unsafe impl GlobalAlloc for Tracking {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let out = unsafe { System.alloc(layout) };
        if !out.is_null() { added(layout.size()); }
        out
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let out = unsafe { System.alloc_zeroed(layout) };
        if !out.is_null() { added(layout.size()); }
        out
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) };
        LIVE.fetch_sub(layout.size(), Ordering::SeqCst);
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let out = unsafe { System.realloc(ptr, layout, size) };
        if !out.is_null() {
            LIVE.fetch_sub(layout.size(), Ordering::SeqCst);
            added(size);
        }
        out
    }
}
#[global_allocator]
static ALLOCATOR: Tracking = Tracking;

macro_rules! document {
    ($writer:ident, $rgba:expr, $side:expr, $pages:expr) => {{
        let mut doc = $writer::PdfDoc::new();
        doc.info.title = "Export memory regression".into();
        doc.info.author = "Craft Trio".into();
        for i in 0..$pages {
            let page = doc.add_page(600.0, 400.0);
            page.text(10.0, 20.0, 12.0, $writer::Font::Helvetica, [0, 0, 0], &format!("Page {} Café", i + 1));
            page.fill_rect(10.0, 30.0, 200.0, 20.0, [20, 80, 140]);
            page.image_rgba($writer::Rect::new(10.0, 60.0, 300.0, 300.0), &$rgba, [$side, $side]);
        }
        doc.finish()
    }};
}

fn measured(run: impl FnOnce() -> Vec<u8>) -> (Vec<u8>, usize, usize) {
    let before = LIVE.load(Ordering::SeqCst);
    PEAK.store(before, Ordering::SeqCst);
    let result = black_box(run());
    let peak = PEAK.load(Ordering::SeqCst).saturating_sub(before);
    let retained = LIVE.load(Ordering::SeqCst).saturating_sub(before);
    (result, peak, retained)
}

fn main() {
    // Initialize stdio before recording allocation baselines.
    println!("GridCraft PDF allocation probe; requested bytes, not OS/2 process RSS");
    let out = std::env::args().nth(1).unwrap_or_else(|| ".".into());
    std::fs::create_dir_all(&out).unwrap();
    for (name, side, pages, complex) in [
        ("opaque-flat", 1024u32, 1usize, false),
        ("alpha-gradient", 1024, 1, true),
        ("multi-page", 512, 4, true),
    ] {
        let mut rgba = vec![255; side as usize * side as usize * 4];
        if complex {
            let mut state = 0x12345678u32;
            for (i, pixel) in rgba.chunks_exact_mut(4).enumerate() {
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                pixel.copy_from_slice(&[state as u8, (state >> 8) as u8, (state >> 16) as u8, (i % 251) as u8]);
            }
        }
        let (old, old_peak, old_retained) = measured(|| document!(baseline, rgba, side, pages));
        let (new, new_peak, new_retained) = measured(|| document!(candidate, rgba, side, pages));
        assert_eq!(new, old, "PDF bytes changed for {name}");
        assert!(new_peak < old_peak, "no peak-memory reduction for {name}: {new_peak} >= {old_peak}");
        std::fs::write(Path::new(&out).join(format!("{name}.pdf")), &new).unwrap();
        println!("PASS {name} bytes={} baseline_peak={old_peak} candidate_peak={new_peak} saved_peak={} baseline_output_capacity={old_retained} candidate_output_capacity={new_retained}", new.len(), old_peak - new_peak);
    }
    println!("PASS all PDF byte-equivalence and requested-allocation checks");
}
