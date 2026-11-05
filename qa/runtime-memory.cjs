// Exercise the rebuilt Rust bridge engines using WASI Preview1 on the build host.
// This validates engine behavior and page-management requests, not OS/2 RAM/GPI.
// Usage: node qa/runtime-memory.cjs word|grid|deck MODULE FIXTURE OUTPUT_DIR
// Optional --pdf exports through the application's own UI; --capture-draw keeps
// all startup packets in sequence for native renderer comparisons outside the VM.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');
const { WASI } = require('node:wasi');

const [kind, modulePath, fixturePath, outputDirectory, ...options] = process.argv.slice(2);
assert(['word', 'grid', 'deck'].includes(kind) && modulePath && fixturePath && outputDirectory,
    'Pass word|grid|deck MODULE FIXTURE OUTPUT_DIR [--pdf] [--capture-draw]');
const outputDir = path.resolve(outputDirectory);
fs.mkdirSync(outputDir, { recursive: true });
const extension = { word: 'docx', grid: 'xlsx', deck: 'pptx' }[kind];
const marker = 'MEMORY_QA_WORD';
const moduleBytes = fs.readFileSync(modulePath);
const compiled = new WebAssembly.Module(moduleBytes);
const imports = WebAssembly.Module.imports(compiled).map(x => x.module + '.' + x.name);
assert(imports.every(name => name.startsWith('wc_host.') || name.startsWith('wasi_snapshot_preview1.')));
let api;
let clock = 0;
let sequence = 0;
let stage = 'instantiate';
const startupCaptures = [];
const owners = new Map();
const pageEvents = [];
const saves = [];
const frames = [];
const checkpoints = [];
const textures = new Map();
let clipboard = {};
const bytes = (offset, size) => {
    offset >>>= 0;
    size >>>= 0;
    assert(api && offset <= api.memory.buffer.byteLength && size <= api.memory.buffer.byteLength - offset,
        `host memory span ${offset}+${size}`);
    return Buffer.from(api.memory.buffer, offset, size);
};
const read = (offset, size) => Buffer.from(bytes(offset, size));
const write = (data, offset, capacity) => {
    if (!(offset >>> 0)) return data.length;
    assert(data.length <= (capacity >>> 0));
    data.copy(bytes(offset, capacity));
    return data.length;
};

// Full owner spans are page-aligned and still owned by the Rust allocator.
// Only suffixes of their committed prefixes may be decommitted. ArrayBuffer
// pages cannot really be released here; zeroing simulates discarded contents.
function memoryPages(operation, offset, length) {
    operation >>>= 0;
    offset >>>= 0;
    length >>>= 0;
    assert(length && !(offset & 4095) && !(length & 4095), 'aligned nonempty memory_pages request');
    bytes(offset, length);
    const end = offset + length;
    let owner;
    if (operation === 0) {
        assert(![...owners.values()].some(x => offset < x.end && end > x.start), 'registered owners overlap');
        owner = { start: offset, end, committed: length };
        owners.set(offset, owner);
    } else {
        owner = [...owners.values()].find(x => offset >= x.start && end <= x.end);
        assert(owner, 'page operation must belong to a registered owner');
        if (operation === 1) {
            assert.equal(offset, owner.start + owner.committed, 'commit extends the committed prefix');
            bytes(offset, length).fill(0);
            owner.committed += length;
        } else if (operation === 2) {
            assert.equal(end, owner.start + owner.committed, 'decommit removes a suffix');
            bytes(offset, length).fill(0);
            owner.committed -= length;
        } else if (operation === 3) {
            assert.equal(offset, owner.start);
            assert.equal(end, owner.end);
            assert.equal(owner.committed, owner.end - owner.start, 'owner must be fully committed before unregister/free');
            owners.delete(owner.start);
        } else assert.fail('Unknown memory_pages operation ' + operation);
    }
    pageEvents.push({ operation, offset, length, stage });
    return 0;
}

const host = {
    memory_pages: memoryPages,
    clipboard_read: (format, at, cap) => write(Buffer.from(clipboard[format] || ''), at, cap),
    clipboard_write: (text, textLength, html, htmlLength) => {
        clipboard = { 1: bytes(text, textLength).toString(), 2: bytes(html, htmlLength).toString() };
        return 1;
    },
    clipboard_image: () => 1,
    open_url_native: () => {},
    net_socket: () => -1,
    net_release: () => -1,
    print_queues: () => 0,
    print_start: () => -2,
    print_page: () => -1,
    print_finish: () => 0,
    save: (name, nameLength, at, size, saveAs) => {
        const fileName = bytes(name, nameLength).toString();
        const data = read(at, size);
        assert(data.length > 0);
        assert(data.subarray(0, 2).toString() === 'PK' || data.subarray(0, 5).toString() === '%PDF-',
            'fixture saves must be OOXML or PDF');
        saves.push({ name: fileName, data, saveAs });
        return 0;
    },
};
const wasi = new WASI({ version: 'preview1', args: [], env: {}, returnOnExit: true });
const instance = new WebAssembly.Instance(compiled, { ...wasi.getImportObject(), wc_host: host });
api = instance.exports;
wasi.initialize(instance);
for (const name of ['wc_init', 'wc_frame', 'wc_event', 'wc_text', 'wc_draw_ptr', 'wc_draw_len',
    'wc_draw_capacity', 'wc_input_capacity', 'wc_draw_consumed', 'wc_a11y_ptr', 'wc_a11y_len']) {
    assert.equal(typeof api[name], 'function', 'Required bridge export ' + name);
}

function checkpoint(name) {
    const registered = [...owners.values()].reduce((n, x) => n + x.end - x.start, 0);
    const decommitted = [...owners.values()].reduce((n, x) => n + x.end - x.start - x.committed, 0);
    const record = {
        name, arenaBytes: api.memory.buffer.byteLength, registeredBytes: registered,
        simulatedDecommittedBytes: decommitted,
        arenaMinusSimulatedDecommitBytes: api.memory.buffer.byteLength - decommitted,
        drawCapacity: api.wc_draw_capacity() >>> 0, inputCapacity: api.wc_input_capacity() >>> 0,
    };
    checkpoints.push(record);
    return record;
}

function accessible(packet) {
    assert.equal(packet.subarray(0, 4).toString(), 'WCA1');
    const rows = [];
    let at = 16;
    while (at < packet.length) {
        assert(at + 24 <= packet.length, 'a11y row header');
        const row = { low: packet.readUInt32LE(at), high: packet.readUInt32LE(at + 4),
            flags: packet.readUInt32LE(at + 8), depth: packet.readUInt32LE(at + 12) };
        const labelLength = packet.readUInt32LE(at + 16);
        const detailLength = packet.readUInt32LE(at + 20);
        at += 24;
        assert(labelLength <= 1024 && detailLength <= 32768 && labelLength + detailLength <= packet.length - at);
        row.label = packet.toString('utf8', at, at + labelLength);
        at += labelLength;
        row.detail = packet.toString('utf8', at, at + detailLength);
        at += detailLength;
        rows.push(row);
    }
    return rows;
}

function parseDraw(packet) {
    assert.equal(packet.subarray(0, 4).toString(), 'WCG1');
    assert(packet.length >= 16 && packet.length <= 32 * 1024 * 1024);
    const setCount = packet.readUInt32LE(4), meshCount = packet.readUInt32LE(8), freeCount = packet.readUInt32LE(12);
    assert(setCount <= 512 && meshCount <= 8192 && freeCount <= 512);
    let at = 16, eligibleMaskBytes = 0, uploadedBytes = 0, largestTextureBytes = 0;
    function take(size) { assert(Number.isSafeInteger(size) && size >= 0 && size <= packet.length - at); const p = at; at += size; return p; }
    for (let n = 0; n < setCount; ++n) {
        const p = take(20);
        const id = packet.readUInt32LE(p), x = packet.readUInt32LE(p + 4), y = packet.readUInt32LE(p + 8);
        const width = packet.readUInt32LE(p + 12), height = packet.readUInt32LE(p + 16);
        assert(width && height && width <= 8192 && height <= 8192);
        const size = width * height * 4, data = take(size);
        uploadedBytes += size;
        largestTextureBytes = Math.max(largestTextureBytes, size);
        if (x === 0xffffffff && y === 0xffffffff) textures.set(id, { width, height });
        else {
            const texture = textures.get(id);
            assert(texture && x <= texture.width - width && y <= texture.height - height);
        }
        let mask = true;
        for (let i = data; i < data + size; i += 4) {
            if (packet[i] !== packet[i + 1] || packet[i] !== packet[i + 2] || packet[i] !== packet[i + 3]) { mask = false; break; }
        }
        if (mask) eligibleMaskBytes += size;
    }
    for (let n = 0; n < meshCount; ++n) {
        const p = take(28), id = packet.readUInt32LE(p);
        const vertices = packet.readUInt32LE(p + 20), indices = packet.readUInt32LE(p + 24);
        assert(vertices <= 1048576 && indices <= 3145728 && indices % 3 === 0);
        if (indices) assert(textures.has(id), 'draw references known texture');
        const vertexAt = take(vertices * 20), indexAt = take(indices * 4);
        for (let i = 0; i < vertices; ++i)
            for (let f = 0; f < 4; ++f) assert(Number.isFinite(packet.readFloatLE(vertexAt + i * 20 + f * 4)));
        for (let i = 0; i < indices; ++i) assert(packet.readUInt32LE(indexAt + i * 4) < vertices);
    }
    for (let n = 0; n < freeCount; ++n) textures.delete(packet.readUInt32LE(take(4)));
    assert.equal(at, packet.length, 'complete WCG1 packet');
    return { setCount, meshCount, freeCount, uploadedBytes, largestTextureBytes, eligibleMaskBytes };
}

function frame(width = 1144, height = 688) {
    assert.equal(api.wc_frame(width, height, clock += 200), 0, stage + ': frame');
    const pointer = api.wc_draw_ptr() >>> 0, size = api.wc_draw_len() >>> 0;
    const capacity = api.wc_draw_capacity() >>> 0;
    assert(capacity >= size);
    const draw = read(pointer, size);
    assert.equal(api.wc_draw_ptr() >>> 0, pointer, 'draw pointer stable until consumed');
    assert.equal(api.wc_draw_len() >>> 0, size, 'draw length stable until consumed');
    const parsed = parseDraw(draw);
    const a11y = read(api.wc_a11y_ptr(), api.wc_a11y_len());
    const rows = accessible(a11y);
    assert.equal(api.wc_draw_consumed(), 0);
    assert.equal(api.wc_draw_len(), 0, 'consumed draw packet is empty');
    const record = { sequence: ++sequence, stage, width, height, bytes: size, capacity,
        capacityAfterConsume: api.wc_draw_capacity() >>> 0, ...parsed };
    frames.push(record);
    if (stage === 'startup' && options.includes('--capture-draw')) {
        const file = kind + '-startup-' + String(record.sequence).padStart(3, '0') + '.wcg';
        fs.writeFileSync(path.join(outputDir, file), draw);
        startupCaptures.push({ file, sequence: record.sequence, width, height, bytes: draw.length,
            sha256: crypto.createHash('sha256').update(draw).digest('hex') });
    }
    return { rows, draw, record };
}
function event(...args) { assert.equal(api.wc_event(...args), 0); }
function key(code, modifiers = 0) {
    event(2, code, 1, 0, modifiers);
    event(2, code, 0, 0, 0);
    return frame();
}
function type(text) {
    for (const ch of text) assert.equal(api.wc_text(ch.codePointAt(0)), 0);
    return frame();
}
function activate(rows, match) {
    const candidates = rows.filter(row => (row.flags & 1) && !(row.flags & 4) &&
        (typeof match === 'string' ? row.label === match : match.test(row.label)));
    assert(candidates.length, 'Accessible command not found: ' + match + '\n' + rows.map(x => x.label).join('\n'));
    const row = candidates.at(-1);
    event(21, row.low, row.high, 0, 0);
    frame();
    return frame();
}
function saveRecords(start, ext) { return saves.slice(start).filter(x => x.data.subarray(0, ext === 'pdf' ? 5 : 2).toString() === (ext === 'pdf' ? '%PDF-' : 'PK')); }

const result = { kind, moduleSha256: crypto.createHash('sha256').update(moduleBytes).digest('hex'), imports,
    scope: 'Actual Rust engine in Node WASI; page requests simulated, no OS/2 guest/GPI/physical RAM test',
    frames, checkpoints, startupCaptures, passed: false };
try {
    stage = 'startup';
    assert.equal(api.wc_init(1144, 688), 0);
    event(20, 1, 0, 0, 0);
    let current;
    for (let n = 0; n < 8; ++n) current = frame();
    const startupText = current.rows.map(row => row.label + '\n' + row.detail).join('\n');
    if (kind === 'word') {
        const docs = current.rows.filter(row => row.flags & 16);
        assert(docs.length, 'blank Word document must be accessible');
        assert(docs.every(row => row.detail.trim() === ''), 'Word starts with a blank document');
        assert(!startupText.includes('The Open Studio Handbook'));
    } else if (kind === 'grid') assert(!/Sales dashboard|Quarterly sales|Sales Dashboard/i.test(startupText), 'Grid starts without the sales sample');
    else assert(!startupText.includes('Northern Lights'), 'Deck starts without the sample presentation');
    result.startupDocumentRows = current.rows.filter(row => row.flags & 16).map(({ label, detail }) => ({ label, detail }));
    checkpoint('blank startup');
    frame(1280, 751);
    frame(640, 480);
    frame();

    stage = 'cancelled large input';
    key(79, 2);
    assert.equal(api.wc_open_requested(), 1);
    const pending = api.wc_reserve_input(3 << 20) >>> 0;
    assert(pending && api.wc_input_capacity() >= 3 << 20);
    checkpoint('large input reserved');
    assert.equal(api.wc_accept_input(0, 0), 0);
    assert.equal(api.wc_input_capacity(), 0, 'cancelled input releases its Vec');
    frame();
    checkpoint('large input cancelled');
    assert.equal(api.wc_reserve_input((16 << 20) + 1025), 0, 'input above file+path limit rejected');
    assert.equal(api.wc_input_capacity(), 0);

    stage = 'fixture open';
    key(79, 2);
    assert.equal(api.wc_open_requested(), 1);
    const fileName = Buffer.from('C:\\CRAFT\\MEMORY.' + extension.toUpperCase());
    const fixture = fs.readFileSync(fixturePath);
    const offset = api.wc_reserve_input(fileName.length + fixture.length) >>> 0;
    assert(offset);
    fileName.copy(bytes(offset, fileName.length));
    fixture.copy(bytes(offset + fileName.length, fixture.length));
    assert.equal(api.wc_accept_input(fileName.length, fixture.length), 0);
    assert.equal(api.wc_input_capacity(), 0, 'accepted input transfers ownership out of pending storage');
    for (let n = 0; n < 4; ++n) current = frame();
    checkpoint('fixture opened');

    stage = 'fixture edit';
    if (kind === 'word') {
        const doc = current.rows.find(row => (row.flags & 16) && (row.flags & 3));
        if (doc) { event(21, doc.low, doc.high, 0, 0); frame(); }
        key(260, 2); // Ctrl+Home.
        type(marker + ' ');
    } else if (kind === 'grid') {
        key(260, 2);
        type('=20+22');
        key(13);
        key(258);
        key(67, 2);
        assert.equal(clipboard[1]?.trim(), '42', 'edited formula recalculates');
    } else key(78, 3); // Ctrl+Shift+N inserts a slide.
    const beforeSave = saves.length;
    key(83, 2);
    for (let n = 0; n < 3 && saves.length === beforeSave; ++n) current = frame();
    const documentSave = saveRecords(beforeSave, extension).at(-1);
    assert(documentSave, 'native Save callback must receive the edited file');
    const documentPath = path.join(outputDir, kind + '-memory-saved.' + extension);
    fs.writeFileSync(documentPath, documentSave.data);
    checkpoint('fixture saved');
    result.outputDocument = { path: documentPath, bytes: documentSave.data.length,
        sha256: crypto.createHash('sha256').update(documentSave.data).digest('hex') };

    // Use independent ZIP/XML readers, not the engine's own serializer.
    const check = String.raw`
import sys, zipfile, xml.etree.ElementTree as ET
kind, filename = sys.argv[1:]
with zipfile.ZipFile(filename) as z:
 assert z.testzip() is None
 if kind == 'word':
  xml=z.read('word/document.xml').decode()
  text=''.join(ET.fromstring(xml).itertext())
  assert 'MEMORY_QA_WORD' in text
  assert 'bold and italic' in text and '<w:tbl' in xml
 elif kind == 'grid':
  assert len([n for n in z.namelist() if n.startswith('xl/charts/chart') and n.endswith('.xml')]) >= 1
  ns={'s':'http://schemas.openxmlformats.org/spreadsheetml/2006/main'}
  sheets=[ET.fromstring(z.read(n)) for n in z.namelist() if n.startswith('xl/worksheets/sheet') and n.endswith('.xml')]
  assert any(c.find('s:f',ns) is not None and c.find('s:f',ns).text == '20+22' and c.find('s:v',ns).text == '42' for sh in sheets for c in sh.findall('.//s:c',ns))
 else:
  ns={'p':'http://schemas.openxmlformats.org/presentationml/2006/main'}
  assert len(ET.fromstring(z.read('ppt/presentation.xml')).findall('.//p:sldId',ns)) == 4
print('Independent ZIP CRC/XML verification PASS: '+kind)
`;
    result.independentDocumentCheck = execFileSync(process.env.PYTHON || 'python3', ['-c', check, kind, documentPath], { encoding: 'utf8' }).trim();

    if (options.includes('--pdf')) {
        stage = 'PDF export';
        const beforePdf = saves.length;
        if (kind === 'word') {
            current = activate(frame().rows, /^Button - File(?: \(.*\))?$/);
            current = activate(current.rows, 'Button - Export');
            current = activate(current.rows, 'Button - PDF document (*.pdf)');
        } else if (kind === 'grid') {
            current = activate(frame().rows, /^Button - File$/);
            current = activate(current.rows, /^Button - Export(?: \(.*\))?$/);
            current = activate(current.rows, /^Button - Create PDF$/);
        } else {
            current = key(80, 2);
            current = activate(current.rows, /^Button - Print$/);
        }
        for (let n = 0; n < 4 && saveRecords(beforePdf, 'pdf').length === 0; ++n) current = frame();
        const pdf = saveRecords(beforePdf, 'pdf').at(-1);
        assert(pdf, 'application export/print must produce a PDF');
        const pdfPath = path.join(outputDir, kind + '-memory-export.pdf');
        fs.writeFileSync(pdfPath, pdf.data);
        result.outputPdf = { path: pdfPath, bytes: pdf.data.length,
            sha256: crypto.createHash('sha256').update(pdf.data).digest('hex') };
        const pdfCheck = String.raw`
import sys, json
from pypdf import PdfReader
kind, filename = sys.argv[1:]
r=PdfReader(filename)
text='\n'.join(page.extract_text() or '' for page in r.pages)
assert len(r.pages) >= 1
if kind == 'word': assert 'MEMORY_QA_WORD' in text and 'bold and italic' in text
elif kind == 'grid': assert 'NORTHSTAR' in text.upper()
else: assert len(r.pages) == 4 and 'FIELD' in text.upper() and 'NOTES' in text.upper()
print(json.dumps({'pages':len(r.pages),'textCharacters':len(text),'passed':True}))
`;
        result.independentPdfCheck = JSON.parse(execFileSync(process.env.PYTHON || 'python3',
            ['-c', pdfCheck, kind, pdfPath], { encoding: 'utf8' }));
        checkpoint('PDF exported');
        key(27);
    }
    stage = 'settled frames';
    for (let n = 0; n < 8; ++n) frame();
    checkpoint('settled after edit/export');
    result.pageRequests = {
        registrations: pageEvents.filter(x => x.operation === 0).length,
        commits: pageEvents.filter(x => x.operation === 1).length,
        decommits: pageEvents.filter(x => x.operation === 2).length,
        unregisters: pageEvents.filter(x => x.operation === 3).length,
        committedBytesRequested: pageEvents.filter(x => x.operation === 1).reduce((n, x) => n + x.length, 0),
        decommittedBytesRequested: pageEvents.filter(x => x.operation === 2).reduce((n, x) => n + x.length, 0),
    };
    assert(result.pageRequests.decommits > 0, 'large allocation cancellation/consumption must request reclamation');
    result.passed = true;
    if (startupCaptures.length) fs.writeFileSync(path.join(outputDir, kind + '-startup-captures.json'),
        JSON.stringify({ scope: 'Host WASI WCG1 packets, not OS/2 screenshots', kind, frames: startupCaptures }, null, 2) + '\n');
    console.log(JSON.stringify(result, null, 2));
} catch (error) {
    result.failure = { stage, message: error.message, stack: error.stack };
    console.error(error.stack);
    process.exitCode = 1;
} finally {
    if (startupCaptures.length) fs.writeFileSync(path.join(outputDir, kind + '-startup-captures.json'),
        JSON.stringify({ scope: 'Host WASI WCG1 packets, not OS/2 screenshots', kind, frames: startupCaptures }, null, 2) + '\n');
    result.pageEvents = pageEvents;
    fs.writeFileSync(path.join(outputDir, kind + '-runtime-memory.json'), JSON.stringify(result, null, 2) + '\n');
}
