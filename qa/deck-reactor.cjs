// Exercise the DeckCraft reactor's shared PM ABI independently of the VM.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { WASI } = require('node:wasi');

const [modulePath, fixturePath, outputPath] = process.argv.slice(2);
assert(modulePath && fixturePath && outputPath, 'Pass the WASI module, a PPTX fixture, and an output PPTX path.');
const wasi = new WASI({ version: 'preview1', args: [], env: {}, returnOnExit: true });
const moduleBytes = fs.readFileSync(modulePath);
const moduleObject = new WebAssembly.Module(moduleBytes);
let app;
let cancelSave = true;
const saves = [];
const bytes = (ptr, size) => new Uint8Array(app.memory.buffer, ptr, size);
const names = new TextDecoder();
const host = {
    save(namePtr, nameLen, dataPtr, dataLen, saveAs) {
        const name = names.decode(bytes(namePtr, nameLen));
        const data = Buffer.from(bytes(dataPtr, dataLen));
        saves.push({ name, data, saveAs });
        return cancelSave ? -1 : 0;
    },
    clipboard_read() { return 0; },
    clipboard_write() { return 1; },
    clipboard_image() { return 1; },
    open_url_native() {},
};
const imports = Object.assign(wasi.getImportObject(), { wc_host: host });
const instance = new WebAssembly.Instance(moduleObject, imports);
wasi.initialize(instance);
app = instance.exports;
assert.equal(app.wc_init(1100, 740), 0);
assert.equal(app.wc_event(20, 1, 0, 0, 0), 0);

let tick = 0;
function frame(width = 1100, height = 740) {
    assert.equal(app.wc_frame(width, height, tick += 150), 0);
    const draw = Buffer.from(bytes(app.wc_draw_ptr(), app.wc_draw_len()));
    assert.equal(draw.toString('ascii', 0, 4), 'WCG1');
    const a11y = Buffer.from(bytes(app.wc_a11y_ptr(), app.wc_a11y_len()));
    assert.equal(a11y.toString('ascii', 0, 4), 'WCA1');
    return { draw, a11y };
}

function key(code, modifiers = 0) {
    assert.equal(app.wc_event(2, code, 1, 0, modifiers), 0);
    assert.equal(app.wc_event(2, code, 0, 0, modifiers), 0);
    return frame();
}

function activate(packet, label) {
    const labels = [];
    for (let offset = 16; offset + 24 <= packet.length;) {
        const low = packet.readUInt32LE(offset);
        const high = packet.readUInt32LE(offset + 4);
        const flags = packet.readUInt32LE(offset + 8);
        const labelSize = packet.readUInt32LE(offset + 16);
        const detailSize = packet.readUInt32LE(offset + 20);
        offset += 24;
        const text = packet.toString('utf8', offset, offset + labelSize);
        offset += labelSize + detailSize;
        if (flags & 1) labels.push(text);
        if (text === label && (flags & 1)) {
            assert.equal(app.wc_event(21, low, high, 0, 0), 0);
            return frame();
        }
    }
    throw new Error('Accessible button not found: ' + label + '. Available: ' + labels.join(', '));
}

frame();
const editor = frame();
assert(editor.draw.readUInt32LE(8) > 1, 'Editor must emit multiple visible meshes.');
assert(editor.a11y.includes('Home'), 'Home ribbon tab must be accessible.');
frame(1440, 900);

key('O'.charCodeAt(0), 2);
assert.equal(app.wc_open_requested(), 1);
assert.equal(app.wc_accept_input(0, 0), 0);
frame();
key('O'.charCodeAt(0), 2);
assert.equal(app.wc_open_requested(), 1);

const name = Buffer.from('C:\\CRAFT\\FIELD.PPTX');
const fixture = fs.readFileSync(fixturePath);
const ptr = app.wc_reserve_input(name.length + fixture.length);
assert(ptr > 0);
bytes(ptr, name.length).set(name);
bytes(ptr + name.length, fixture.length).set(fixture);
assert.equal(app.wc_accept_input(name.length, fixture.length), 0);
frame();

key('N'.charCodeAt(0), 3); // Ctrl+Shift+N inserts a slide.
key('S'.charCodeAt(0), 2);
assert.equal(saves.length, 1);
assert.equal(saves[0].name, name.toString());
assert.equal(saves[0].saveAs, 0);
assert.equal(app.wc_close_request(), 0, 'Cancelled save must leave the edited file dirty.');
frame();
key(27); // Cancel the quit dialog.
cancelSave = false;
key('S'.charCodeAt(0), 2);
assert.equal(saves.length, 2);
const output = saves[1].data;
assert.equal(output.toString('ascii', 0, 2), 'PK');
fs.writeFileSync(outputPath, output);

const reopened = frame();
key(292); // F5
const show = frame();
assert.notDeepEqual(show.draw, reopened.draw);
assert(!show.a11y.includes('Home'), 'The slideshow must hide the editor ribbon.');
key(27); // Escape
assert(frame().a11y.includes('Home'), 'Esc must return to the editor.');
assert.equal(app.wc_close_request(), 1);
const editorWorkflowMemoryBytes = app.memory.buffer.byteLength;

key('P'.charCodeAt(0), 2);
const print = frame();
activate(print.a11y, 'Button - Print');
assert.equal(saves.length, 3, 'Print must hand a PDF to the host.');
assert(saves[2].name.endsWith('.pdf'));
assert.equal(saves[2].data.toString('ascii', 0, 5), '%PDF-');
fs.writeFileSync(outputPath + '.pdf', saves[2].data);

if (process.argv.includes('--require-native-save')) {
    activate(frame().a11y, 'Button - Save As (unchecked)');
    const saveAs = frame();
    cancelSave = true;
    const cancelled = activate(saveAs.a11y, 'Button - Save');
    assert(cancelled.a11y.includes('Save as type'), 'Cancel must keep the Save As page open.');
    cancelSave = false;
    activate(cancelled.a11y, 'Button - Save');
    assert(!frame().a11y.includes('Save as type'), 'Successful save must return to the editor.');
    assert.equal(saves.length, 5);
    assert.equal(saves[3].saveAs, 1);
    assert.equal(saves[4].saveAs, 1);
}
console.log(JSON.stringify({
    test: 'DeckCraft native reactor ABI',
    imports: WebAssembly.Module.imports(moduleObject).map(({ module, name }) => module + '.' + name),
    sampleDrawBytes: editor.draw.length,
    sampleAccessibilityBytes: editor.a11y.length,
    editorWorkflowMemoryBytes,
    wasmLinearMemoryBytes: app.memory.buffer.byteLength,
    outputPptxBytes: output.length,
    printPdfBytes: saves[2].data.length,
    frames: tick / 150,
}, null, 2));
