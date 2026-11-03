//! Project DeckCraft's AccessKit tree onto standard Presentation Manager controls.

use egui::accesskit::{Action, ActionRequest, Node, NodeId, Role, Toggled, TreeId, TreeUpdate};
use std::collections::{HashMap, HashSet};

pub(crate) const SET_ENABLED: i32 = 20;
pub(crate) const ACTIVATE: i32 = 21;

const MAX_ROWS: usize = 7000;
const MAX_PACKET: usize = 2 << 20;
const MAX_VALUE: usize = 32 << 10;
const MAX_DEPTH: usize = 64;
const CLICK: u32 = 1;
const FOCUS: u32 = 2;
const DISABLED: u32 = 4;
const FOCUSED: u32 = 8;
const DOCUMENT: u32 = 16;

#[derive(Default)]
pub(crate) struct Accessibility {
    pub(crate) enabled: bool,
    pub(crate) packet: Vec<u8>,
    actions: HashMap<NodeId, Action>,
}

impl Accessibility {
    pub(crate) fn set_enabled(&mut self, context: &egui::Context, enabled: bool) {
        self.enabled = enabled;
        if enabled {
            context.enable_accesskit();
        } else {
            context.disable_accesskit();
            self.packet.clear();
            self.actions.clear();
        }
        context.request_repaint();
    }

    pub(crate) fn action(&self, low: u32, high: u32) -> Option<egui::Event> {
        let target_node = NodeId(u64::from(low) | (u64::from(high) << 32));
        Some(egui::Event::AccessKitActionRequest(ActionRequest {
            action: *self.actions.get(&target_node)?,
            target_tree: TreeId::ROOT,
            target_node,
            data: None,
        }))
    }

    pub(crate) fn capture(&mut self, update: TreeUpdate, pixels_per_point: f32) {
        self.packet.clear();
        self.actions.clear();
        if !self.enabled {
            return;
        }
        let Some(tree) = update.tree else { return };
        // egui publishes a complete snapshot each pass, including the root.
        let nodes: HashMap<NodeId, Node> = update.nodes.into_iter().collect();
        let caret = nodes
            .get(&update.focus)
            .filter(|node| node.role() == Role::Document)
            .and_then(|node| node.text_selection())
            .and_then(|selection| {
                let position = selection.focus;
                let run = nodes.get(&position.node)?;
                let bounds = run.bounds()?;
                let x = run.character_positions()?.get(position.character_index).map_or(bounds.x1, |offset| bounds.x0 + f64::from(*offset));
                let h = bounds.height();
                let scale = f64::from(pixels_per_point);
                (x.is_finite() && bounds.y0.is_finite() && h.is_finite() && h > 0.0)
                    .then(|| [(x * scale) as i32, (bounds.y0 * scale) as i32, (h * scale).ceil() as i32])
            })
            .unwrap_or([0; 3]);
        self.packet.extend_from_slice(b"WCA1");
        for coord in caret {
            self.packet.extend_from_slice(&coord.to_le_bytes());
        }
        let mut pending = vec![(tree.root, 0usize)];
        let mut seen = HashSet::new();
        let mut rows = 0;
        while let Some((id, depth)) = pending.pop() {
            if depth > MAX_DEPTH || !seen.insert(id) || rows >= MAX_ROWS {
                continue;
            }
            let Some(node) = nodes.get(&id).filter(|node| !node.is_hidden()) else { continue };
            let role = node.role();
            let name = node.label().unwrap_or("");
            let value = if role == Role::PasswordInput { "" } else { node.value().unwrap_or("") };
            let describe =
                !name.is_empty() || !value.is_empty() || !matches!(role, Role::Unknown | Role::GenericContainer | Role::Window | Role::Group);
            if depth < MAX_DEPTH {
                pending.extend(node.children().iter().rev().map(|child| (*child, depth + usize::from(describe))));
            }
            if !describe {
                continue;
            }
            let mut flags = 0;
            if node.is_disabled() {
                flags |= DISABLED;
            } else if node.supports_action(Action::Click) {
                flags |= CLICK;
                self.actions.insert(id, Action::Click);
            } else if node.supports_action(Action::Focus) {
                flags |= FOCUS;
                self.actions.insert(id, Action::Focus);
            }
            if id == update.focus {
                flags |= FOCUSED;
            }
            let role_name = match role {
                Role::TextRun | Role::Label => String::new(),
                Role::Heading => format!("Heading {}", node.level().unwrap_or(1)),
                Role::ListItem => "List item".into(),
                Role::TextInput | Role::MultilineTextInput => "Text field".into(),
                Role::CheckBox => "Check box".into(),
                Role::ComboBox | Role::EditableComboBox => "Combo box".into(),
                Role::MenuItem => "Menu item".into(),
                Role::PasswordInput => "Password field".into(),
                _ => format!("{role:?}"),
            };
            let mut label = role_name;
            let summary = if name.is_empty() { value } else { name };
            if !label.is_empty() && !summary.is_empty() {
                label.push_str(" - ");
            }
            label.extend(summary.chars().take(220).map(|ch| if ch.is_control() { ' ' } else { ch }));
            if label.trim().is_empty() {
                continue;
            }
            match node.toggled() {
                Some(Toggled::True) => label.push_str(" (checked)"),
                Some(Toggled::False) => label.push_str(" (unchecked)"),
                Some(Toggled::Mixed) => label.push_str(" (mixed)"),
                None => {}
            }
            if node.is_selected() == Some(true) {
                label.push_str(" (selected)");
            }
            match node.is_expanded() {
                Some(true) => label.push_str(" (expanded)"),
                Some(false) => label.push_str(" (collapsed)"),
                None => {}
            }
            let mut detail = String::new();
            if role == Role::Document {
                flags |= DOCUMENT;
                document_text(&mut detail, node, &nodes);
            } else {
                append_value(&mut detail, value);
                if let Some(description) = node.description().filter(|description| !description.is_empty()) {
                    if !detail.is_empty() {
                        append_value(&mut detail, "\n");
                    }
                    append_value(&mut detail, description);
                }
            }
            if self.packet.len() + 24 + label.len() + detail.len() > MAX_PACKET {
                break;
            }
            self.packet.extend_from_slice(&id.0.to_le_bytes());
            for word in [flags, depth as u32, label.len() as u32, detail.len() as u32] {
                self.packet.extend_from_slice(&word.to_le_bytes());
            }
            self.packet.extend_from_slice(label.as_bytes());
            self.packet.extend_from_slice(detail.as_bytes());
            rows += 1;
        }
    }
}

fn append_value(out: &mut String, value: &str) {
    let mut end = value.len().min(MAX_VALUE.saturating_sub(out.len()));
    while !value.is_char_boundary(end) {
        end -= 1;
    }
    out.push_str(value.get(..end).unwrap_or_default());
}

fn document_text(out: &mut String, root: &Node, nodes: &HashMap<NodeId, Node>) {
    let mut pending: Vec<_> = root.children().iter().rev().copied().collect();
    let mut seen = HashSet::new();
    while let Some(id) = pending.pop() {
        if out.len() >= MAX_VALUE || seen.len() >= MAX_ROWS {
            break;
        }
        if !seen.insert(id) {
            continue;
        }
        let Some(node) = nodes.get(&id).filter(|node| !node.is_hidden()) else { continue };
        match node.role() {
            Role::TextRun => append_value(out, node.value().unwrap_or("")),
            Role::ListMarker => {
                append_value(out, node.value().unwrap_or(""));
                append_value(out, " ");
            }
            Role::Image => {
                append_value(out, "[Image");
                if let Some(label) = node.label() {
                    append_value(out, " - ");
                    append_value(out, label);
                }
                append_value(out, "]");
            }
            _ => {}
        }
        pending.extend(node.children().iter().rev().copied());
    }
}
