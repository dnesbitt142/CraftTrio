# OS/2 Accessibility Navigator

The WordCraft reading-pane behavior and component results below were measured with the earlier standalone build.
Press **Ctrl+Shift+F12** in the editor to open the Accessibility Navigator. The navigator presents WordCraft's current ribbon controls and document structure as a native Presentation Manager list and read-only text control. It reads the same document and controls the editor is displaying.

| Keyboard input | Action |
| --- | --- |
| Up or Down | Select an item and show its text and state in the reading pane. |
| Tab or Shift+Tab | Move between the list, reading pane and available buttons. |
| Enter | Activate the selected control and return to the editor. Text fields receive focus. |
| Ctrl+D | Select the document and move directly to the reading pane. |
| Ctrl+Shift+F12 | Return to the editor with the navigator still open. Use the same shortcut in the editor to bring the navigator forward. |
| Escape | Close the navigator. |

The document tree preserves the upstream reading order and includes headings, lists, tables, links and image descriptions. The reading pane covers the visible pages and neighboring pages described by WordCraft. Scroll the editor to reach other pages, then return to the navigator. Large ribbon buttons expose their command names and enabled state. The navigator uses the active Presentation Manager code page. The editor also exposes its text cursor through the PM cursor API while the navigator is enabled.

The navigator uses standard Presentation Manager controls to present text and keyboard focus. IBM Screen Reader/2 uses an off-screen model of the OS/2 interface and can be customized with application profiles. Speech output, Braille devices and Screen Reader/2 itself require a configured installation and were outside the tests below. [IBM Screen Reader/2 overview](https://research.ibm.com/publications/screen-reader2-access-to-os2-and-the-graphical-user-interface) · [Screen Reader/2 and its Profile Access Language](https://www.os2world.com/wiki/index.php/How_to_Make_OS/2_Talk_..._And_Why._Access_to_OS/2_with_IBM_Screen_Reader/2)

## Component tests

- A Rust AccessKit fixture verified the packet's document and heading text, password-value redaction, caret coordinates, 64-bit node activation and disable behavior.
- A test of the WordCraft bridge selected Add/Edit Citation by its accessibility name, activated it through the semantic action, and verified the five fixture replies and the inserted citation.
- OpenWatcom compiled the navigator with all warnings treated as errors.
- A small LX probe linked the same native navigator and clipboard code-page converter. On Warp 4.52, Ctrl+Shift+F12 opened the navigator and Ctrl+D put the accented fixture text in the native reading pane. Shift+Tab, Up and Enter activated the expected control and returned focus to the editor. The disabled control sent no action. The PM cursor query reported the expected position and height. Escape closed the navigator, and the shortcut reopened it. The probe also rejected a truncated packet. [Guest probe log](a11y-probe.log)
