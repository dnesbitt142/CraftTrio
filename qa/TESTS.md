# Craft Trio test record

The Warp test guests use isolated copies of the same OS/2 Warp 4.52 installation, a Pentium CPU profile, 384 MiB RAM, and SciTech SNAP. The screenshots are framebuffer captures at 1280×800. The [guest display query](snap-display-mode.txt) measured 32 bits per pixel and 16,777,216 colors.

## Applications on Warp

| Program | Checks | Startup engine arena |
| --- | --- | --- |
| WordCraft | Opened the built-in handbook, maximized from 1144×688 to 1280×751, displayed the handbook in the native Accessibility Navigator, opened the supplied DOCX through the native picker, edited its heading, and saved it. A separate reader checked ZIP integrity, the new heading, accented text, and table cells. [Saved DOCX](wordcraft-saved-qa.docx) · [Guest log](word-warp.log) | 35,127,296 bytes |
| DeckCraft | Opened the eight-slide Northern Lights sample, maximized the PM window, and imported the supplied Field Notes PPTX through the native picker, and displayed it in slideshow mode. [DeckCraft test details](DECKCRAFT.md) | 14,876,672 bytes |
| GridCraft | Opened the sales dashboard, maximized from 1144×688 to 1280×751, opened the supplied XLSX, entered a formula, recalculated the forecast, and saved. A separate XLSX reader checked the saved formula and cached result, the dependent value, sheet names, merged header, chart, and ZIP CRCs. The native clipboard peer read the copied result. [GridCraft test details](grid-warp.md) | 7,929,856 bytes |

Arena figures are the committed engine linear memory recorded at the third startup frame. PM and rasterizer allocations and native code pages are separate.

## Native host and loader

A small OS/2 LX probe loaded the shipping `CRAFTPM.DLL` from beside the executable while its working directory was on a different drive. It exercised the DLL's clock, engine-memory and rendering callbacks. Both packed and unpacked copies of the icon-bound probe ran on Warp. Their 1280×800 framebuffer captures differ only in the desktop clock. The packed probe resized from 1144×688 to 1280×751. [Unpacked log](pm-probe-plain.log) · [Packed log](pm-probe-packed.log)

The Workplace Shell installer ran twice from a directory with spaces. The second run updated the same folder and three launchers. This check used small OS/2 probe executables with the suite's icons. [Desktop capture](wps-installation.png) · [Icon resource log](icons.log)

## Host checks

- The shared rasterizer passed pixel comparisons, partial texture updates, clipping, randomized triangle coverage, and malformed-packet checks after the framebuffer change.
- The packed LX writer checked each encoded page against its original before writing the executable. The compression preserves code, resource bytes, and relocations.
- The GridCraft bridge opened the XLSX test workbook, calculated `20+22`, copied `42` as text and HTML, pasted text from the host, tested Save As cancellation and wrote new XLSX and PDF files. An independent XLSX inspection verified the edited formula, cached value, pasted cell and embedded chart.

- The size-focused DeckCraft bridge exercised Open and cancel, Save and Save As, editing, slide playback, resize, the accessibility tree, and PDF export. Independent readers verified four slides and four PDF pages. The PPTX and PDF bytes matched the earlier build. The committed engine arena after the PDF export fell from 110,297,088 to 93,192,192 bytes.

## Prior standalone WordCraft test evidence

[Clipboard](prior-wordcraft/CLIPBOARD.md) · [Printing](prior-wordcraft/PRINTING.md) · [Accessibility controls and component checks](ACCESSIBILITY.md)
