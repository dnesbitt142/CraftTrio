# DeckCraft 0.15.0 verification

The OS/2 sandbox reported Warp 4.50 revision 14.086. A Presentation Manager probe reported a 1280×800 desktop with 32 bits per pixel and 16,777,216 physical colors. The capture files are direct QEMU framebuffers.

## Reactor regression

The host-side regression drives the same Rust-to-Presentation Manager interface compiled into the EXE. It tests the WCG1 drawing and WCA1 accessibility packets at two window sizes, opening and cancelling the file picker, adding a slide, cancelling and retrying save, the dirty-window close prompt, and starting and leaving the slide show. It also activates Print through the accessibility tree and captures the generated PDF.

The optimized run also tested File > Save As. Cancelling left that page open. Completing the save returned to the editor. Independent readers reopened the generated PPTX and PDF. The three-slide fixture became four slides and kept the original Field Notes title and table. The four-page PDF retained selectable title text. The two builds produced byte-identical PPTX and PDF outputs for this workflow.

| Reactor measurement | Baseline | Size-optimized |
| --- | ---: | ---: |
| Linear memory after open, edit, save and slide show | 27,000,832 B | 25,886,720 B |
| Linear memory after print workflow | 110,297,088 B | 93,192,192 B |
| Packed OS/2 executable | 56,823,290 B | 33,085,935 B |

The memory values measure the reactor's committed linear memory in the host-side test. They exclude the OS/2 process's code, display surfaces and other allocations.

## Baseline Warp smoke

The packed baseline opened the original Northern Lights sample in the native DeckCraft window. Its custom icon appeared in the title bar. Maximizing the Presentation Manager window expanded the ribbon and slide area and recalculated the slide zoom from 69% to 81%. Ribbon labels remained legible.

The slide sorter rendered all eight sample slides, including the diagram, chart and table. Ctrl+O opened the native Presentation Manager picker. The three-slide PPTX fixture loaded with a font substitution notice. Ctrl+Shift+N added a second slide. Ctrl+S saved the four-slide presentation back to its original path in the disposable guest and cleared the edited flag.

The baseline also opened the supplied Field Notes presentation and inserted a fourth slide. Ctrl+S completed and the editor reported Saved.

## Packaged Warp build

The 33,085,935-byte optimized executable loaded the production `CRAFTPM.DLL`, displayed the eight-slide Northern Lights sample, and reflowed when maximized. The startup log measured 14,876,672 bytes of engine linear memory. The native file picker opened the three-slide Field Notes fixture. One imported presentation frame took about 105 seconds in the emulated Pentium guest.

[Maximized editor](../screenshots/deckcraft-01-maximized-editor.png) · [Imported presentation](../screenshots/deckcraft-02-pptx-import.png) · [Native slideshow](../screenshots/deckcraft-03-slideshow.png) · [Guest log excerpt](deck-warp.log)

F5 opened the imported presentation in slideshow mode in the native PM window. The first slideshow frame took about 148 seconds including rendering and presentation to the screen on the emulated Pentium. The engine regression also exercised leaving the slideshow with Escape.
