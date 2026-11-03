# Craft Trio for OS/2 Warp 4.5

WordCraft, DeckCraft, and GridCraft run as 32-bit Presentation Manager programs. They share native desktop services in `CRAFTPM.DLL`. Each editor runs its own engine and carries a distinct icon.

## Run the suite

1. Extract the archive to a writable HPFS directory. Keep all four binaries together in `BIN`.
2. Generate a fresh entropy file on a modern host with `head -c 65536 /dev/urandom > ENTROPY.BIN`. Transfer it into `BIN`.
3. In an OS/2 command window, change to `BIN`. Run `START /PM WORDCRFT.EXE`, `START /PM DECKCRFT.EXE`, or `START /PM GRIDCRFT.EXE`.
4. To add Workplace Shell launchers, run `INSTALL.CMD` from the extracted suite directory. This creates the **Craft Suite** folder on your Desktop. Run it again after moving the suite to refresh the launchers.

Each editor opens a sample. Use **Ctrl+O** for the native file chooser and the files in `examples` for more to explore. Press **Ctrl+Shift+F12** to open the native Accessibility Navigator. Its keyboard controls are in [ACCESSIBILITY.md](qa/ACCESSIBILITY.md).

| Program | Main file format | Printing |
| --- | --- | --- |
| WordCraft | DOCX | OS/2 print queue. Install a printer in OS/2 before opening **File › Print**. |
| DeckCraft | PPTX | **File › Print** saves a PDF. |
| GridCraft | XLSX | **File › Export › Create PDF** saves a PDF. |

The file chooser accepts ASCII paths. The shared port limits imports to 16 MiB and exports to 48 MiB. Each editor commits its engine memory on demand. WordCraft and GridCraft cap that arena at 96 MiB. DeckCraft caps it at 128 MiB to accommodate PDF export. Large documents, presentations with embedded media, or large spreadsheets can hit that cap. DeckCraft substitutes built-in fallback fonts when presentation fonts are unavailable and has no native audio output.

The editors write host and engine logs to their working directory. WordCraft uses `WCGUI.LOG` and `WCENGINE.LOG`, DeckCraft uses `DCGUI.LOG` and `DCENGINE.LOG`, and GridCraft uses `GCGUI.LOG` and `GCENGINE.LOG`. Network features that open a web page use the OS/2 browser. The optional `CRAFT_BROWSER` setting supplies its executable path. WordCraft can use a compatible Zotero integration peer on `127.0.0.1:23116` and an OS/2 curl installation with a trusted CA bundle for release checks. Set `CRAFT_CURL` to the executable path if curl is outside `PATH`.

## Installed footprint

| Executable | Before LX packing | Installed |
| --- | ---: | ---: |
| WordCraft | 225.0 MiB | 73.0 MiB |
| DeckCraft | 94.8 MiB | 31.6 MiB |
| GridCraft | 70.1 MiB | 23.3 MiB |

The shared `CRAFTPM.DLL` is 89.7 KiB. LX packing reduces disk use. Removing a duplicate framebuffer saves 3.7 MiB per editor at a 1280×751 client size. DeckCraft's size-focused build and smaller PDF backdrop cache reduced committed engine memory after the fixture PDF export from 105.2 MiB to 88.9 MiB in the host ABI test. The PM host keeps a separate bitmap in the format GPI expects. Initial loads and some redraws were slow on the emulated Pentium.

[Build from source](BUILD.md) · [Warp test record](qa/TESTS.md) · [Screenshots](screenshots/README.md)
