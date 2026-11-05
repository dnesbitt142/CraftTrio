#!/usr/bin/env python3
"""Independently validate and compare native workflow Office and PDF outputs.

Usage: workflow-check.py APP BASELINE_DIR OPTIMIZED_DIR REPORT.json
Pass the same directory twice to validate one build's outputs.
Requires pypdf; Office checks use only Python's standard ZIP/XML readers.
"""
import argparse
from decimal import Decimal, InvalidOperation
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET
from zipfile import ZipFile

from pypdf import PdfReader


EXTENSIONS = {"word": "docx", "grid": "xlsx", "deck": "pptx"}
NS = {"w": "http://schemas.openxmlformats.org/wordprocessingml/2006/main",
      "s": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
      "p": "http://schemas.openxmlformats.org/presentationml/2006/main",
      "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships"}
SCOPE = ("Independent ZIP CRC/XML and pypdf checks of native QA workflow outputs, "
         "with Office package content and PDF page/text comparison. This validator "
         "does not render PDF pixels or establish OS/2 PM, printer, or physical-memory behavior.")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def canonical_xml(data, entry):
    """Normalize XML syntax; omit only recognized core-property timestamps."""
    root = ET.fromstring(data)
    excluded = []
    if entry == "docProps/core.xml":
        for local in ("modified",):
            tag = "{http://purl.org/dc/terms/}" + local
            for node in root.iter(tag):
                excluded.append({"field": "dcterms:" + local, "value": node.text})
                node.text = "TIMESTAMP_IGNORED_FOR_CONTENT_COMPARISON"
    text = ET.tostring(root, encoding="unicode")
    return ET.canonicalize(text, rewrite_prefixes=True).encode(), excluded


def enabled_property(run, name):
    node = run.find(f"w:rPr/w:{name}", NS)
    return node is not None and node.get("{" + NS["w"] + "}val", "true").lower() not in ("0", "false", "off")


def office(path, app):
    raw = path.read_bytes()
    representation, raw_entries, ignored = {}, {}, []
    with ZipFile(path) as archive:
        entries = archive.namelist()
        require(len(entries) == len(set(entries)), "Office ZIP has duplicate entry names")
        require(archive.testzip() is None, "Office ZIP failed CRC verification")
        for name in sorted(entries):
            if name.endswith("/"):
                continue
            data = archive.read(name)
            raw_entries[name] = sha(data)
            if name.endswith((".xml", ".rels")) or name == "[Content_Types].xml":
                data, omitted = canonical_xml(data, name)
                ignored += [{"entry": name, **item} for item in omitted]
            representation[name] = sha(data)

        if app == "word":
            document = ET.fromstring(archive.read("word/document.xml"))
            text = "".join(node.text or "" for node in document.findall(".//w:t", NS))
            require("MEMORY_QA_WORD" in text, "Saved DOCX is missing the edit marker")
            require("Warp WordCraft" in text, "Saved DOCX is missing the original heading")
            styled = [run for run in document.findall(".//w:r", NS)
                      if "bold and italic" in "".join(node.text or "" for node in run.findall("w:t", NS))]
            require(any(enabled_property(run, "b") and enabled_property(run, "i") for run in styled),
                    "Saved DOCX did not preserve the original bold-and-italic run")
            tables = len(document.findall(".//w:tbl", NS))
            require(tables > 0, "Saved DOCX did not preserve its table")
            checks = {"editMarker": True, "originalHeading": True, "boldAndItalicRun": True, "tableCount": tables}
        elif app == "grid":
            charts = sorted(name for name in entries if re.fullmatch(r"xl/charts/chart[^/]*\.xml", name))
            require(charts, "Saved XLSX did not preserve a chart part")
            edited = []
            for name in sorted(entries):
                if not re.fullmatch(r"xl/worksheets/sheet[^/]*\.xml", name):
                    continue
                sheet = ET.fromstring(archive.read(name))
                for cell in sheet.findall(".//s:c", NS):
                    formula, cached = cell.find("s:f", NS), cell.find("s:v", NS)
                    if formula is None or (formula.text or "").strip().lstrip("=") != "20+22" or cached is None:
                        continue
                    try:
                        correct = Decimal(cached.text or "") == Decimal(42)
                    except InvalidOperation:
                        correct = False
                    if correct:
                        edited.append({"sheet": name, "cell": cell.get("r"), "formula": formula.text, "cached": cached.text})
            require(edited, "Saved XLSX is missing formula 20+22 with cached value 42")
            checks = {"editedFormulaCells": edited, "chartParts": charts}
        else:
            presentation = ET.fromstring(archive.read("ppt/presentation.xml"))
            slides = presentation.findall("p:sldIdLst/p:sldId", NS)
            require(len(slides) == 4, "Saved PPTX must contain four slide IDs")
            require(len({node.get("id") for node in slides}) == 4, "Saved PPTX slide IDs are duplicated")
            checks = {"slideCount": len(slides)}
    payload = json.dumps(representation, sort_keys=True, separators=(",", ":")).encode()
    return {"path": str(path.resolve()), "bytes": len(raw), "sha256": sha(raw), "checks": checks,
            "entryCount": len(representation), "contentSha256": sha(payload),
            "entries": representation, "rawEntries": raw_entries, "ignoredCoreTimestamps": ignored}


def pdf(path, app):
    raw = path.read_bytes()
    reader = PdfReader(path, strict=True)
    require(not reader.is_encrypted, "Exported PDF must not be encrypted")
    pages = []
    for page in reader.pages:
        pages.append({"text": page.extract_text() or "", "mediaBox": [str(v) for v in page.mediabox],
                      "cropBox": [str(v) for v in page.cropbox], "rotation": int(page.rotation)})
    require(pages, "Exported PDF has no pages")
    text = "\n".join(page["text"] for page in pages)
    if app == "word":
        require("MEMORY_QA_WORD" in text and "bold and italic" in text,
                "Word PDF is missing the edited marker or original formatted text")
    elif app == "grid":
        require("NORTHSTAR" in text.upper(), "Grid PDF is missing the forecast heading")
    else:
        require(len(pages) == 4, "Deck PDF must contain four pages")
        require("FIELD" in text.upper() and "NOTES" in text.upper(), "Deck PDF is missing FIELD NOTES text")
    return {"path": str(path.resolve()), "bytes": len(raw), "sha256": sha(raw), "pageCount": len(pages),
            "textCharacters": len(text), "pages": pages, "checksPassed": True}


def verify(directory, app):
    document_path = directory / f"{app}-saved.{EXTENSIONS[app]}"
    pdf_path = directory / f"{app}-export.pdf"
    log = directory / "run.log"
    require(log.is_file() and log.stat().st_size > 0, f"Missing workflow run log: {log}")
    log_text = log.read_text(errors="replace")
    require(not re.search(r"\bFAIL\b|panicked at", log_text), "Workflow run log reports a failure")
    return {"directory": str(directory.resolve()), "document": office(document_path, app), "pdf": pdf(pdf_path, app),
            "runLog": {"path": str(log.resolve()), "sha256": sha(log.read_bytes()),
                       "passMarkerPresent": bool(re.search(r"\bPASS\b", log_text)), "lastLines": log_text.splitlines()[-5:]}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", choices=EXTENSIONS)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("optimized", type=Path)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    report = {"scope": SCOPE, "app": args.app, "passed": False}
    code = 0
    try:
        baseline, optimized = verify(args.baseline, args.app), verify(args.optimized, args.app)
        left, right = baseline["document"]["entries"], optimized["document"]["entries"]
        different_entries = [name for name in sorted(left.keys() | right.keys()) if left.get(name) != right.get(name)]
        pdf_equal = baseline["pdf"]["pages"] == optimized["pdf"]["pages"]
        final_state = {}
        for suffix in ("a11y", "ppm"):
            name = f"{args.app}-final.{suffix}"
            first, second = args.baseline / name, args.optimized / name
            if first.exists() or second.exists():
                require(first.is_file() and second.is_file(), f"Native final-state file is missing from one build: {name}")
                first_data, second_data = first.read_bytes(), second.read_bytes()
                final_state[name] = {"baselineSha256": sha(first_data), "optimizedSha256": sha(second_data),
                                     "baselineBytes": len(first_data), "optimizedBytes": len(second_data),
                                     "byteIdentical": first_data == second_data}
        report.update({"baseline": baseline, "optimized": optimized,
                       "comparison": {"officeContentEqual": not different_entries, "differentOfficeEntries": different_entries,
                                      "pdfPageTextAndBoxesEqual": pdf_equal,
                                      "nativeFinalStateFiles": final_state,
                                      "rawOfficeSha256Equal": baseline["document"]["sha256"] == optimized["document"]["sha256"],
                                      "rawPdfSha256Equal": baseline["pdf"]["sha256"] == optimized["pdf"]["sha256"],
                                      "timestampPolicy": "Only dcterms:modified values in docProps/core.xml are ignored for Office content comparison; ZIP container timestamps/compression are outside entry content. All other XML/relationship content is canonicalized and binary entries must match exactly."}})
        require(not different_entries, "Office package content differs between optimization levels")
        require(pdf_equal, "PDF pages, extracted text, page boxes or rotation differ between optimization levels")
        require(all(item["byteIdentical"] for item in final_state.values()), "Native final-state accessibility or rendered pixels differ")
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        code = 1
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    if code:
        print(f"FAIL {args.app}: {report['error']}", file=sys.stderr)
    else:
        comparison = report["comparison"]
        print(f"PASS {args.app}: Office content and PDF page/text checks; "
              f"pages={report['baseline']['pdf']['pageCount']}; "
              f"raw Office identical={comparison['rawOfficeSha256Equal']}; "
              f"raw PDF identical={comparison['rawPdfSha256Equal']}")
    return code


if __name__ == "__main__":
    sys.exit(main())
