#!/usr/bin/env python3
"""Independently parse and rasterize PDFs from test-grid-export.py.

Needs pypdf and PyMuPDF. The producer probe first asserts byte equivalence
against the pinned previous implementation, so these checks cover both.
"""
from pathlib import Path
import argparse
import hashlib
import json

import fitz
from pypdf import PdfReader

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--pdfs', type=Path, default=root / 'build/export-memory/grid/pdf')
parser.add_argument('--result', type=Path, default=root / 'build/export-memory/grid/independent-pdf-check.json')
args = parser.parse_args()
results = []
for name, count in [('opaque-flat', 1), ('alpha-gradient', 1), ('multi-page', 4)]:
    path = args.pdfs / (name + '.pdf')
    reader = PdfReader(path, strict=True)
    assert len(reader.pages) == count
    assert reader.metadata.title == 'Export memory regression'
    hashes = []
    with fitz.open(path) as rendered:
        assert len(rendered) == count
        for index, page in enumerate(rendered):
            assert f'Page {index + 1} Café' in page.get_text()
            assert len(page.get_images()) == 1
            pixels = page.get_pixmap(alpha=False)
            assert (pixels.width, pixels.height) == (600, 400)
            hashes.append(hashlib.sha256(pixels.samples).hexdigest())
    results.append({'fixture': name, 'pages': count, 'render_sha256': hashes,
                    'independent_reader': 'PyMuPDF and pypdf', 'passed': True})
args.result.parent.mkdir(parents=True, exist_ok=True)
args.result.write_text(json.dumps(results, indent=2) + '\n')
print('PASS independent readers: 3 PDFs, 6 pages, text, metadata, images, rasterization')
