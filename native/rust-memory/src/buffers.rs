//! Shared capacity policy for the three OS/2 editor bridges.
//!
//! Call drawing preparation only at the next frame, after the host consumed
//! the preceding packet. No capacity change occurs in draw_ptr/draw_len.

const DRAW_SLACK: usize = 256 << 10;
const CONSUMED_SLACK: usize = 64 << 10;
const TRIM_EXCESS: usize = 1 << 20;

/// Empty a previous draw packet and trim a grossly oversized retained buffer.
/// Optional compaction is fallible: allocation failure keeps reusable storage.
/// Exact reservation avoids amortized growth past the packet's own size cap.
pub fn prepare_draw(out: &mut Vec<u8>, bytes: usize) -> Option<()> {
    out.clear();
    trim_empty(out, bytes.max(DRAW_SLACK));
    out.try_reserve_exact(bytes).ok()?;
    Some(())
}

/// Release a consumed packet's one-shot texture-upload capacity immediately,
/// including when the editor becomes idle and no subsequent frame is needed.
/// Mesh-only bytes estimate the next frame, preventing repeated grow/trim for
/// complex documents. Optional compaction failure keeps the usable allocation.
pub fn consume_draw(out: &mut Vec<u8>, mesh_bytes: usize) {
    out.clear();
    trim_empty(out, mesh_bytes.max(CONSUMED_SLACK));
}

/// Reserve caller-owned input. Invalid/failed requests release prior capacity.
/// The returned address remains valid until the next reserve or accept call.
pub fn reserve_input(input: &mut Vec<u8>, len: usize, limit: usize) -> Option<usize> {
    if len == 0 || len > limit {
        *input = Vec::new();
        return None;
    }
    input.clear();
    trim_empty(input, len);
    if input.try_reserve_exact(len).is_err() {
        *input = Vec::new();
        return None;
    }
    input.resize(len, 0);
    Some(input.as_mut_ptr() as usize)
}

fn trim_empty(buffer: &mut Vec<u8>, target: usize) {
    let capacity = buffer.capacity();
    if capacity <= target.saturating_mul(2) || capacity.saturating_sub(target) < TRIM_EXCESS {
        return;
    }
    let mut smaller = Vec::new();
    if smaller.try_reserve_exact(target).is_ok() {
        *buffer = smaller;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_large_texture_packet_is_not_retained_for_small_frames() {
        let mut draw = vec![0x55; 8 << 20];
        prepare_draw(&mut draw, 1024).unwrap();
        assert_eq!(draw.len(), 0);
        assert!(draw.capacity() >= 1024);
        assert!(draw.capacity() <= DRAW_SLACK * 2);
        draw.extend_from_slice(&[1, 2, 3]);
        let pointer = draw.as_ptr();
        // Querying a completed packet does not mutate or trim it.
        assert_eq!(draw.as_slice(), &[1, 2, 3]);
        assert_eq!(draw.as_ptr(), pointer);
    }

    #[test]
    fn normal_sized_draw_packets_reuse_their_storage() {
        let mut draw = Vec::with_capacity(512 << 10);
        draw.extend_from_slice(&[1, 2, 3]);
        let pointer = draw.as_ptr();
        prepare_draw(&mut draw, 400 << 10).unwrap();
        assert_eq!(draw.as_ptr(), pointer);
        assert_eq!(draw.capacity(), 512 << 10);
        assert!(draw.is_empty());
    }

    #[test]
    fn consumed_texture_upload_releases_peak_without_waiting_for_input() {
        let mut draw = vec![0x55; 8 << 20];
        consume_draw(&mut draw, 32 << 10);
        assert!(draw.is_empty());
        assert!(draw.capacity() <= CONSUMED_SLACK * 2);
    }

    #[test]
    fn consumed_complex_mesh_retains_its_reusable_budget() {
        let mut draw = vec![0x55; 8 << 20];
        let pointer = draw.as_ptr();
        consume_draw(&mut draw, 6 << 20);
        assert!(draw.is_empty());
        assert_eq!(draw.as_ptr(), pointer);
        assert_eq!(draw.capacity(), 8 << 20);
    }

    #[test]
    fn exact_reservation_handles_big_packets_and_capacity_overflow() {
        let mut draw = Vec::new();
        prepare_draw(&mut draw, 3 << 20).unwrap();
        assert!(draw.capacity() >= 3 << 20);
        assert!(draw.capacity() < 4 << 20);
        assert_eq!(prepare_draw(&mut draw, usize::MAX), None);
    }

    #[test]
    fn invalid_or_cancelled_input_drops_old_storage() {
        let mut input = vec![7; 2 << 20];
        assert_eq!(reserve_input(&mut input, 0, 16 << 20), None);
        assert_eq!(input.capacity(), 0);
        input = vec![7; 2 << 20];
        assert_eq!(reserve_input(&mut input, 17 << 20, 16 << 20), None);
        assert_eq!(input.capacity(), 0);
    }

    #[test]
    fn smaller_input_trims_and_success_is_zero_initialized() {
        let mut input = vec![7; 4 << 20];
        let pointer = reserve_input(&mut input, 4096, 16 << 20).unwrap();
        assert_eq!(pointer, input.as_mut_ptr() as usize);
        assert_eq!(input.len(), 4096);
        assert!(input.capacity() < 1 << 20);
        assert!(input.iter().all(|b| *b == 0));
    }
}
