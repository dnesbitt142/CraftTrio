# Presentation Manager printing

These results are from the earlier standalone WordCraft build.
WordCraft uses the installed OS/2 printer queues from File › Print. It renders one document page at a time and sends it to the selected queue using the standard PM graphics spooler. Page ranges, copies, collation, print markup, background options, and grayscale use the application’s settings. The renderer produces up to 144 DPI at the original page size. The printer driver determines which paper sizes and duplex settings are available. A printer can hold the job until the selected paper is loaded.

The test ran in a separate Warp 4.52 overlay with PSPRINT 30.907 Generic PostScript (Enhanced) configured on LPT1. QEMU captured the bytes that the printer driver sent to the parallel port.

| Probe | Result |
| --- | --- |
| Color portrait | The spooler accepted job #1 and emitted two Letter pages. Ghostscript rendered both at 72 DPI. Page 1 contains the expected RGB values (245, 35, 35) and (245, 150, 35). Page 2 contains (30, 35, 225) and (30, 150, 225). |
| A4 on the Letter queue | The spooler held job #3 with status 0x0200 (form change) and two spooled pages. The queue produced zero pages before the test job was cancelled. |
| Landscape and grayscale | Ghostscript rendered two Letter pages. All pixels were grayscale. The two colored rectangles on each input page rotated with the page and produced the expected luminance values. |
| Long-edge duplex | PSPRINT accepted job #6 and emitted its DuplexNoTumble feature with `/Duplex true/Tumble false`. |

On the final WordCraft executable, Ctrl+O loaded the rich DOCX fixture. File › Print showed the one-page preview and selected the native WordCraft Test queue. Pressing Print returned to the document and PSPRINT emitted one Letter page. Ghostscript rendered the captured stream at 150 DPI. The heading, bold and italic text, table cells, and “Café & résumé” are visible on the rendered page and matched independent OCR.

[WordCraft print preview](print-preview.png) · [Captured PostScript](warp-printed.ps) · [Rendered print output](warp-printed.png)

[Native probe source](print-probe.c) · [Color probe log](print-probe.log) · [A4 queue status](print-form-hold.log)

Use OpenWatcom with the same OS/2 flags as the build to compile and link the probe against `host/gui_print.c`. Run it from a writable directory in the guest. The optional arguments are orientation (1 portrait or 2 landscape), native form number (0 for printer default), duplex (0 default, 1 off, 2 long edge, 3 short edge), and grayscale (0 color or 1 grayscale). The probe uses the first installed queue and writes `WCPRINT.LOG`.
