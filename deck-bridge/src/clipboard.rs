//! Presentation Manager clipboard integration for the upstream editor and egui widgets.
use deckcraft_ui_egui::Services;
use egui::{Event, Key, Modifiers, OutputCommand};

const TEXT: u32 = 1;
const HTML: u32 = 2;
const PICTURE: u32 = 5;
const MAX_TEXT: usize = 16 << 20;
const MAX_PICTURE: usize = 64 << 20;

#[link(wasm_import_module = "wc_host")]
unsafe extern "C" {
    fn clipboard_read(format: u32, bytes: *mut u8, capacity: usize) -> u32;
    fn clipboard_write(text: *const u8, text_len: usize, html: *const u8, html_len: usize, math: *const u8, math_len: usize) -> u32;
    #[link_name = "open_url_native"]
    fn open_url(url: *const u8, length: usize);
    fn clipboard_image(bytes: *const u8, width: usize, height: usize) -> u32;
}

fn bytes(format: u32, limit: usize) -> Option<Vec<u8>> {
    // The clipboard may change between the size query and the copy. The host bounds the write.
    let size = unsafe { clipboard_read(format, std::ptr::null_mut(), 0) } as usize;
    if size == 0 || size > limit {
        return None;
    }
    let mut out = Vec::new();
    out.try_reserve_exact(size).ok()?;
    out.resize(size, 0);
    let written = unsafe { clipboard_read(format, out.as_mut_ptr(), out.len()) } as usize;
    if written > out.len() {
        return None;
    }
    out.truncate(written);
    Some(out)
}

fn text() -> Option<String> {
    Some(String::from_utf8(bytes(TEXT, MAX_TEXT)?).ok()?.replace("\r\n", "\n"))
}

fn html() -> Option<String> {
    String::from_utf8(bytes(HTML, MAX_TEXT)?).ok()
}

fn put(html: &str, text: &str, mathml: &str) -> bool {
    unsafe { clipboard_write(text.as_ptr(), text.len(), html.as_ptr(), html.len(), mathml.as_ptr(), mathml.len()) != 0 }
}

pub(crate) fn install(services: &mut Services) {
    services.clipboard_html = Some(Box::new(html));
    services.clipboard_set_html = Some(Box::new(|html, text| put(html, text, "")));
    services.clipboard_image = Some(Box::new(|| bytes(PICTURE, MAX_PICTURE)));
}

pub(crate) fn key_event(events: &mut Vec<Event>, key: Key, pressed: bool, repeat: bool, modifiers: Modifiers) {
    let control = modifiers.command_only();
    let shift = modifiers.shift && !modifiers.ctrl && !modifiers.alt;
    if pressed {
        match key {
            Key::C | Key::Insert if control => {
                events.push(Event::Copy);
                return;
            }
            Key::X if control => {
                events.push(Event::Cut);
                return;
            }
            Key::Delete if shift => {
                events.push(Event::Cut);
                return;
            }
            _ => {}
        }
    }
    if pressed && (key == Key::Insert && shift || key == Key::V && control) {
        events.push(Event::Paste(text().unwrap_or_default()));
        if key == Key::Insert {
            // The native Shift+Insert chord is a Paste key even if Shift is released first.
            events.push(Event::Key { key: Key::Paste, physical_key: None, pressed: false, repeat, modifiers: Modifiers::NONE });
        }
        // Like egui-winit, suppress the Ctrl+V press. Its later release can paste a
        // picture with no text even when the Ctrl key comes up first.
        return;
    }
    // Retain other key events for shortcuts such as Ctrl+Alt+V (Paste Special).
    events.push(Event::Key { key, physical_key: None, pressed, repeat, modifiers });
}

pub(crate) fn output(commands: &[OutputCommand]) {
    for command in commands {
        match command {
            OutputCommand::CopyText(text) => {
                put("", text, "");
            }
            OutputCommand::CopyImage(image) => {
                let Some(size) = image.pixels.len().checked_mul(4).filter(|&n| n <= MAX_PICTURE) else { continue };
                let mut rgba = Vec::new();
                if rgba.try_reserve_exact(size).is_err() {
                    continue;
                }
                for pixel in &image.pixels {
                    rgba.extend_from_slice(&pixel.to_srgba_unmultiplied());
                }
                unsafe { clipboard_image(rgba.as_ptr(), image.width(), image.height()) };
            }
            OutputCommand::OpenUrl(url) => {
                unsafe { open_url(url.url.as_ptr(), url.url.len()) };
            }
            OutputCommand::TextSelectionSettled(_) => {}
        }
    }
}
