use deckcraft_engine::Session;
use deckcraft_ui_egui::SlideApp;
use egui::epaint::{ClippedPrimitive, ImageData, Primitive};
use egui::{Context, Event, Key, Modifiers, PointerButton, RawInput, Rect, TextureId};
use std::cell::RefCell;

use crate::a11y::Accessibility;
use crate::file_bridge::Files;

const MAX_EVENTS: usize = 4096;
const MAX_SIDE: i32 = 4096;
const MAX_PACKET: usize = 32 << 20;
const SCALE: f32 = 1.0;

struct Bridge {
    app: SlideApp,
    files: Files,
    context: Context,
    events: Vec<Event>,
    buttons: i32,
    modifiers: Modifiers,
    focused: bool,
    should_close: bool,
    close_pending: bool,
    last_tick: Option<u32>,
    elapsed_ms: u64,
    repaint_delay_ms: u32,
    draw: Vec<u8>,
    draw_keep_bytes: usize,
    accessibility: Accessibility,
}

thread_local! {
    static APP: RefCell<Option<Bridge>> = const { RefCell::new(None) };
}

fn with_app<T>(f: impl FnOnce(&mut Bridge) -> T) -> Option<T> {
    APP.with(|app| app.try_borrow_mut().ok()?.as_mut().map(f))
}

fn dimensions(w: i32, h: i32) -> Option<egui::Vec2> {
    ((1..=MAX_SIDE).contains(&w) && (1..=MAX_SIDE).contains(&h))
        .then(|| egui::vec2(w as f32, h as f32))
}

#[unsafe(no_mangle)]
pub extern "C" fn wc_init(w: i32, h: i32) -> i32 {
    if dimensions(w, h).is_none() {
        return 1;
    }
    APP.with(|state| {
        let Ok(mut state) = state.try_borrow_mut() else {
            return 1;
        };
        if state.is_some() {
            return 0;
        }
        let files = Files::default();
        let session = Session::with_new();
        let mut services = files.services();
        crate::clipboard::install(&mut services);
        let app = SlideApp::new(session, services);
        let context = Context::default();
        context.set_pixels_per_point(SCALE);
        *state = Some(Bridge {
            app,
            files,
            context,
            events: Vec::new(),
            buttons: 0,
            modifiers: Modifiers::default(),
            focused: true,
            should_close: false,
            close_pending: false,
            last_tick: None,
            elapsed_ms: 0,
            repaint_delay_ms: u32::MAX,
            draw: Vec::new(),
            draw_keep_bytes: 0,
            accessibility: Accessibility::default(),
        });
        0
    })
}

fn modifiers(mask: i32) -> Modifiers {
    let ctrl = mask & 2 != 0;
    Modifiers {
        shift: mask & 1 != 0,
        ctrl,
        alt: mask & 4 != 0,
        command: ctrl,
        mac_cmd: false,
    }
}

fn key(code: i32) -> Option<Key> {
    let name = match code {
        8 => "Backspace",
        9 => "Tab",
        13 => "Enter",
        27 => "Escape",
        127 => "Delete",
        256 => "ArrowLeft",
        257 => "ArrowRight",
        258 => "ArrowUp",
        259 => "ArrowDown",
        260 => "Home",
        261 => "End",
        262 => "PageUp",
        263 => "PageDown",
        264 => "Insert",
        288 => "F1",
        289 => "F2",
        290 => "F3",
        291 => "F4",
        292 => "F5",
        293 => "F6",
        294 => "F7",
        295 => "F8",
        296 => "F9",
        297 => "F10",
        298 => "F11",
        299 => "F12",
        32..=126 => {
            let ascii = [(code as u8).to_ascii_uppercase()];
            return Key::from_name(std::str::from_utf8(&ascii).ok()?);
        }
        _ => return None,
    };
    Key::from_name(name)
}

#[unsafe(no_mangle)]
pub extern "C" fn wc_event(kind: i32, a: i32, b: i32, c: i32, d: i32) -> i32 {
    with_app(|state| {
        if state.events.len() > MAX_EVENTS - 5 {
            return 1;
        }
        let mods = modifiers(d);
        if mods != state.modifiers {
            state.events.push(Event::ModifiersChanged(mods));
            state.modifiers = mods;
        }
        match kind {
            1 => {
                let scale = state.context.pixels_per_point();
                let pos = egui::pos2(a as f32 / scale, b as f32 / scale);
                state.events.push(Event::PointerMoved(pos));
                for (mask, button) in [
                    (1, PointerButton::Primary),
                    (2, PointerButton::Secondary),
                    (4, PointerButton::Middle),
                ] {
                    if (state.buttons ^ c) & mask != 0 {
                        state.events.push(Event::PointerButton {
                            pos,
                            button,
                            pressed: c & mask != 0,
                            modifiers: mods,
                        });
                    }
                }
                state.buttons = c & 7;
            }
            2 => {
                if let Some(key) = key(a) {
                    crate::clipboard::key_event(&mut state.events, key, b != 0, c != 0, mods);
                }
            }
            3 => state.events.push(Event::MouseWheel {
                unit: egui::MouseWheelUnit::Point,
                delta: egui::vec2(a as f32, b as f32),
                phase: egui::TouchPhase::Move,
                source: egui::MouseWheelSource::Wheel,
                modifiers: mods,
            }),
            crate::a11y::SET_ENABLED => state.accessibility.set_enabled(&state.context, a != 0),
            crate::a11y::ACTIVATE => {
                if let Some(event) = state.accessibility.action(a as u32, b as u32) {
                    state.events.push(event);
                }
            }
            4 => state.events.push(Event::PointerGone),
            5 => {
                state.focused = a != 0;
                state.events.push(Event::WindowFocused(state.focused));
            }
            _ => return 1,
        }
        0
    })
    .unwrap_or(1)
}

#[unsafe(no_mangle)]
pub extern "C" fn wc_text(codepoint: u32) -> i32 {
    let Some(ch) = char::from_u32(codepoint).filter(|ch| !ch.is_control()) else {
        return 1;
    };
    with_app(|state| {
        if state.events.len() >= MAX_EVENTS {
            return 1;
        }
        state.events.push(Event::Text(ch.to_string()));
        0
    })
    .unwrap_or(1)
}

#[unsafe(no_mangle)]
pub extern "C" fn wc_frame(w: i32, h: i32, time_ms: u32) -> i32 {
    let Some(size) = dimensions(w, h) else {
        return 1;
    };
    with_app(|state| {
        let previous_scale = state.context.pixels_per_point();
        let screen = Rect::from_min_size(egui::Pos2::ZERO, size / previous_scale);
        if let Some(last_tick) = state.last_tick.replace(time_ms) {
            state.elapsed_ms = state
                .elapsed_ms
                .saturating_add(u64::from(time_ms.wrapping_sub(last_tick)));
        }
        let mut input = RawInput {
            screen_rect: Some(screen),
            max_texture_side: Some(2048),
            time: Some(state.elapsed_ms as f64 / 1000.0),
            events: std::mem::take(&mut state.events),
            focused: state.focused,
            ..RawInput::default()
        };
        if let Some(root) = input.viewports.get_mut(&egui::ViewportId::ROOT) {
            root.native_pixels_per_point = Some(SCALE);
            root.inner_rect = Some(screen);
            root.focused = Some(state.focused);
            if std::mem::take(&mut state.close_pending) {
                root.events.push(egui::ViewportEvent::Close);
            }
        }
        let ctx = &state.context;
        let app = &mut state.app;
        app.raw_input_hook(ctx, &mut input);
        let mut logic = true;
        let frame = ctx.run_ui(input, |ui| {
            if std::mem::take(&mut logic) {
                app.logic(ui.ctx());
            }
            app.ui(ui);
        });
        let scale = frame.pixels_per_point;
        if let Some(update) = frame.platform_output.accesskit_update {
            state.accessibility.capture(update, scale);
        }
        crate::clipboard::output(&frame.platform_output.commands);
        if let Some(root) = frame.viewport_output.get(&egui::ViewportId::ROOT) {
            state.should_close |= root
                .commands
                .iter()
                .any(|command| matches!(command, egui::ViewportCommand::Close));
        }
        state.repaint_delay_ms = if scale != previous_scale {
            0
        } else {
            frame
                .viewport_output
                .get(&egui::ViewportId::ROOT)
                .and_then(|root| u32::try_from(root.repaint_delay.as_millis()).ok())
                .unwrap_or(u32::MAX)
        };
        let primitives = ctx.tessellate(frame.shapes, frame.pixels_per_point);
        let mut textures = frame.textures_delta;
        let screen_pixels = Rect::from_min_size(egui::Pos2::ZERO, size);
        let packed = pack_draw(
            &mut state.draw,
            &textures,
            &primitives,
            screen_pixels,
            scale,
        );
        // The PM host receives every delta in the draw packet and owns its application.
        textures.clear();
        if let Some(keep_bytes) = packed {
            state.draw_keep_bytes = keep_bytes;
            0
        } else {
            eprintln!("DeckCraft's frame exceeded the Presentation Manager bridge limits.");
            1
        }
    })
    .unwrap_or(1)
}

/// Delay until the next requested repaint. The native host also wakes for input.
#[unsafe(no_mangle)]
pub extern "C" fn wc_repaint_delay_ms() -> u32 {
    with_app(|state| state.repaint_delay_ms).unwrap_or(u32::MAX)
}

/// Take one pending presentation or media open request.
#[unsafe(no_mangle)]
pub extern "C" fn wc_open_requested() -> i32 {
    with_app(|state| state.files.take_request()).unwrap_or(0)
}

/// Allocate linear memory for one incoming filename and its contents.
#[unsafe(no_mangle)]
pub extern "C" fn wc_reserve_input(len: usize) -> usize {
    with_app(|state| state.files.reserve_input(len))
        .flatten()
        .unwrap_or(0)
}

/// Deliver the UTF-8 name followed by raw file bytes to DeckCraft's own import queue.
#[unsafe(no_mangle)]
pub extern "C" fn wc_accept_input(name_len: usize, data_len: usize) -> i32 {
    with_app(|state| {
        if state
            .files
            .accept_input(name_len, data_len, || state.app.begin_external_file("open"))
            .is_some()
        {
            state.context.request_repaint();
            0
        } else {
            1
        }
    })
    .unwrap_or(1)
}

/// The native window requested a close. `0` lets the upstream save prompt run first.
#[unsafe(no_mangle)]
pub extern "C" fn wc_close_request() -> i32 {
    with_app(|state| {
        if state.app.quit_confirmed
            || !state
                .app
                .session
                .documents()
                .iter()
                .any(|document| document.is_dirty())
        {
            return 1;
        }
        state.close_pending = true;
        state.context.request_repaint();
        0
    })
    .unwrap_or(1)
}

/// Acknowledge an upstream viewport close after the user has answered the prompt.
#[unsafe(no_mangle)]
pub extern "C" fn wc_should_close() -> i32 {
    with_app(|state| i32::from(std::mem::take(&mut state.should_close))).unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn wc_draw_ptr() -> usize {
    with_app(|state| state.draw.as_ptr() as usize).unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn wc_draw_len() -> usize {
    with_app(|state| state.draw.len()).unwrap_or(0)
}

/// Called only after the native renderer has consumed this frame. Texture
/// uploads are one-shot: retain the mesh-sized budget, not the upload peak.
#[unsafe(no_mangle)]
pub extern "C" fn wc_draw_consumed() -> i32 {
    with_app(|state| {
        craft_memory::buffers::consume_draw(&mut state.draw, state.draw_keep_bytes);
        0
    })
    .unwrap_or(1)
}

/// Diagnostic capacities only: neither query changes packet storage or pointers.
#[unsafe(no_mangle)]
pub extern "C" fn wc_draw_capacity() -> usize {
    with_app(|state| state.draw.capacity()).unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn wc_input_capacity() -> usize {
    with_app(|state| state.files.input_capacity()).unwrap_or(0)
}

/// The current native accessibility snapshot remains valid until the next frame.
#[unsafe(no_mangle)]
pub extern "C" fn wc_a11y_ptr() -> usize {
    with_app(|state| state.accessibility.packet.as_ptr() as usize).unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn wc_a11y_len() -> usize {
    with_app(|state| state.accessibility.packet.len()).unwrap_or(0)
}

fn texture_number(id: TextureId) -> Option<u32> {
    let (value, tag) = match id {
        TextureId::Managed(n) => (n, 0),
        TextureId::User(n) => (n, 0x8000_0000),
    };
    (value < 0x8000_0000).then(|| value as u32 | tag)
}

fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn put_f32(out: &mut Vec<u8>, value: f32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn pack_draw(
    out: &mut Vec<u8>,
    textures: &egui::TexturesDelta,
    primitives: &[ClippedPrimitive],
    screen: Rect,
    scale: f32,
) -> Option<usize> {
    let pixels = |pos: egui::Pos2| (pos.to_vec2() * scale).to_pos2();
    let meshes = || {
        primitives.iter().filter_map(|item| {
            let Primitive::Mesh(mesh) = &item.primitive else {
                return None;
            };
            let clip = Rect::from_min_max(pixels(item.clip_rect.min), pixels(item.clip_rect.max))
                .intersect(screen);
            (clip.is_positive() && !mesh.indices.is_empty()).then_some((mesh, clip))
        })
    };
    let updates = || {
        textures
            .set
            .iter()
            .flat_map(|(id, deltas)| deltas.iter().map(move |delta| (*id, delta)))
    };
    let mut bytes = 16usize.checked_add(textures.free.len().checked_mul(4)?)?;
    let mut texture_bytes = 0usize;
    let mut set_count = 0u32;
    for (_, delta) in updates() {
        set_count = set_count.checked_add(1)?;
        let ImageData::Color(image) = &delta.image;
        let upload_bytes = 20usize.checked_add(image.pixels.len().checked_mul(4)?)?;
        bytes = bytes.checked_add(upload_bytes)?;
        texture_bytes = texture_bytes.checked_add(upload_bytes)?;
    }
    let mut mesh_count = 0u32;
    for (mesh, _) in meshes() {
        bytes = bytes
            .checked_add(28)?
            .checked_add(mesh.vertices.len().checked_mul(20)?)?
            .checked_add(mesh.indices.len().checked_mul(4)?)?;
        mesh_count = mesh_count.checked_add(1)?;
    }
    if bytes > MAX_PACKET {
        return None;
    }
    craft_memory::buffers::prepare_draw(out, bytes)?;
    out.extend_from_slice(b"WCG1");
    put_u32(out, set_count);
    put_u32(out, mesh_count);
    put_u32(out, u32::try_from(textures.free.len()).ok()?);
    for (id, delta) in updates() {
        let ImageData::Color(image) = &delta.image;
        put_u32(out, texture_number(id)?);
        let [x, y] = delta.pos.unwrap_or([u32::MAX as usize; 2]);
        put_u32(out, u32::try_from(x).ok()?);
        put_u32(out, u32::try_from(y).ok()?);
        for d in image.size {
            put_u32(out, u32::try_from(d).ok()?);
        }
        for color in &image.pixels {
            out.extend_from_slice(&color.to_array());
        }
    }
    for (mesh, clip) in meshes() {
        put_u32(out, texture_number(mesh.texture_id)?);
        for value in [clip.min.x, clip.min.y, clip.max.x, clip.max.y] {
            put_f32(out, value);
        }
        put_u32(out, u32::try_from(mesh.vertices.len()).ok()?);
        put_u32(out, u32::try_from(mesh.indices.len()).ok()?);
        for vertex in &mesh.vertices {
            let pos = pixels(vertex.pos);
            for value in [pos.x, pos.y, vertex.uv.x, vertex.uv.y] {
                put_f32(out, value);
            }
            out.extend_from_slice(&vertex.color.to_array());
        }
        for index in &mesh.indices {
            put_u32(out, *index);
        }
    }
    for id in &textures.free {
        put_u32(out, texture_number(*id)?);
    }
    bytes.checked_sub(texture_bytes)
}
