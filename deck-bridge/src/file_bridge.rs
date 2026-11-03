//! File selection and byte delivery for the Presentation Manager host.
use std::cell::Cell;
use std::rc::Rc;

use deckcraft_ui_egui::{Inbox, InboxEvent, InboxFile, Services};

const MAX_PATH_BYTES: usize = 1024;
const MAX_INPUT_BYTES: usize = 16 << 20;

#[link(wasm_import_module = "wc_host")]
unsafe extern "C" {
    #[link_name = "save"]
    fn native_save(name: *const u8, name_len: usize, data: *const u8, data_len: usize, save_as: i32, path: *mut u8, path_cap: usize) -> i32;
}

fn save(name: &str, data: &[u8], save_as: bool) -> Result<Option<String>, String> {
    let mut path = [0u8; MAX_PATH_BYTES];
    // The host validates both slices and writes at most the capacity of the output buffer.
    let result = unsafe { native_save(name.as_ptr(), name.len(), data.as_ptr(), data.len(), i32::from(save_as), path.as_mut_ptr(), path.len()) };
    match result {
        -1 => Ok(None),
        0 => Ok(Some(name.to_owned())),
        n if n > 0 => {
            let name = path.get(..n as usize).ok_or("The OS/2 path is too long")?;
            let name = std::str::from_utf8(name).map_err(|_| "The OS/2 path is not valid UTF-8")?;
            if name.contains('\0') {
                return Err("The OS/2 path contains a zero byte".into());
            }
            Ok(Some(name.to_owned()))
        }
        _ => Err("OS/2 could not save the presentation".into()),
    }
}

#[derive(Default)]
pub(crate) struct Files {
    request: Rc<Cell<Option<(i32, u64)>>>,
    inbox: Inbox,
    active: Option<u64>,
    input: Vec<u8>,
}

impl Files {
    pub(crate) fn services(&self) -> Services {
        let request = self.request.clone();
        let inbox = self.inbox.clone();
        Services {
            open_async: Some(Box::new(move |purpose, id| {
                let kind = match purpose {
                    "picture" => 2,
                    "audio" => 3,
                    "video" => 4,
                    _ => 1,
                };
                if let Some((_, id)) = request.replace(Some((kind, id)))
                    && let Ok(mut inbox) = inbox.lock()
                {
                    inbox.push(InboxEvent::Cancelled { id });
                }
            })),
            inbox: Some(self.inbox.clone()),
            host_save: Some(Box::new(save)),
            ..Services::default()
        }
    }

    pub(crate) fn take_request(&mut self) -> i32 {
        let Some((kind, id)) = self.request.take() else { return 0 };
        if let Some(id) = self.active.replace(id)
            && let Ok(mut inbox) = self.inbox.lock()
        {
            inbox.push(InboxEvent::Cancelled { id });
        }
        kind
    }

    pub(crate) fn reserve_input(&mut self, len: usize) -> Option<usize> {
        if len == 0 || len > MAX_INPUT_BYTES + MAX_PATH_BYTES {
            return None;
        }
        self.input.clear();
        self.input.try_reserve_exact(len).ok()?;
        self.input.resize(len, 0);
        Some(self.input.as_mut_ptr() as usize)
    }

    pub(crate) fn accept_input(&mut self, name_len: usize, data_len: usize, direct_open: impl FnOnce() -> u64) -> Option<()> {
        let mut inbox = self.inbox.lock().ok()?;
        if name_len == 0 && data_len == 0 {
            inbox.push(InboxEvent::Cancelled { id: self.active.take()? });
            return Some(());
        }
        if name_len == 0 || name_len > MAX_PATH_BYTES || data_len > MAX_INPUT_BYTES || name_len.checked_add(data_len)? != self.input.len() {
            return None;
        }
        let name = std::str::from_utf8(self.input.get(..name_len)?).ok()?;
        if name.contains('\0') {
            return None;
        }
        let name = name.to_owned();
        self.input.copy_within(name_len.., 0);
        self.input.truncate(data_len);
        let id = self.active.take().unwrap_or_else(direct_open);
        inbox.push(InboxEvent::File(InboxFile { id, name, bytes: std::mem::take(&mut self.input) }));
        Some(())
    }
}
