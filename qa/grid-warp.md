# GridCraft on Warp 4.52

The packed GridCraft executable ran with `CRAFTPM.DLL` on a 384 MiB Pentium guest using SciTech SNAP at 1280×800 and 32 bits per pixel. The reactor reported a 7.56 MiB linear arena at startup. The native maximize control reflowed the client from 1144×688 to 1280×751. [Native frame log](grid-gui.log)

1. Opened the built-in Quarterly Sales Dashboard. The ribbon, sparklines, bar chart, and pie chart fit in the maximized window at 90% worksheet zoom. [Dashboard](../screenshots/gridcraft-01-sales-dashboard.png)
2. Used Ctrl+O in GridCraft to open the Presentation Manager file dialog and load the supplied `PILOT.XLSX`. The workbook showed its merged title, frozen panes, currency formatting, conditional colors, and formulas. [Forecast](../screenshots/gridcraft-02-forecast-formulas.png)
3. Entered `=20+22` in B4. GridCraft displayed 42 and recalculated D4 to $1,176. The Quick Access Save button wrote the workbook. The first keyboard save attempt left the modified marker visible.
4. Copied the saved file from Warp. An independent reader checked the ZIP CRCs, sheet names, merged title, edited and dependent formulas, cached results, and embedded chart. Reopened a copy of the same saved file in Warp and displayed its Sales trend chart. [Saved workbook](grid-warp-saved.xlsx) · [Verification script](verify-grid.py) · [Reopened chart](../screenshots/gridcraft-03-workbook-chart.png)
5. Used Home ribbon Copy on B4. An independent Presentation Manager process read the native `CF_TEXT` clipboard value as `42` followed by CRLF. [Clipboard bytes](grid-native-copy.txt)
