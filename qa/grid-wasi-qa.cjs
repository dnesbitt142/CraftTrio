const assert = require('node:assert/strict');
const fs = require('node:fs');
const { WASI } = require('node:wasi');

const [modulePath, fixture, output] = process.argv.slice(2);
const compiled = new WebAssembly.Module(fs.readFileSync(modulePath));
const wasi = new WASI({ version: 'preview1', args: [], env: {}, returnOnExit: true });
const importList = WebAssembly.Module.imports(compiled);
console.log('Imports:', importList.map(({ module, name }) => `${module}.${name}`).join(' '));
assert.ok(importList.every(({ module }) => ['wasi_snapshot_preview1', 'wc_host'].includes(module)));
const workbookPath = 'C:\\TRIO\\PILOT.XLSX';
const newPath = 'C:\\TRIO\\PILOT-QA.XLSX';
let api;
let clipboard = {};
let saves = [];
let cancelSave = false;
const memory = () => Buffer.from(api.memory.buffer);
const read = (at, size) => memory().subarray(at, at + size);
const write = (data, offset, cap) => {
    if (!offset) return data.length;
    assert.ok(data.length <= cap);
    data.copy(memory(), offset);
    return data.length;
};
const wc_host = {
    clipboard_read: (kind, at, cap) => write(Buffer.from(clipboard[kind] || ''), at, cap),
    clipboard_write: (text, textLen, html, htmlLen) => {
        clipboard = { 1: read(text, textLen).toString(), 2: read(html, htmlLen).toString() };
        return 1;
    },
    clipboard_image: () => 1,
    open_url_native: () => {},
    save: (name, nameLen, at, size, saveAs, path, cap) => {
        const bytes = Buffer.from(read(at, size));
        assert.equal(bytes.subarray(0, 2).toString(), 'PK');
        saves.push({ name: read(name, nameLen).toString(), saveAs, bytes });
        if (cancelSave) return -1;
        return saveAs ? write(Buffer.from(newPath), path, cap) : 0;
    },
};
const instance = new WebAssembly.Instance(compiled, { ...wasi.getImportObject(), wc_host });
api = instance.exports;
wasi.initialize(instance);
let clock = 0;
const frame = (w = 1144, h = 688) => {
    assert.equal(api.wc_frame(w, h, clock += 300), 0);
    const draw = read(api.wc_draw_ptr(), api.wc_draw_len());
    assert.equal(draw.subarray(0, 4).toString(), 'WCG1');
    return draw.length;
};
const event = (...args) => assert.equal(api.wc_event(...args), 0);
const key = (key, mask = 0) => { event(2, key, 1, 0, mask); event(2, key, 0, 0, 0); frame(); };
const type = (contents) => {
    for (const ch of contents) {
        event(2, ch.codePointAt(0), 1, 0, 0);
        assert.equal(api.wc_text(ch.codePointAt(0)), 0);
        event(2, ch.codePointAt(0), 0, 0, 0);
    }
    frame();
};
function open(path, file) {
    const name = Buffer.from(path);
    const at = api.wc_reserve_input(name.length + file.length);
    assert.ok(at);
    write(Buffer.concat([name, file]), at, name.length + file.length);
    assert.equal(api.wc_accept_input(name.length, file.length), 0);
    frame();
    frame();
    frame();
}
assert.equal(api.wc_init(1144, 688), 0);
event(20, 1, 0, 0, 0);
const startup = [frame(), frame(), frame(), frame()];
const accessibility = read(api.wc_a11y_ptr(), api.wc_a11y_len());
assert.equal(accessibility.subarray(0, 4).toString(), 'WCA1');
assert.ok(accessibility.includes('Button - Paste') && accessibility.includes('Bold (Ctrl+B)'));
assert.ok(frame(1280, 960) > 32);
console.log('Sales dashboard, accessibility and 1280×960 resize rendered', startup);
console.log('WASI linear memory after startup', api.memory.buffer.byteLength, 'bytes');
key(79, 2);
assert.equal(api.wc_open_requested(), 1);
open(workbookPath, fs.readFileSync(fixture));
key(260, 2);
type('=20+22');
key(13);
key(258);
key(67, 2);
assert.equal(clipboard[1]?.trim(), "42");
assert.match(clipboard[2], /<table/);
key(257);
clipboard = { 1: '314159' };
key(86, 2);
key(67, 2);
assert.match(clipboard[1], /314,?159/);
cancelSave = true;
key(83, 3);
assert.equal(saves.length, 1);
assert.equal(saves[0].saveAs, 1);
cancelSave = false;
key(83, 2);
assert.equal(saves.length, 2);
assert.equal(saves[1].name, workbookPath);
assert.equal(saves[1].saveAs, 0);
key(83, 3);
assert.equal(saves.length, 3);
assert.equal(saves[2].saveAs, 1);
fs.writeFileSync(output, saves[2].bytes);
assert.equal(api.wc_close_request(), 0);
frame();
assert.equal(api.wc_should_close(), 1);
console.log('Native open, formula edit, plain/HTML clipboard, external paste, Save As cancellation, Save, Save As and clean close completed.');
