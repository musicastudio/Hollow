// node editor/test.mjs : checks the editor's pure logic (logic.mjs) with plain asserts.
import assert from 'node:assert/strict';
import zlib from 'node:zlib';
import * as L from './logic.mjs';

let count = 0;
async function test(name, fn) {
  try { await fn(); count++; } catch (e) { console.error(`FAIL ${name}\n`, e); process.exitCode = 1; }
}
const img = (w, h, f = () => [0, 0, 0, 0]) => {
  const o = L.blank(w, h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) o.data.set(f(x, y), (y * w + x) * 4);
  return o;
};

await test('tile rects on both axes, clamped', () => {
  assert.deepEqual(L.tileRect(10, 40, 4, 'y', 2), [0, 20, 10, 10]);
  assert.deepEqual(L.tileRect(40, 10, 4, 'x', 3), [30, 0, 10, 10]);
  assert.deepEqual(L.tileRect(10, 40, 4, 'y', -1), [0, 0, 10, 10]);
  assert.deepEqual(L.tileRect(10, 40, 4, 'y', 9), [0, 30, 10, 10]);
  assert.deepEqual(L.tileRect(10, 40), [0, 0, 10, 40]);
  assert.equal(L.valueTile(128, 0.5), 64);
  assert.equal(L.valueTile(128, 1), 127);
  assert.equal(L.valueTile(1, 0.7), 0);
});

await test('button state tile index', () => {
  assert.equal(L.buttonTile({}, 2, 0), 0);
  assert.equal(L.buttonTile({}, 2, 1), 1);
  assert.equal(L.buttonTile({}, 2, 5), 1); // value capped at states - 1
  // groups [normal][pressed][hover], 2 states each
  const hp = { hoverTiles: true, pressedTiles: true };
  assert.equal(L.buttonTile(hp, 6, 0), 0);
  assert.equal(L.buttonTile(hp, 6, 1, { pressed: true }), 3);
  assert.equal(L.buttonTile(hp, 6, 1, { pressed: true, hover: true }), 3); // pressed wins over hover
  assert.equal(L.buttonTile(hp, 6, 1, { hover: true }), 5);
  assert.equal(L.buttonTile(hp, 6, 7, { hover: true }), 5); // min(value, states - 1) before the group offset
  assert.equal(L.buttonTile({ ...hp, disabledTile: true }, 7, 1, { disabled: true }), 6);
  assert.equal(L.buttonTile({ ...hp, disabledTile: true }, 7, 1, { hover: true }), 5);
  assert.equal(L.buttonTile({ pressedTiles: true }, 4, 0, { pressed: true, hover: true }), 2);
  assert.equal(L.buttonTile({ hoverTiles: true }, 4, 1, { hover: true }), 3); // no pressed group: hover is group 1
  assert.equal(L.buttonTile({ hoverTiles: true }, 4, 1, { pressed: true }), 1); // no pressed tiles: stays normal
  assert.equal(L.buttonTile({ disabledTile: true }, 3, 1, { disabled: false }), 1);
  assert.equal(L.buttonTile({ disabledTile: false }, 3, 1, { disabled: true }), 1); // no disabled tile: normal
});

await test('nine-slice source and destination rects', () => {
  const p = L.nineSlice([0, 0, 30, 30], [10, 10, 10, 10], [100, 100, 50, 40]);
  assert.equal(p.length, 9);
  assert.deepEqual(p[0], [[0, 0, 10, 10], [100, 100, 10, 10]]);
  assert.deepEqual(p[2], [[20, 0, 10, 10], [140, 100, 10, 10]]);
  assert.deepEqual(p[4], [[10, 10, 10, 10], [110, 110, 30, 20]]);
  assert.deepEqual(p[8], [[20, 20, 10, 10], [140, 130, 10, 10]]);
  // top and bottom 0: the vertical axis scales as a whole
  assert.deepEqual(L.nineSlice([0, 30, 20, 10], [0, 5, 0, 5], [0, 0, 40, 20]), [
    [[0, 30, 5, 10], [0, 0, 5, 20]], [[5, 30, 10, 10], [5, 0, 30, 20]], [[15, 30, 5, 10], [35, 0, 5, 20]]]);
  // smaller than the fixed bands: they shrink, the middle vanishes
  assert.deepEqual(L.nineSlice([0, 0, 20, 10], [0, 10, 0, 10], [0, 0, 10, 10]).map(q => q[1]), [[0, 0, 5, 10], [5, 0, 5, 10]]);
  // imagePieces: same size or no slice draws 1:1 clipped to the rect
  assert.deepEqual(L.imagePieces([0, 10, 30, 10], [2, 2, 2, 2], [5, 5, 30, 10]), [[[0, 10, 30, 10], [5, 5, 30, 10]]]);
  assert.deepEqual(L.imagePieces([0, 0, 30, 10], null, [5, 5, 50, 4]), [[[0, 0, 30, 4], [5, 5, 30, 4]]]);
  assert.equal(L.imagePieces([0, 0, 30, 10], [2, 2, 2, 2], [5, 5, 60, 10]).length, 3 * 3 - 0);
});

await test('slider handle position', () => {
  assert.deepEqual(L.sliderPos([10, 20, 12, 100], 12, 10, 0), [10, 110]);
  assert.deepEqual(L.sliderPos([10, 20, 12, 100], 12, 10, 1), [10, 21]);
  assert.deepEqual(L.sliderPos([10, 20, 100, 12], 10, 12, 0.5, false), [54, 20]);
});

await test('tile split and join', () => {
  const strip = img(4, 12, (x, y) => [x, y, 0, 255]);
  const tiles = L.splitTiles(strip, 3, 'y');
  assert.equal(tiles.length, 3);
  assert.deepEqual([tiles[1].width, tiles[1].height, tiles[1].data[1]], [4, 4, 4]);
  assert.deepEqual(L.joinTiles(tiles, 'y'), strip);
  const wide = L.joinTiles(tiles, 'x');
  assert.deepEqual([wide.width, wide.height], [12, 4]);
  assert.deepEqual(L.splitTiles(wide, 3, 'x'), tiles);
  assert.throws(() => L.joinTiles([img(2, 2), img(2, 3)]), /tile 1 is 2x3/);
});

// A font of 256 glyphs, glyph i being (i % 3) + 1 px wide, with one pixel coloured by its code.
const glyphs = Array.from({ length: 256 }, (_, i) => img((i % 3) + 1, 5, (x, y) => (x === 0 && y === 1 && i !== 32 ? [i, 0, 0, 255] : [0, 0, 0, 0])));

await test('glyph strip with a marker row: build, parse, split back', () => {
  const strip = L.buildFont(glyphs);
  assert.deepEqual([strip.width, strip.height], [glyphs.reduce((s, g) => s + g.width, 0), 6]);
  let marks = 0;
  for (let x = 0; x < strip.width; x++) if (strip.data[x * 4 + 3]) marks++;
  assert.equal(marks, 256);
  const f = L.parseFont(strip);
  assert.equal(f.marker, true);
  assert.deepEqual([f.y, f.h], [1, 5]);
  assert.deepEqual(f.glyphs.slice(0, 4), [[0, 1], [1, 2], [3, 3], [6, 1]]);
  assert.deepEqual(L.fontGlyphs(strip, f), glyphs);
  assert.equal(L.isEmpty(L.fontGlyphs(strip, f)[32]), true);
  assert.equal(L.isEmpty(L.fontGlyphs(strip, f)[65]), false);
  assert.throws(() => L.buildFont(glyphs.slice(1)), /256/);
  assert.throws(() => L.buildFont([img(1, 4), ...glyphs.slice(1)]), /glyph 1 is 5 px tall/);
  assert.deepEqual([L.fitGlyph(img(0, 5), 7).width, L.fitGlyph(img(2, 9), 7).height], [1, 7]);
});

await test('fixed-width font and text layout', () => {
  const fixed = L.parseFont(img(512, 7, (x, y) => (y === 0 && x < 10 ? [255, 255, 255, 255] : [0, 0, 0, 0])));
  assert.deepEqual([fixed.marker, fixed.y, fixed.h, fixed.glyphs[65]], [false, 0, 7, [130, 2]]);
  const f = L.parseFont(L.buildFont(glyphs));
  const w = c => f.glyphs[c][1];
  const q = L.layoutText(f, 'AB', [10, 20, 40, 30], { align: 'center', valign: 'middle' });
  const lw = w(65) + w(66);
  assert.deepEqual(q.map(g => [g.code, g.dx, g.dy]), [[65, 10 + Math.trunc((40 - lw) / 2), 20 + Math.trunc((30 - 5) / 2)], [66, 10 + Math.trunc((40 - lw) / 2) + w(65), 32]]);
  const r = L.layoutText(f, 'A\\nBB', [0, 0, 20, 20], { align: 'right', valign: 'bottom' }, [1, 2, 3, 4]);
  assert.deepEqual(r.map(g => [g.dx, g.dy]), [[17 - w(65), 6], [17 - 2 * w(66), 11], [17 - w(66), 11]]);
  assert.equal(L.layoutText(f, '€', [0, 0, 9, 9])[0].code, 63); // above Latin-1: '?'
  assert.equal(L.layoutText(f, 'é', [0, 0, 9, 9])[0].code, 233);
  // negative space centres toward zero, as C++ integer division does
  assert.equal(L.layoutText(f, 'BBBBB', [0, 0, 1, 5], { align: 'center' })[0].dx, Math.trunc((1 - 5 * w(66)) / 2));
});

await test('PNG encode and decode', async () => {
  const pic = img(7, 5, (x, y) => [x * 30, y * 50, (x * y * 17) & 255, (x + y) * 20]);
  const bytes = await L.pngEncode(pic);
  assert.deepEqual([...bytes.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
  assert.deepEqual(await L.pngDecode(bytes), pic);
  // hand-made files in other colour types, checked against zlib
  const chunk = (type, data) => {
    const b = Buffer.alloc(12 + data.length);
    b.writeUInt32BE(data.length, 0); b.write(type, 4, 'latin1'); Buffer.from(data).copy(b, 8);
    b.writeUInt32BE(zlib.crc32(b.subarray(4, 8 + data.length)), 8 + data.length);
    return b;
  };
  const png = (w, h, depth, ctype, rows, extra = []) => {
    const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = depth; ihdr[9] = ctype;
    return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), ...extra,
      chunk('IDAT', zlib.deflateSync(Buffer.concat(rows.map(r => Buffer.from([0, ...r]))))), chunk('IEND', [])]);
  };
  // 2-bit palette with tRNS: indices 0 1 2 3 in one row
  const pal = await L.pngDecode(png(4, 1, 2, 3, [[0b00011011]], [chunk('PLTE', [255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9]), chunk('tRNS', [0, 128])]));
  assert.deepEqual([...pal.data], [255, 0, 0, 0, 0, 255, 0, 128, 0, 0, 255, 255, 9, 9, 9, 255]);
  // 16-bit grey + alpha keeps the high bytes
  const ga = await L.pngDecode(png(1, 1, 16, 4, [[0x12, 0x34, 0xab, 0xcd]]));
  assert.deepEqual([...ga.data], [0x12, 0x12, 0x12, 0xab]);
  // 8-bit RGB with a transparent colour key
  const rgb = await L.pngDecode(png(2, 1, 8, 2, [[1, 2, 3, 4, 5, 6]], [chunk('tRNS', [0, 4, 0, 5, 0, 6])]));
  assert.deepEqual([...rgb.data], [1, 2, 3, 255, 4, 5, 6, 0]);
  // 1-bit grey scales to 0/255
  const bw = await L.pngDecode(png(3, 1, 1, 0, [[0b10100000]]));
  assert.deepEqual([...bw.data], [255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255]);
  await assert.rejects(L.pngDecode(new Uint8Array([1, 2, 3, 4])), /not a PNG/);
});

// A small skin used by the reference tests.
const skinDocs = () => ({
  'skin.json': { format: 1, name: 'T', root: 'main', vars: { timbre: 'tl' }, images: { 'knobs/big': { tiles: 64 }, 'panels/box': { slice: [2, 2, 2, 2] } } },
  'params.json': [{ id: 'op.a.ratio', name: 'A Ratio' }, { id: 'op.b.ratio', name: 'B Ratio' }, { id: 'arp.tempo', name: 'Tempo', min: 20, max: 300 }],
  'views/main.json': { size: [200, 100], fill: { image: 'panels/box' }, widgets: [
    { type: 'embed', name: 'pages', rect: [0, 0, 150, 100], view: 'page_op', vars: { op: 'a' } },
    { type: 'button', name: 'nav_op', rect: [150, 0, 50, 20], fill: { image: 'panels/box' }, text: { text: 'Op', font: 'small' }, action: { goto: 'page_op', stack: 'pages', vars: { op: 'b' } } },
    { type: 'button', name: 'nav_arp', rect: [150, 20, 50, 20], action: { goto: 'page_arp', stack: 'pages' } },
    { type: 'button', name: 'about', rect: [150, 40, 50, 20], action: { toggle: 'about_box' } },
    { type: 'plate', name: 'about_box', rect: [10, 10, 100, 50], hidden: true, text: { text: 'About', font: 'small' } },
  ] },
  'views/page_op.json': { size: [150, 100], widgets: [
    { type: 'dial', name: 'ratio', rect: [0, 0, 40, 40], image: 'knobs/big', param: 'op.{op}.ratio' },
    { type: 'list', name: 'lst', rect: [0, 50, 100, 40], header: { text: 'H', font: 'small' }, row: { font: 'tiny' } },
    { type: 'custom', name: 'xy', rect: [50, 0, 40, 40], kind: 'pad', params: ['arp.tempo', 'op.a.ratio'] },
  ] },
  'views/page_arp.json': { size: [150, 100], widgets: [{ type: 'dial', name: 'tempo', rect: [0, 0, 40, 40], image: 'knobs/big', param: 'arp.tempo' }] },
});

await test('reference renaming across a skin', () => {
  const d = skinDocs();
  assert.equal(L.renameRefs(d, 'image', 'knobs/big', 'knobs/large'), 3);
  assert.deepEqual(Object.keys(d['skin.json'].images), ['knobs/large', 'panels/box']);
  assert.equal(d['views/page_arp.json'].widgets[0].image, 'knobs/large');
  assert.equal(L.renameRefs(d, 'image', 'panels/box', 'panels/frame'), 3);
  assert.equal(d['views/main.json'].fill.image, 'panels/frame');
  assert.equal(L.renameRefs(d, 'font', 'small', 'caption'), 3);
  assert.equal(d['views/page_op.json'].widgets[1].header.font, 'caption');
  assert.equal(d['views/page_op.json'].widgets[1].row.font, 'tiny');
  assert.equal(L.renameRefs(d, 'view', 'page_op', 'page_operator'), 2);
  assert.equal(d['views/main.json'].widgets[0].view, 'page_operator');
  assert.equal(d['views/main.json'].widgets[1].action.goto, 'page_operator');
  assert.equal(L.renameRefs(d, 'view', 'main', 'root_view'), 1);
  assert.equal(d['skin.json'].root, 'root_view');
  assert.equal(L.renameRefs(d, 'param', 'arp.tempo', 'arp.rate'), 2);
  assert.deepEqual(d['views/page_op.json'].widgets[2].params, ['arp.rate', 'op.a.ratio']);
  assert.equal(L.renameRefs(d, 'image', 'nothing', 'x'), 0);
});

await test('validation', () => {
  const have = { images: new Set(['knobs/big', 'panels/box']), fonts: new Set(['small', 'tiny']) };
  assert.deepEqual(L.validate(skinDocs(), have), []);
  const d = skinDocs(), ws = d['views/main.json'].widgets;
  ws.push({ type: 'plate', name: 'about_box', rect: [190, 90, 20, 20], fill: { image: 'gone' } });
  ws.push({ type: 'button', name: 'bad_nav', rect: [0, 0, 5, 5], action: { goto: 'page_x', stack: 'nowhere' } });
  ws.push({ type: 'dial', name: 'bad_param', rect: [0, 0, 5, 5], param: 'op.{op}.nothing' });
  ws.push({ type: 'blob', rect: [0, 0, 1] });
  d['params.json'].push({ id: 'arp.tempo', name: 'Again' }, { id: 'f.cut', name: 'Cut', min: 0, max: 10, taper: 'log' });
  const msgs = L.validate(d, have).map(i => `${i.widget ?? '-'}: ${i.msg}`);
  for (const m of ['-: duplicate param id "arp.tempo"', '-: log taper needs min and max > 0 ("f.cut")', 'about_box: missing image "gone"', 'bad_nav: missing view "page_x"',
    'bad_nav: missing stack "nowhere"', 'bad_param: missing param "op.{op}.nothing"', 'about_box: duplicate name "about_box"', 'about_box: outside its view',
    '-: unknown type "blob"', '-: blob without a name', '-: rect is not [x, y, w, h]']) assert.ok(msgs.includes(m), `expected: ${m}\ngot: ${msgs.join('\n')}`);
  assert.equal(msgs.length, 11);
});

await test('stacks, pages and embeds', () => {
  const d = skinDocs();
  assert.equal(L.findEmbed(d, 'main', 'pages').widget.view, 'page_op');
  assert.equal(L.findEmbed(d, 'main', 'nope'), null);
  const tags = L.viewTags(d);
  assert.deepEqual([...tags.page_op], ['pages']);
  assert.deepEqual([...tags.page_arp], ['pages']);
  assert.equal(tags.main, undefined);
  assert.deepEqual(L.viewNames(d), ['main', 'page_arp', 'page_op']);
  assert.ok(L.paramMatcher('op.{op}.ratio')('op.b.ratio'));
  assert.ok(!L.paramMatcher('op.{op}.ratio')('op.b.c.ratio'));
  assert.equal(L.resolveVars('op.{op}.ratio {op:upper} {nope}', { op: 'b' }), 'op.b.ratio B {nope}');
});

await test('JSON key order and layout', () => {
  const w = { fill: { colour: '#ffffffff' }, rect: [1, 2, 3, 4], zeta: 1, name: 'n', layer: 2, type: 'plate', alpha: [1, 2] };
  assert.deepEqual(L.orderedKeys(w), ['type', 'name', 'rect', 'layer', 'fill', 'zeta', 'alpha']);
  const v = { size: [10, 10], widgets: [w] };
  const s = L.stringify(v);
  assert.equal(s, [
    '{',
    '  "size": [10, 10],',
    '  "widgets": [',
    '    {',
    '      "type": "plate",',
    '      "name": "n",',
    '      "rect": [1, 2, 3, 4],',
    '      "layer": 2,',
    '      "fill": { "colour": "#ffffffff" },',
    '      "zeta": 1,',
    '      "alpha": [1, 2]',
    '    }',
    '  ]',
    '}'].join('\n'));
  assert.deepEqual(JSON.parse(s), v);
  const long = { labels: Array.from({ length: 40 }, (_, i) => `label ${i}`) };
  assert.ok(L.stringify(long).includes('\n    "label 0",\n'));
  const d = skinDocs();
  for (const doc of Object.values(d)) assert.deepEqual(JSON.parse(L.stringify(doc)), doc);
  assert.ok(L.stringify(d['params.json']).includes('\n  { "id": "arp.tempo", "name": "Tempo", "min": 20, "max": 300 }\n'));
  assert.equal(L.stringify({ a: undefined, b: [] }), '{\n  "b": []\n}');
});

await test('values, taper and printf', () => {
  assert.equal(L.formatValue('%.2f', 1.234), '1.23');
  assert.equal(L.formatValue('%3.0f', 7), '  7');
  assert.equal(L.formatValue('%5.1f dB', -3.54), ' -3.5 dB');
  assert.equal(L.formatValue('%d%%', 49.6), '50%');
  assert.equal(L.formatValue('%%%.1f', 2), '%2.0');
  assert.equal(L.formatValue('%+05.1f', 2.5), '+02.5');
  assert.equal(L.formatValue('%-5d|', 3), '3    |');
  assert.equal(L.formatValue('%g', 0.0001), '0.0001');
  assert.equal(L.formatValue('%g', 1e-5), '1e-05');
  assert.equal(L.formatValue('%g', 100), '100');
  assert.equal(L.formatValue('%.3g', 0.5), '0.5');
  assert.equal(L.formatValue('%e', 1234.5), '1.234500e+03');
  assert.equal(L.formatValue('no conversion', 1), 'no conversion');
  const log = { id: 'f', min: 20, max: 20000, default: Math.sqrt(20 * 20000), taper: 'log' };
  assert.ok(Math.abs(L.widgetValue({}, log).norm - 0.5) < 1e-9);
  assert.ok(Math.abs(L.fromNorm(0.5, 20, 20000, 'log') - Math.sqrt(20 * 20000)) < 1e-9);
  assert.equal(L.widgetValue({}, { ...log, taper: undefined }).norm, (log.default - 20) / 19980);
  const sw = { id: 's', min: 0, max: 2, default: 1, steps: 2, labels: ['Low', 'Mid', 'High'] };
  assert.equal(L.displayText({}, sw), 'Mid');
  assert.equal(L.widgetValue({}, sw).index, 1);
  assert.equal(L.displayText({}, { id: 'n', min: 0, max: 10, default: 3, steps: 10 }), '3');
  assert.equal(L.displayText({ format: '%.1f Hz' }, { id: 'n', default: 0.25 }), '0.2 Hz');
  assert.equal(L.displayText({ range: [0, 100], value: 42 }, null), '42.00');
  assert.deepEqual(L.widgetValue({ value: 1 }, null).index, 1);
  assert.equal(L.cssColour('#ff000080'), 'rgba(255,0,0,0.5019607843137255)');
  assert.equal(L.cssColour('#00ff00'), 'rgba(0,255,0,1)');
  assert.equal(L.cssColour('red'), null);
});

await test('surfaces', () => {
  const ramps = L.surfaceRamps({ lightness: [0.7, 0.86], chroma: [0.06, 0.14] });
  assert.equal(ramps.weight[255], 256); assert.equal(ramps.weight[100], 0); assert.equal(ramps.chroma[0], 256); assert.equal(ramps.chroma[60], 0);
  assert.equal(L.surfaceRamps({ strength: 0.5 }).weight[255], 128);
  const tex = { width: 2, height: 1, data: new Uint8Array([200, 220, 240, 255, 100, 110, 120, 255]) };
  // white becomes the texture where it lies; a saturated pixel and a dark one stay; alpha is kept
  const px = new Uint8Array([255, 255, 255, 255, 255, 255, 255, 128, 255, 120, 0, 255, 40, 40, 40, 255]);
  L.applySurface(px, 4, 1, 0, 0, tex, ramps);
  assert.deepEqual([...px], [200, 220, 240, 255, 100, 110, 120, 128, 255, 120, 0, 255, 40, 40, 40, 255]);
  // a light grey takes the texture times its lightness (230 of 255), as the runtime rounds it
  const grey = new Uint8Array([230, 230, 230, 255]);
  L.applySurface(grey, 1, 1, 1, 0, tex, ramps);
  assert.deepEqual([...grey], [Math.floor(100 * 230 / 255), Math.floor(110 * 230 / 255), Math.floor(120 * 230 / 255), 255]);
});

await test('reflecting surfaces', () => {
  // the slope of a drawing: lighter to the left reads as tilted right (+x), flat reads 0
  const edge = new Uint8Array(3 * 1 * 4);
  [255, 128, 0].forEach((v, i) => edge.set([v, v, v, 255], i * 4));
  const sl = L.surfaceSlope(edge, 3, 1);
  assert.ok(sl[2] > 60 && sl[3] === 0, [...sl]);
  assert.deepEqual([...L.surfaceSlope(new Uint8Array(4).fill(255), 1, 1)], [0, 0]);
  // the lookup moves by slope * relief / 128 and by the sheet's normals * dents / 128, wrapping the texture
  const ramps = L.surfaceRamps({});
  const tex = { width: 4, height: 1, data: new Uint8Array([10, 10, 10, 255, 20, 20, 20, 255, 30, 30, 30, 255, 40, 40, 40, 255]) };
  const white = () => new Uint8Array([255, 255, 255, 255]);
  assert.equal(L.applySurface(white(), 1, 1, 0, 0, tex, ramps, { slope: new Int8Array([127, 0]), relief: 256 })[0], 30);   // moved 2 right
  const normals = { width: 1, height: 1, data: new Uint8Array([192, 128, 128, 255]) };   // the sheet tilted right by half
  assert.equal(L.applySurface(white(), 1, 1, 1, 0, tex, ramps, { normals, dents: 4 })[0], 40);   // 64 * 4 / 128 = 2 right of x 1
  assert.equal(L.applySurface(white(), 1, 1, 0, 0, tex, ramps, { slope: new Int8Array([-127, 0]), relief: 128 })[0], 20);   // 127 left of 0 wraps to 1
});

await test('view chains', () => {
  const d = {
    'skin.json': { root: 'main' },
    'views/main.json': { widgets: [{ type: 'embed', name: 'side', view: 'nav' }, { type: 'embed', name: 'pages', view: 'page_a' }] },
    'views/nav.json': { widgets: [{ type: 'button', name: 'b', action: { goto: 'page_b', stack: 'pages' } }, { type: 'embed', name: 'more', view: 'deep' }] },
    'views/page_a.json': { widgets: [] }, 'views/page_b.json': { widgets: [] }, 'views/deep.json': { widgets: [{ type: 'embed', name: 'loop', view: 'main' }] },
    'views/lonely.json': { widgets: [] },
  };
  const p = L.viewParents(d);
  assert.deepEqual(p.nav, { view: 'main', via: 'side' });
  assert.deepEqual(p.page_b, { view: 'main', via: 'pages' }); // a goto shows its page in the stack's embed
  assert.deepEqual(L.viewChain(p, 'deep'), ['main', 'nav', 'deep']);
  assert.deepEqual(L.viewChain(p, 'page_b'), ['main', 'page_b']);
  assert.deepEqual(L.viewChain(p, 'main'), ['main']); // the loop back to the root is not a parent
  assert.deepEqual(L.viewChain(p, 'lonely'), ['lonely']);
});

await test('menu item trees', () => {
  const mk = () => [{ label: 'A', value: 0 }, { separator: true }, { label: 'Sub', items: [{ label: 'S1', value: 1 }, { label: 'S2', value: 2 }] }, { label: 'B', value: 3, check: true }];
  const labels = items => items.map(x => (x.separator ? '-' : x.items ? `${x.label}(${labels(x.items)})` : x.label)).join(' ');
  let m = mk();
  assert.equal(L.menuItemAt(m, [2, 1]).label, 'S2');
  assert.equal(L.menuKind(m[1]), 'separator'); assert.equal(L.menuKind(m[2]), 'submenu'); assert.equal(L.menuKind(m[0]), 'item');
  assert.deepEqual(L.menuPaths(m), [[0], [1], [2], [2, 0], [2, 1], [3]]);
  assert.equal(L.menuNextValue(m), 4);
  assert.equal(L.menuNextValue([]), 0);
  // moves within a list, across levels, and the shift of the removal
  assert.deepEqual(L.menuMove(m, [0], [4]), [3]); assert.equal(labels(m), '- Sub(S1 S2) B A');
  m = mk(); assert.deepEqual(L.menuMove(m, [3], [0]), [0]); assert.equal(labels(m), 'B A - Sub(S1 S2)');
  m = mk(); assert.deepEqual(L.menuMove(m, [0], [2, 1]), [1, 1]); assert.equal(labels(m), '- Sub(S1 A S2) B');
  m = mk(); assert.deepEqual(L.menuMove(m, [2, 0], [0]), [0]); assert.equal(labels(m), 'S1 A - Sub(S2) B');
  m = mk(); assert.equal(L.menuMove(m, [2], [2, 1]), null); assert.equal(labels(m), 'A - Sub(S1 S2) B'); // not into itself
  // indent into the previous item (making it a submenu), outdent after the parent; separators take nothing
  m = mk(); assert.deepEqual(L.menuIndent(m, [3]), [2, 2]); assert.equal(labels(m), 'A - Sub(S1 S2 B)');
  assert.deepEqual(L.menuOutdent(m, [2, 0]), [3]); assert.equal(labels(m), 'A - Sub(S2 B) S1');
  assert.equal(L.menuIndent(m, [2]), null); // the previous item is a separator
  assert.equal(L.menuIndent(m, [0]), null);
  assert.equal(L.menuOutdent(m, [1]), null);
  m = mk(); assert.deepEqual(L.menuIndent(m, [1]), [0, 0]); assert.equal(labels(m), 'A(-) Sub(S1 S2) B');
  // insert and remove
  m = mk(); assert.deepEqual(L.menuInsert(m, [2, 2], { label: 'S3' }), [2, 2]); assert.equal(labels(m), 'A - Sub(S1 S2 S3) B');
  assert.deepEqual(L.menuInsert(m, [99], { label: 'Z' }), [4]);
  assert.equal(L.menuRemove(m, [2, 0]).label, 'S1'); assert.equal(labels(m), 'A - Sub(S2 S3) B Z');
  assert.equal(L.menuRemove(m, [9]), undefined);
});

await test('inspector parsers and paths', () => {
  const items = L.parseItems('Sine = 0\n-\nSaw\nSquare = 5\nTri', [{ label: 'x', value: 0, icon: 'wave' }]);
  assert.deepEqual(items, [{ label: 'Sine', value: 0, icon: 'wave' }, { separator: true }, { label: 'Saw', value: 1 }, { label: 'Square', value: 5 }, { label: 'Tri', value: 6 }]);
  assert.equal(L.formatItems(items), 'Sine = 0\n-\nSaw = 1\nSquare = 5\nTri = 6');
  assert.equal(L.parseItems('  \n'), undefined);
  assert.deepEqual(L.parseVars('op = a, timbre=tl'), { op: 'a', timbre: 'tl' });
  assert.equal(L.parseVars(''), undefined);
  assert.equal(L.formatVars({ op: 'a', t: 'b' }), 'op=a, t=b');
  const o = { text: { font: 'f' } };
  L.setPath(o, 'text.align', 'center', 'left');
  assert.deepEqual(o, { text: { font: 'f', align: 'center' } });
  L.setPath(o, 'text.align', 'left', 'left');
  L.setPath(o, 'text.font', undefined);
  assert.deepEqual(o, {});
  L.setPath(o, 'fill.image', undefined);
  assert.deepEqual(o, {});
  L.setPath(o, 'pad', [0, 0, 0, 0], [0, 0, 0, 0]);
  assert.deepEqual(o, {});
  L.setPath(o, 'fill.tile', 0);
  assert.deepEqual(o, { fill: { tile: 0 } });
  assert.equal(L.getPath(o, 'fill.tile'), 0);
  assert.equal(L.getPath(o, 'text.font'), undefined);
});

await test('layout helpers', () => {
  const rs = [[0, 0, 10, 10], [20, 5, 10, 20], [50, 30, 20, 10]];
  assert.deepEqual(L.align(rs, 'left').map(r => r[0]), [0, 0, 0]);
  assert.deepEqual(L.align(rs, 'right').map(r => r[0]), [60, 60, 50]);
  assert.deepEqual(L.align(rs, 'hcentre').map(r => r[0]), [30, 30, 25]);
  assert.deepEqual(L.align(rs, 'bottom').map(r => r[1]), [30, 20, 30]);
  assert.deepEqual(L.align(rs, 'vcentre').map(r => r[1]), [15, 10, 15]);
  assert.deepEqual(L.align(rs, 'width').map(r => r[2]), [10, 10, 10]);
  assert.deepEqual(L.distribute([[0, 0, 10, 10], [50, 0, 10, 10], [12, 0, 10, 10]], 'x').map(r => r[0]), [0, 50, 25]);
  assert.deepEqual(L.snapDelta([10, 15, 20], [0, 22, 100], 3), { d: 2, at: 22 });
  assert.equal(L.snapDelta([10], [20], 3), null);
  assert.equal(L.uniqueName('dial_1', new Set(['dial_1', 'dial_2'])), 'dial_3');
  assert.equal(L.uniqueName('tempo', new Set(['tempo'])), 'tempo_2');
  assert.equal(L.uniqueName('free', new Set()), 'free');
  assert.equal(L.snake('My Knob-Strip (2)'), 'my_knob_strip_2');
  assert.deepEqual(L.drawOrder([{ n: 'a' }, { n: 'b', layer: 0 }, { n: 'c', layer: 7 }, { n: 'd', layer: 3 }]).map(w => w.n), ['b', 'a', 'd', 'c']);
  // piano keys as the runtime lays them out: whites first, blacks 0.7 wide, 0.59 tall, shifted per note
  const keys = L.pianoKeys(60, 12, [0, 0, 70, 40]);
  assert.deepEqual([keys.filter(k => !k.black).length, keys.filter(k => k.black).length], [7, 5]);
  assert.deepEqual(keys[0], { note: 60, black: false, rect: [0, 0, 10, 40] });
  assert.deepEqual(keys.find(k => k.note === 61).rect, [6, 0, 7, 24]); // centre round((1 - 0.057) * 10) = 9
  assert.deepEqual(keys.find(k => k.note === 63).rect, [18, 0, 7, 24]); // centre round((2 + 0.128) * 10) = 21
  assert.equal(keys.slice(7).every(k => k.black), true);
  assert.equal(L.pianoVelocity([0, 10, 10, 40], 10), 1);
  assert.equal(L.pianoVelocity([0, 10, 10, 40], 49), 127);
  assert.equal(L.padHandle([0, 0, 200, 30]), 7);
  assert.equal(L.padHandle([0, 0, 200, 200]), 14);
});

await test('parallel pool: limit, order, progress, errors', async () => {
  let running = 0, most = 0;
  const seen = [];
  const out = await L.pool([5, 1, 4, 2, 3, 0, 6, 2, 1, 3], 3, async (v, i) => {
    running++; most = Math.max(most, running);
    await new Promise(r => setTimeout(r, v));
    running--;
    return v * 10 + i;
  }, (d, n) => seen.push(`${d}/${n}`));
  assert.deepEqual(out, [50, 11, 42, 23, 34, 5, 66, 27, 18, 39]);
  assert.equal(most, 3);
  assert.deepEqual(seen.at(-1), '10/10');
  assert.equal(seen.length, 10);
  assert.deepEqual(await L.pool([], 8, async () => 1), []);
  let started = 0;
  await assert.rejects(L.pool([1, 2, 3, 4, 5, 6], 2, async v => { started++; if (v === 2) throw new Error('bad 2'); await new Promise(r => setTimeout(r, 2)); }), /bad 2/);
  assert.ok(started < 6, 'stops starting new work after an error');
});

await test('showIf / enableIf conditions', () => {
  const ps = new Map([['fx.on', { id: 'fx.on', min: 0, max: 1, steps: 1 }], ['op.b.wave', { id: 'op.b.wave', min: 0, max: 2, steps: 2, labels: ['Sine', 'Saw', 'Square'] }],
    ['n', { id: 'n', min: 1, max: 32, steps: 31 }]]);
  const vals = { 'fx.on': 0, 'op.b.wave': 2, n: 8 }, vars = { op: 'b' };
  const env = { param: id => (ps.has(id) ? { v: vals[id], p: ps.get(id) } : null), variable: k => vars[k], sub: s => L.resolveVars(s, vars) };
  const ev = c => L.evalCondition(c, env);
  assert.equal(ev(undefined), true);
  assert.equal(ev('fx.on'), false); // at its minimum
  vals['fx.on'] = 1;
  assert.equal(ev('fx.on'), true);
  assert.equal(ev({ param: 'op.{op}.wave', equals: 2 }), true); // {var} in the id
  assert.equal(ev({ param: 'op.{op}.wave', equals: 'Square' }), true); // a label
  assert.equal(ev({ param: 'op.{op}.wave', notEquals: 2 }), false);
  assert.equal(ev({ var: 'op', equals: 'b' }), true);
  assert.equal(ev({ var: 'op', notEquals: 'b' }), false);
  assert.equal(ev({ var: 'nope', equals: 'b' }), false);
  assert.equal(ev('no.such.param'), true); // a typo never hides a widget
  assert.equal(ev({ param: 'n', atLeast: 8 }), true);
  assert.equal(ev({ param: 'n', atLeast: 9 }), false);
  assert.equal(ev({ param: 'n', atMost: 8 }), true);
  // the inspector form round-trips every kind
  for (const c of ['fx.on', { param: 'x', equals: 2 }, { param: 'x', notEquals: 0 }, { param: 'x', atLeast: 3 }, { param: 'x', atMost: 4 }, { var: 'op', equals: 'pitch' }, { var: 'op', notEquals: 'a' }])
    assert.deepEqual(L.makeCond(L.condForm(c)), c);
  assert.deepEqual(L.condForm(undefined), { kind: '', name: '', value: '' });
  assert.equal(L.makeCond({ kind: 'param:eq', name: '', value: '1' }), undefined);
  assert.deepEqual(L.makeCond({ kind: 'param:eq', name: 'x', value: 'Saw' }), { param: 'x', equals: 'Saw' });
  assert.deepEqual(L.makeCond({ kind: 'var:eq', name: 'op', value: '2' }), { var: 'op', equals: '2' }); // vars compare as text
});

await test('flow layout and scrollbar geometry', () => {
  // the column starts at the y of the first widget in the file
  const rs = [[5, 50, 100, 10], [0, 0, 100, 20], null, [3, 99, 50, 15]];
  assert.deepEqual(L.flowLayout(rs, [true, true, true, true], 2), { rects: [[5, 50, 100, 10], [0, 62, 100, 20], null, [3, 84, 50, 15]], height: 99 });
  // hidden ones take no space (and sit where they would go)
  assert.deepEqual(L.flowLayout(rs, [true, false, true, true], 2), { rects: [[5, 50, 100, 10], [0, 62, 100, 20], null, [3, 62, 50, 15]], height: 77 });
  assert.deepEqual(L.flowLayout(rs, [false, true, true, true]).rects[1], [0, 50, 100, 20]);
  assert.equal(L.flowLayout(rs, [false, false, false, false]).height, 50);
  // animated heights: a widget takes its current height in the column
  assert.deepEqual(L.flowLayout(rs, [true, true, true, true], 0, [10, 5, 0, 15]), { rects: [[5, 50, 100, 10], [0, 60, 100, 20], null, [3, 65, 50, 15]], height: 80 });
  assert.equal(L.scrollBar([0, 0, 50, 100], 100, 0), null); // fits: no bar
  assert.deepEqual(L.scrollBar([0, 0, 50, 100], 50, 0, 6, true), { pos: 0, max: 0, track: [44, 0, 6, 100], thumb: [44, 0, 6, 100] }); // always
  const b = L.scrollBar([10, 20, 50, 100], 400, 150, 6);
  assert.deepEqual(b, { pos: 150, max: 300, track: [54, 20, 6, 100], thumb: [54, 20 + Math.round(150 * 75 / 300), 6, 25] });
  assert.equal(L.scrollBar([10, 20, 50, 100], 400, 999).pos, 300); // clamped
  assert.equal(L.scrollBar([10, 20, 50, 100], 400, 999).thumb[1], 95); // thumb at the bottom
  assert.equal(L.scrollBar([0, 0, 50, 100], 100000, 0).thumb[3], 8); // minimum thumb
  assert.equal(L.scrollFromThumb(b, 20), 0);
  assert.equal(L.scrollFromThumb(b, 95), 300);
  assert.equal(L.scrollFromThumb(b, 57), 148);
  assert.equal(L.scrollFromThumb(b, 500), 300);
});

await test('menu and tooltip layout', () => {
  const items = [{ label: 'Sine', value: 0 }, { label: 'Triangle', value: 1 }, { separator: true }, { label: 'Noise', value: 3 }];
  const measure = s => s.length * 5, style = { pad: [3, 1, 6, 1], border: '#000000ff' }, mt = { measure, lineH: 9, checkW: 8, arrowW: 6 };
  const m = L.menuLayout(items, style, mt, [20, 30, 30, 12], [200, 100]);
  // rows 9 + 1 + 1 = 11 tall, separators 3 (separatorHeight), 1 px border; width: pad + check + longest label + pad + border
  assert.deepEqual(m.rect, [20, 42, 1 + 3 + 8 + 40 + 6 + 1, 1 + 11 * 3 + 3 + 1]);
  assert.deepEqual(m.rows.map(r => r.rect), [[21, 43, 57, 11], [21, 54, 57, 11], [21, 65, 57, 3], [21, 68, 57, 11]]);
  assert.deepEqual(m.pad, [11, 1, 6, 1]);
  // too low for below: above the widget; too far right: moved left; at least the widget's width
  const up = L.menuLayout(items, { ...style, rowHeight: 16, separatorHeight: 5 }, { ...mt, checkW: 0 }, [180, 80, 100, 12], [200, 100]);
  assert.deepEqual(up.rect, [100, 80 - 55, 100, 55]); // 3 rows of 16, a separator of 5, the border
  // "over": on the widget's top-left, sliding to stay in the window
  assert.deepEqual(L.menuLayout(items, style, mt, [20, 30, 30, 12], [200, 100], 'over').rect.slice(0, 2), [20, 30]);
  assert.deepEqual(L.menuLayout(items, style, mt, [190, 95, 5, 5], [200, 100], 'over').rect.slice(0, 2), [141, 62]);
  // a submenu sits right of its row, or left when there is no room; an arrow column when an item has a submenu
  const sub = L.menuLayout(items, style, mt, [21, 54, 57, 11], [200, 100], 'side');
  assert.deepEqual(sub.rect.slice(0, 2), [78, 53]);
  assert.deepEqual(L.menuLayout(items, style, mt, [150, 54, 40, 11], [200, 100], 'side').rect.slice(0, 2), [150 - 59, 53]);
  assert.equal(L.menuLayout([{ label: 'x', items: [] }], {}, mt, [0, 0, 0, 0], [200, 100]).pad[2], 4 + 6);
  // native menus start a column at a columnBreak item
  const cols = L.menuLayout([{ label: 'a' }, { label: 'bb', columnBreak: true }, { label: 'c' }], { native: true, pad: [0, 0, 0, 0] }, { measure, lineH: 10 }, [0, 0, 0, 0], [200, 100]);
  assert.deepEqual(cols.rows.map(r => r.rect), [[0, 0, 5, 10], [5, 0, 10, 10], [5, 10, 10, 10]]);
  assert.deepEqual(cols.rect, [0, 0, 15, 20]);
  // named styles override the menu style; "native" (or no menu) is a native menu
  const sk = { menu: { fill: '#111111ff', place: 'over', styles: { plain: { place: 'below', hoverBand: null } } } };
  assert.deepEqual(L.menuStyleOf(sk, 'plain'), { fill: '#111111ff', place: 'below', hoverBand: null });
  assert.deepEqual(L.menuStyleOf(sk), { fill: '#111111ff', place: 'over' });
  assert.equal(L.menuStyleOf(sk, 'native'), null);
  assert.equal(L.menuStyleOf({}, 'plain'), null);
  assert.equal(L.menuRowAt(m, 25, 56).i, 1);
  assert.equal(L.menuRowAt(m, 25, 66).item.separator, true);
  assert.equal(L.menuRowAt(m, 5, 56), null);
  assert.equal(L.menuStep(items, 1, 1), 3); // skips the separator
  assert.equal(L.menuStep(items, 3, 1), 0); // wraps
  assert.equal(L.menuStep(items, -1, -1), 3);
  assert.equal(L.menuStep([{ separator: true }], -1, 1), -1);
  const t = L.tipLayout('Two\\nlines', { pad: [2, 1, 2, 1], border: '#000000ff' }, measure, 8, [10, 10], [200, 100]);
  assert.deepEqual(t.rect, [14, 30, 25 + 4 + 2, 16 + 2 + 2]);
  assert.deepEqual(t.pad, [3, 2, 3, 2]);
  assert.deepEqual(L.tipLayout('x', {}, measure, 8, [195, 95], [200, 100]).rect, [187, 95 - 12 - 4, 13, 12]); // kept inside
});

await test('values: display, parsing, status text', () => {
  const p = { id: 'a.t', name: 'Tempo', min: 10, max: 500, default: 120, unit: 'bpm', format: '%.1f' };
  assert.equal(L.statusText(p, 140), 'Tempo: 140.0 bpm');
  assert.equal(L.parseValueText(' 140 bpm', p), 140);
  assert.equal(L.parseValueText('-3.5e1x', p), -35);
  assert.equal(L.parseValueText('fast', p), null);
  const w = { id: 'w', min: 0, max: 2, steps: 2, labels: ['Sine', 'Saw', 'Square'] };
  assert.equal(L.parseValueText('SAW', w), 1);
  assert.equal(L.parseValueText('2', w), 2);
  assert.equal(L.displayText({}, w, 2), 'Square');
  assert.equal(L.displayText({ format: '%.0f' }, w, 2), '2'); // a widget format replaces the labels, as in the runtime
});

// A small skin for play mode: a page stack with navigation, actions, a dial with MIDI, a dropdown,
// a scrolling flow list, a piano and a status line.
const playDocs = () => ({
  'skin.json': { format: 1, name: 'P', root: 'main', vars: { op: 'a', timbre: 'tl' }, tooltip: { delay: 500 } },
  'params.json': [
    { id: 'x.on', name: 'On', min: 0, max: 1, steps: 1 }, { id: 'x.count', name: 'Count', min: 0, max: 10, steps: 10 },
    { id: 'x.mode', name: 'Mode', min: 0, max: 2, steps: 2, labels: ['A', 'B', 'C'] },
    { id: 'op.a.amt', name: 'A Amount', min: 0, max: 100, format: '%.0f' }, { id: 'op.b.amt', name: 'B Amount', min: 0, max: 100, default: 50 },
    { id: 'x.freq', name: 'Freq', min: 20, max: 20000, default: 20, taper: 'log', host: false },
  ],
  'views/main.json': { size: [200, 150], widgets: [
    { type: 'embed', name: 'pages', rect: [0, 0, 100, 100], view: 'page_a' },
    { type: 'button', name: 'nav_a', rect: [100, 0, 20, 10], press: 'up', action: { goto: 'page_a', stack: 'pages', vars: { op: 'a' } } },
    { type: 'button', name: 'nav_b', rect: [120, 0, 20, 10], press: 'up', action: { goto: 'page_b', stack: 'pages', vars: { op: 'b' } } },
    { type: 'button', name: 'set_br', rect: [100, 10, 20, 10], action: { set: { timbre: 'br' } } },
    { type: 'button', name: 'tog', rect: [100, 20, 20, 10], action: { toggle: 'about' } },
    { type: 'plate', name: 'about', rect: [140, 10, 30, 30], hidden: true },
    { type: 'button', name: 'mom', rect: [100, 30, 20, 10], press: 'momentary', param: 'x.on', tip: 'Hold me' },
    { type: 'button', name: 'rep', rect: [100, 40, 20, 10], press: 'repeat', param: 'x.count', action: { step: 1 } },
    { type: 'button', name: 'cyc', rect: [100, 50, 20, 10], toggle: true, param: 'x.mode' },
    { type: 'button', name: 'radio', rect: [150, 0, 20, 10], param: 'x.mode', action: { value: 2 } },
    { type: 'dropdown', name: 'dd', rect: [100, 60, 40, 10], param: 'x.mode', items: [{ label: 'A', value: 0 }, { separator: true }, { label: 'B', value: 1 }, { label: 'C', value: 2 }] },
    { type: 'dial', name: 'wheel', rect: [180, 40, 10, 60], midi: 'bend', spring: 0.5, value: 0.5 },
    { type: 'dial', name: 'freq', rect: [150, 40, 20, 20], param: 'x.freq', drag: 'rotary' },
    { type: 'dial', name: 'fader', rect: [150, 70, 10, 40], drag: 'absolute', margin: 5, range: [0, 10], steps: 10 },
    { type: 'textbox', name: 'status', rect: [0, 140, 100, 10], status: true, text: { text: 'idle' } },
    { type: 'embed', name: 'list', rect: [0, 100, 100, 30], view: 'col', scroll: { track: 't', thumb: 'h', width: 6 } },
    { type: 'custom', name: 'piano', rect: [100, 110, 70, 40], kind: 'piano', first: 60, count: 12 },
    { type: 'textbox', name: 'note', rect: [170, 110, 30, 20], key: 'notes.{op}', editable: true, text: { text: 'none' } },
    { type: 'plate', name: 'cover', rect: [0, 130, 10, 10], showIf: { var: 'timbre', equals: 'br' } },
  ] },
  'views/page_a.json': { size: [100, 100], widgets: [
    { type: 'dial', name: 'amt', rect: [0, 0, 20, 20], param: 'op.{op}.amt' },
    { type: 'number', name: 'num', rect: [20, 0, 30, 10], param: 'x.count', editable: true },
    { type: 'number', name: 'plain', rect: [20, 10, 30, 10], param: 'op.{op}.amt', sensitivity: 0.5 },
    { type: 'plate', name: 'only_a', rect: [0, 30, 10, 10], showIf: { var: 'op', equals: 'a' } },
    { type: 'button', name: 'gated', rect: [0, 40, 10, 10], param: 'x.on', toggle: true, enableIf: { param: 'x.mode', atLeast: 1 } },
  ] },
  'views/page_b.json': { size: [100, 100], widgets: [{ type: 'dial', name: 'amt', rect: [0, 0, 20, 20], param: 'op.{op}.amt' }] },
  'views/col.json': { size: [100, 0], flow: 'column', gap: 2, widgets: [0, 1, 2, 3, 4].map(i => ({ type: 'plate', name: `row_${i}`, rect: [0, 0, 100, 10], ...(i === 1 ? { showIf: 'x.on' } : {}) })) },
});
const find = (pc, name) => [...L.treeItems(pc.root)].find(it => it.key.endsWith('/' + name));
const val = (pc, name) => L.getPlain(pc.st, find(pc, name));
const click = (pc, x, y, o = {}) => { L.playDown(pc, x, y, o); L.playUp(pc, x, y, o); };

await test('play tree: embeds, vars, conditions, flow, scrolling', () => {
  const d = playDocs(), st = L.playState(d), pc = L.playCtx(d, st, 'main');
  assert.deepEqual([pc.root.w, pc.root.h], [200, 150]);
  assert.equal(find(pc, 'amt').pid, 'op.a.amt');
  assert.equal(find(pc, 'amt').key, 'main/pages>page_a/amt');
  assert.equal(find(pc, 'only_a').show, true);
  assert.equal(find(pc, 'cover').show, false);
  assert.equal(find(pc, 'gated').enabled, false); // x.mode is 0, needs at least 1
  // the list: 4 rows of 10 with gaps of 2 (row_1 is off) = 46 px in a 30 px embed
  const list = find(pc, 'list');
  assert.equal(list.child.h, 46);
  assert.deepEqual(find(pc, 'row_2').rect, [0, 112, 100, 10]);
  assert.deepEqual(list.bar.track, [94, 100, 6, 30]);
  st.values['x.on'] = 1;
  pc.rebuild();
  assert.equal(find(pc, 'list').child.h, 58);
  assert.deepEqual(find(pc, 'row_2').rect, [0, 124, 100, 10]);
  st.scroll[list.key] = 5;
  pc.rebuild();
  assert.deepEqual(find(pc, 'row_0').rect, [0, 95, 100, 10]);
  st.scroll[list.key] = 500; // stale: clamped at build time
  pc.rebuild();
  assert.equal(find(pc, 'list').bar.pos, 28);
  assert.equal(find(pc, 'row_4').rect[1] + 10, 130);
  // the editor lays hidden widgets out too while it shows them
  st.values['x.on'] = 0;
  assert.equal(L.playCtx(d, L.playState(d), 'col', { showAll: true }).root.h, 58);
  assert.equal(L.playCtx(d, L.playState(d), 'col').root.h, 46);
  // an embed never shows a view above it
  d['views/page_a.json'].widgets.push({ type: 'embed', name: 'loop', rect: [0, 0, 10, 10], view: 'main' });
  assert.equal(find(L.playCtx(d, L.playState(d), 'main'), 'loop').child, undefined);
});

await test('play: buttons, press modes and actions', () => {
  const d = playDocs(), st = L.playState(d), pc = L.playCtx(d, st, 'main');
  const tile = name => L.buttonValue(pc, find(pc, name));
  assert.deepEqual([tile('nav_a'), tile('nav_b')], [0, 0]); // the files show page_a without vars
  click(pc, 125, 5); // nav_b ("up": acts on release inside)
  assert.deepEqual(st.embeds['main/pages'], { view: 'page_b', vars: { op: 'b' } });
  assert.equal(find(pc, 'amt').pid, 'op.b.amt');
  assert.deepEqual([tile('nav_a'), tile('nav_b')], [0, 1]);
  L.playDown(pc, 105, 5); L.playUp(pc, 150, 5); // released outside: no goto
  assert.equal(st.embeds['main/pages'].view, 'page_b');
  click(pc, 105, 5);
  assert.deepEqual([tile('nav_a'), tile('nav_b')], [1, 0]);
  // set lights up by itself, and conditions on vars follow
  assert.equal(tile('set_br'), 0);
  click(pc, 105, 15);
  assert.equal(st.vars.timbre, 'br');
  assert.equal(tile('set_br'), 1);
  assert.equal(find(pc, 'cover').show, true);
  // toggle flips the widget's hidden
  click(pc, 105, 25);
  assert.equal(find(pc, 'about').hidden, false);
  click(pc, 105, 25);
  assert.equal(find(pc, 'about').hidden, true);
  // momentary: 1 while held (pressed state for the tile), 0 after
  L.playDown(pc, 105, 35);
  assert.equal(val(pc, 'mom'), 1);
  assert.equal(st.press.key, 'main/mom');
  L.playUp(pc, 105, 35);
  assert.equal(val(pc, 'mom'), 0);
  assert.equal(st.press, null);
  // repeat + step: once on press, then every 100 ms after 400 ms, clamped to the range
  pc.now = 1000;
  L.playDown(pc, 105, 45);
  assert.equal(val(pc, 'rep'), 1);
  pc.now = 1399; assert.equal(L.playTick(pc), false);
  pc.now = 1400; assert.equal(L.playTick(pc), true);
  pc.now = 1450; assert.equal(L.playTick(pc), false);
  pc.now = 1500; L.playTick(pc);
  assert.equal(val(pc, 'rep'), 3);
  L.playUp(pc, 105, 45);
  assert.equal(val(pc, 'rep'), 3); // a nudge does not act again on release
  assert.equal(tile('rep'), 0); // a step button's tile does not follow the param
  for (let i = 0; i < 12; i++) click(pc, 105, 45);
  assert.equal(val(pc, 'rep'), 10);
  // toggle on a three-value param steps and wraps
  click(pc, 105, 55); assert.equal(val(pc, 'cyc'), 1);
  click(pc, 105, 55); assert.equal(val(pc, 'cyc'), 2);
  click(pc, 105, 55); assert.equal(val(pc, 'cyc'), 0);
  // a value action is a radio button: it sets its param and reads on only while the param holds that value
  assert.equal(L.buttonValue(pc, find(pc, 'radio')), 0);
  click(pc, 155, 5); assert.equal(val(pc, 'cyc'), 2); assert.equal(L.buttonValue(pc, find(pc, 'radio')), 1);
  click(pc, 105, 55); assert.equal(L.buttonValue(pc, find(pc, 'radio')), 0);
  // hover state for hover tiles
  L.playMove(pc, 106, 56);
  assert.equal(st.hover, 'main/cyc');
  // enableIf: disabled widgets ignore input until the condition holds
  click(pc, 125, 5); click(pc, 105, 5); // back to page_a
  click(pc, 5, 45);
  assert.equal(val(pc, 'gated'), 0);
  click(pc, 105, 55); // x.mode = 1
  click(pc, 5, 45);
  assert.equal(val(pc, 'gated'), 1);
  // a goto starts the page fresh; the old page's state below that embed is dropped
  st.hidden['main/pages>page_a/only_a'] = true;
  click(pc, 125, 5); click(pc, 105, 5);
  assert.equal(st.hidden['main/pages>page_a/only_a'], undefined);
});

await test('play: dials, numbers, wheel, MIDI and spring', () => {
  const d = playDocs(), st = L.playState(d), pc = L.playCtx(d, st, 'main');
  // relative vertical drag: a full range per 200 px at sensitivity 1, Shift for fine
  L.playDown(pc, 10, 10); L.playMove(pc, 10, -40); L.playUp(pc, 10, -40);
  assert.ok(Math.abs(val(pc, 'amt') - 25) < 1e-9);
  L.playDown(pc, 10, 10); L.playMove(pc, 10, -40, { shift: true }); L.playUp(pc, 10, -40);
  assert.ok(Math.abs(val(pc, 'amt') - 27.5) < 1e-9);
  // overshoot past an end is kept: the value moves again only once the pointer is back in range
  L.playDown(pc, 10, 10); L.playMove(pc, 10, -300); L.playMove(pc, 10, -290);
  assert.equal(val(pc, 'amt'), 100);
  L.playMove(pc, 10, -125);
  assert.ok(Math.abs(val(pc, 'amt') - 95) < 1e-9);
  L.playUp(pc, 10, -125);
  // double-click resets to the default
  L.playDown(pc, 10, 10, { dbl: true });
  assert.equal(val(pc, 'amt'), 0);
  assert.equal(st.press, null);
  // wheel: continuous dials move 0.05 per notch, stepped ones one step per whole notch
  assert.equal(L.playWheel(pc, 10, 10, 2), true);
  assert.ok(Math.abs(val(pc, 'amt') - 10) < 1e-9);
  assert.equal(L.playWheel(pc, 155, 80, 0.5), true);
  assert.equal(val(pc, 'fader'), 0);
  L.playWheel(pc, 155, 80, 0.6);
  assert.equal(val(pc, 'fader'), 1);
  // absolute drag with margin, on a stepped local value
  L.playDown(pc, 155, 75); // 5 px margin: the top of the travel
  assert.equal(val(pc, 'fader'), 10);
  L.playMove(pc, 155, 70 + 40 - 1 - 5);
  assert.equal(val(pc, 'fader'), 0);
  L.playUp(pc, 155, 104);
  // rotary: 270 degrees around the centre, here on a log param
  L.playDown(pc, 160, 40); L.playUp(pc, 160, 40); // straight up: the middle
  assert.ok(Math.abs(val(pc, 'freq') - Math.sqrt(20 * 20000)) < 1e-6);
  // number: vertical drag in value units per pixel (sensitivity 0.5 -> 0.5 per px)
  L.playDown(pc, 30, 15); L.playMove(pc, 30, 5); L.playUp(pc, 30, 5);
  assert.ok(Math.abs(val(pc, 'plain') - 15) < 1e-9);
  // MIDI: bend 0..16383, and the spring returns the wheel on release
  st.events.length = 0;
  L.playDown(pc, 185, 70); L.playMove(pc, 185, 50);
  assert.deepEqual(st.events.map(e => e.midi), [`pitch bend ${Math.round(0.6 * 16383)}`]);
  L.playUp(pc, 185, 50);
  assert.equal(val(pc, 'wheel'), 0.5);
  assert.equal(st.events.at(-1).midi, 'pitch bend 8192');
  // status: "Name: value" of the param under the pointer (the number above set op.a.amt), then its own text 1.5 s after leaving
  pc.now = 5000;
  L.playMove(pc, 10, 10);
  assert.equal(find(pc, 'status').text, 'A Amount: 15');
  pc.now = 6000; L.playMove(pc, 60, 60);
  assert.equal(find(pc, 'status').text, 'A Amount: 15');
  pc.now = 7499; pc.rebuild();
  assert.equal(find(pc, 'status').text, 'A Amount: 15');
  pc.now = 7500; pc.rebuild();
  assert.equal(find(pc, 'status').text, 'idle');
});

await test('play: menus, text entry, tooltips, piano, scrolling', () => {
  const d = playDocs(), st = L.playState(d), pc = L.playCtx(d, st, 'main', { measure: s => s.length * 5, lineH: 8 });
  click(pc, 110, 65); // opens on press; the release over the widget chooses nothing
  assert.equal(st.menu.levels[0].hover, 0); // the item for the current value
  const m = L.menuLayouts(pc)[0];
  assert.deepEqual(m.rect.slice(0, 2), [100, 70]); // below the widget
  L.playKey(pc, 'ArrowDown'); // skips the separator
  assert.equal(st.menu.levels[0].hover, 2);
  L.playKey(pc, 'Enter');
  assert.equal(st.menu, null);
  assert.equal(val(pc, 'dd'), 1);
  click(pc, 110, 65);
  click(pc, m.rows[3].rect[0] + 2, m.rows[3].rect[1] + 2); // released on a row: chosen
  assert.equal(val(pc, 'dd'), 2);
  click(pc, 110, 65);
  click(pc, m.rows[1].rect[0] + 2, m.rows[1].rect[1] + 1); // a separator keeps it open
  assert.notEqual(st.menu, null);
  L.playDown(pc, 5, 5); // elsewhere closes it, and does nothing else
  assert.equal(st.menu, null);
  L.playDown(pc, 110, 65); L.playKey(pc, 'Escape');
  assert.equal(st.menu, null);
  // editable number and textbox: a double-click asks for an inline editor
  st.events.length = 0;
  L.playDown(pc, 30, 5, { dbl: true });
  assert.deepEqual(st.events, [{ edit: 'main/pages>page_a/num', text: '0', rect: [20, 0, 30, 10] }]);
  assert.equal(L.playCommit(pc, 'main/pages>page_a/num', '7 steps'), true);
  assert.equal(val(pc, 'num'), 7);
  assert.equal(L.playCommit(pc, 'main/pages>page_a/num', 'many'), false);
  L.playDown(pc, 180, 115, { dbl: true });
  assert.deepEqual(st.events.at(-1), { edit: 'main/note', text: 'none', rect: [170, 110, 30, 20], multiline: false });
  L.playCommit(pc, 'main/note', 'Hello');
  assert.deepEqual(st.text, { 'notes.a': 'Hello' });
  assert.equal(find(pc, 'note').text, 'Hello');
  // tooltips show once the pointer rests for the delay, and go on press
  pc.now = 100; L.playMove(pc, 105, 35);
  assert.equal(L.tipVisible(pc), false);
  pc.now = 300; L.playMove(pc, 106, 36); // moved: the wait starts again
  pc.now = 700; assert.equal(L.tipVisible(pc), false);
  pc.now = 800; assert.equal(L.tipVisible(pc), true);
  assert.equal(st.tip.text, 'Hold me');
  L.playDown(pc, 106, 36);
  assert.equal(st.tip, null);
  L.playUp(pc, 106, 36);
  // piano: note on with velocity from the height, note off on release, sliding changes notes
  st.events.length = 0;
  L.playDown(pc, 101, 110); L.playMove(pc, 111, 149); L.playUp(pc, 111, 149);
  assert.deepEqual(st.events.map(e => e.midi), ['note on 60 velocity 1', 'note off 60', 'note on 62 velocity 127', 'note off 62']);
  // scrolling: the wheel, then the thumb
  const key = find(pc, 'list').key;
  assert.equal(L.playWheel(pc, 50, 110, -1), true);
  assert.equal(st.scroll[key], 16);
  L.playDown(pc, 97, 129); L.playMove(pc, 97, 140); L.playUp(pc, 97, 140);
  assert.equal(st.scroll[key], 16); // at the end already: 46 - 30
  L.playDown(pc, 97, 101); // on the track above the thumb: jumps there
  assert.equal(st.scroll[key], 0);
  L.playUp(pc, 97, 101);
  assert.equal(L.playWheel(pc, 150, 5, 1), false); // nothing to scroll or turn here
});

await test('play: menu styles, submenus, check items, context menus, keys', () => {
  const d = playDocs();
  d['skin.json'].menu = { pad: [2, 1, 2, 1], place: 'over', releaseGuard: 300, styles: { plain: { place: 'below', releaseGuard: 0 }, quiet: { keys: 'escape' } } };
  d['views/main.json'].widgets.push(
    { type: 'dropdown', name: 'dd2', rect: [100, 80, 40, 10], param: 'x.mode', captionFromItem: true, items: [{ label: 'A', value: 0 },
      { label: 'More', items: [{ label: 'Bee', short: 'B', value: 1 }, { label: 'C', value: 2 }] }, { label: 'Page B', check: true, action: { goto: 'page_b', stack: 'pages' } }] },
    { type: 'plate', name: 'ctxw', rect: [100, 90, 40, 10], context: [{ label: 'Bright', check: true, action: { set: { timbre: 'br' } } }, { separator: true }, { label: 'Two', value: 2 }] },
    { type: 'dropdown', name: 'dd3', rect: [0, 140, 40, 10], param: 'x.mode', menuStyle: 'quiet', items: [{ label: 'A', value: 0 }, { label: 'B', value: 1 }] },
    { type: 'dropdown', name: 'dd4', rect: [140, 140, 40, 10], param: 'x.mode', menuStyle: 'native', items: [{ label: 'A', value: 0 }] });
  const st = L.playState(d), pc = L.playCtx(d, st, 'main', { measure: s => s.length * 5, lineH: 8 }), dd2 = () => find(pc, 'dd2');
  // "over" with a release guard: the menu covers the widget, the opening click's release is ignored
  pc.now = 1000;
  click(pc, 105, 85);
  assert.deepEqual(L.menuLayouts(pc)[0].rect.slice(0, 2), [100, 80]);
  assert.notEqual(st.menu, null);
  assert.equal(L.menuChecked(pc, dd2().w.items[0]), true);
  assert.equal(L.menuChecked(pc, dd2().w.items[2]), false);
  // hovering a submenu item opens it to the right; a release there, after the guard, chooses
  let lay = L.menuLayouts(pc)[0];
  L.playMove(pc, lay.rows[1].rect[0] + 2, lay.rows[1].rect[1] + 2);
  assert.equal(st.menu.levels.length, 2);
  const subLay = L.menuLayouts(pc)[1];
  assert.equal(subLay.rect[0], lay.rect[0] + lay.rect[2]);
  L.playMove(pc, subLay.rows[0].rect[0] + 2, subLay.rows[0].rect[1] + 2);
  pc.now = 1400;
  L.playUp(pc, subLay.rows[0].rect[0] + 2, subLay.rows[0].rect[1] + 2);
  assert.equal(st.menu, null);
  assert.equal(val(pc, 'dd2'), 1);
  assert.equal(L.itemForValue(dd2().w.items, 1).short, 'B'); // captionFromItem shows the short text
  // keyboard: Right opens the submenu, Enter chooses in it
  pc.now = 2000;
  L.playDown(pc, 105, 85);
  assert.equal(st.menu.levels[0].hover, 1); // the submenu holding the current value
  L.playKey(pc, 'ArrowRight');
  assert.deepEqual([st.menu.levels.length, st.menu.levels[1].hover], [2, 0]);
  L.playKey(pc, 'ArrowDown'); L.playKey(pc, 'Enter');
  assert.equal(val(pc, 'dd2'), 2);
  L.playUp(pc, 105, 85);
  // an item with an action runs it instead of setting the value; a check item shows when it is on
  pc.now = 3000;
  L.playDown(pc, 105, 85); L.playKey(pc, 'ArrowDown'); L.playKey(pc, 'Enter'); L.playUp(pc, 105, 85);
  assert.equal(st.embeds['main/pages'].view, 'page_b');
  assert.equal(val(pc, 'dd2'), 2);
  L.playDown(pc, 105, 85);
  assert.equal(L.menuChecked(pc, dd2().w.items[2]), true);
  L.playKey(pc, 'Escape');
  // context menu: right-click on release, at the pointer, in the "plain" style (below, no guard)
  L.playDown(pc, 110, 95, { right: true });
  assert.equal(st.menu, null);
  L.playUp(pc, 110, 95, { right: true });
  assert.equal(st.menu.context, true);
  lay = L.menuLayouts(pc)[0];
  assert.deepEqual(lay.rect.slice(0, 2), [110, 95]);
  assert.equal(st.menu.levels[0].hover, -1);
  click(pc, lay.rows[0].rect[0] + 2, lay.rows[0].rect[1] + 2);
  assert.equal(st.vars.timbre, 'br');
  L.playDown(pc, 110, 95, { right: true }); L.playUp(pc, 110, 95, { right: true });
  assert.equal(L.menuChecked(pc, L.menuLayouts(pc)[0].rows[0].item), true);
  click(pc, lay.rows[2].rect[0] + 2, lay.rows[2].rect[1] + 2); // a value item sets the widget's value
  assert.equal(L.getPlain(st, find(pc, 'ctxw')), 2);
  L.playDown(pc, 5, 5, { right: true }); L.playUp(pc, 5, 5, { right: true }); // nothing to open here
  assert.equal(st.menu, null);
  // keys "escape": only Escape works
  L.playDown(pc, 5, 145);
  assert.equal(L.playKey(pc, 'ArrowDown'), false);
  assert.equal(L.playKey(pc, 'Escape'), true);
  assert.equal(st.menu, null);
  // "native": the native stand-in style
  L.playDown(pc, 145, 145);
  assert.deepEqual(L.menuStyleNow(pc), { native: true });
});

await test('play: lists, MIDI learn and MIDI in', () => {
  const d = playDocs();
  d['skin.json'].learn = { param: 'gui.learn', outline: '#ff0000ff' };
  d['params.json'].push({ id: 'gui.learn', name: 'Learn', min: 0, max: 1, steps: 1, host: false });
  d['views/page_b.json'].widgets.push(
    { type: 'list', name: 'map', rect: [0, 30, 100, 50], source: 'midi_map', rowHeight: 10, rowGap: 2, colGap: 4, columns: [{ width: 30, align: 'right', edit: 'int' }, { width: 60 }],
      rows: [['--', 'fixed'], ['a', '1'], ['b', '2'], ['c', '3']], scroll: { width: 5 }, context: [{ label: 'Remove', action: { midi_map: 'remove' } }, { label: 'Reset', action: { midi_map: 'reset' } }] },
    { type: 'button', name: 'learn', rect: [0, 90, 10, 10], toggle: true, param: 'gui.learn' },
    { type: 'dial', name: 'mod', rect: [30, 0, 10, 10], midi: 'cc7' });
  const st = L.playState(d), pc = L.playCtx(d, st, 'main', { measure: s => s.length * 5, lineH: 8 });
  click(pc, 125, 5); // page_b
  let map = find(pc, 'map');
  assert.deepEqual(map.rows.map(r => r.cells), [['--', 'fixed'], ['a', '1'], ['b', '2'], ['c', '3']]);
  // geometry: rows of 10 + 2, cells 30 and 60 wide 4 apart, a scrollbar (4 rows = 46 px in 50 px: none yet)
  assert.equal(map.list.bar, null);
  assert.deepEqual(map.list.rows[0].cells.map(c => c.rect), [[0, 30, 30, 10], [34, 30, 60, 10]]);
  assert.equal(map.list.rows.length, 5); // rows are drawn down to the bottom
  // learn: while learn is on, a click picks a host param as the target instead of working the widget
  click(pc, 5, 95);
  click(pc, 10, 10);
  assert.equal(st.learn, 'op.b.amt');
  assert.equal(val(pc, 'amt'), 50);
  L.playMidiIn(pc, { cc: 7, value: 127 });
  assert.deepEqual([st.learn, st.midiMap], [null, { 7: 'op.b.amt' }]);
  assert.equal(val(pc, 'amt'), 100); // the assigned CC moves its param
  assert.ok(Math.abs(val(pc, 'mod') - 1) < 1e-9); // a dial with midi "cc7" follows too
  L.playMidiIn(pc, { cc: 7, value: 0 });
  assert.equal(val(pc, 'amt'), 0);
  map = find(pc, 'map');
  assert.deepEqual(map.rows[0], { cc: 7, cells: ['7', 'B Amount'] });
  assert.ok(map.list.bar); // 5 rows now: it scrolls
  assert.equal(L.playWheel(pc, 50, 40, -1), true);
  assert.equal(st.scroll[map.key], 8); // a row per notch, but 58 - 50 is all there is
  L.playWheel(pc, 50, 40, 1);
  // a click selects a row, a double-click on an int cell edits it; the CC cell moves the assignment
  st.events.length = 0;
  L.playDown(pc, 10, 32); L.playUp(pc, 10, 32);
  assert.equal(st.sel[map.key], 0);
  L.playDown(pc, 10, 32, { dbl: true });
  assert.deepEqual(st.events[0], { edit: map.key, cell: [0, 0], text: '7', rect: [0, 30, 30, 10] });
  L.playUp(pc, 10, 32);
  assert.equal(L.playCommit(pc, map.key, 'x', [0, 0]), false);
  assert.equal(L.playCommit(pc, map.key, '9', [0, 0]), true);
  assert.deepEqual(st.midiMap, { 9: 'op.b.amt' });
  L.playCommit(pc, map.key, 'hello', [1, 1]);
  assert.deepEqual(find(pc, 'map').rows[1].cells, ['--', 'hello']);
  // Delete removes the selected assignment; the context menu's remove and reset work on the list
  L.playDown(pc, 10, 32); L.playUp(pc, 10, 32);
  assert.equal(L.playKey(pc, 'Delete'), true);
  assert.deepEqual(st.midiMap, {});
  st.midiMap = { 3: 'x.count', 4: 'x.mode' };
  pc.rebuild();
  L.playDown(pc, 10, 44, { right: true }); L.playUp(pc, 10, 44, { right: true }); // row 1 (cc 4) selected, menu open
  assert.equal(st.sel[map.key], 1);
  const lay = L.menuLayouts(pc)[0];
  click(pc, lay.rows[0].rect[0] + 2, lay.rows[0].rect[1] + 2);
  assert.deepEqual(st.midiMap, { 3: 'x.count' });
  L.playDown(pc, 10, 44, { right: true }); L.playUp(pc, 10, 44, { right: true });
  click(pc, lay.rows[1].rect[0] + 2, lay.rows[1].rect[1] + 2);
  assert.deepEqual(st.midiMap, {});
  // incoming notes are held keys
  L.playMidiIn(pc, { note: 61, velocity: 100 });
  assert.deepEqual(st.notesIn, [61]);
  L.playMidiIn(pc, { note: 61, velocity: 0 });
  assert.deepEqual(st.notesIn, []);
});

await test('play: reset, wheel, zeroText, editOn, sources, enableIf on embeds, piano options', () => {
  const d = playDocs(), pa = d['views/page_a.json'].widgets, mw = d['views/main.json'].widgets;
  Object.assign(pa.find(w => w.name === 'plain'), { reset: 'zero', wheel: 0.1, zeroText: '-' });
  Object.assign(pa.find(w => w.name === 'amt'), { reset: false, wheel: 0 });
  Object.assign(mw.find(w => w.name === 'note'), { editOn: 'click' });
  Object.assign(mw.find(w => w.name === 'piano'), { velocity: 90, glide: false });
  mw.push({ type: 'textbox', name: 'cpu', rect: [0, 0, 1, 1], layer: 0, source: 'cpu', format: '%3d%%' },
    { type: 'plate', name: 'mod', rect: [0, 0, 1, 1], layer: 0, source: 'modified' }, { type: 'plate', name: 'in', rect: [0, 0, 1, 1], layer: 0, source: 'midi_in' },
    { type: 'textbox', name: 'voices', rect: [0, 0, 1, 1], layer: 0, source: 'voices' });
  const st = L.playState(d), pc = L.playCtx(d, st, 'main');
  assert.deepEqual([find(pc, 'cpu').text, find(pc, 'mod').source, find(pc, 'in').source, find(pc, 'voices').text], ['  0%', 0, 0, '0']);
  L.playDown(pc, 30, 15); L.playMove(pc, 30, 5); L.playUp(pc, 30, 5);
  assert.ok(Math.abs(val(pc, 'plain') - 5) < 1e-9);
  assert.equal(find(pc, 'mod').source, 1); // modified
  L.playDown(pc, 30, 15, { dbl: true }); // reset "zero"
  assert.equal(val(pc, 'plain'), 0);
  assert.equal(L.numberText(find(pc, 'plain').w, find(pc, 'plain').p, 0), '-');
  // "valueText": a data table's cell, its row a param's value and its column the widget's own value less first
  const vtCtx = { data: { names: [['a', 'b'], ['c', 'd', '']] }, pmap: new Map([['x.row', { id: 'x.row', min: 0, max: 1 }]]), st: { values: { 'x.row': 1 } } };
  const vtW = { valueText: { table: 'names', row: 'x.row', first: 10 }, format: '%.0f' }, vtIt = { vt_row: 'x.row' };
  assert.equal(L.numberText(vtW, null, 11, vtCtx, vtIt), 'd');
  assert.equal(L.numberText(vtW, null, 12, vtCtx, vtIt), '12');   // an empty cell: the plain number
  assert.equal(L.numberText({ valueText: { scale: 0.1, format: '%.1f' } }, null, 2200), '220.0');
  assert.equal(L.playWheel(pc, 30, 15, 1), true); // wheel 0.1 of the range
  assert.ok(Math.abs(val(pc, 'plain') - 10) < 1e-9);
  L.playDown(pc, 10, 10, { dbl: true }); // reset false: nothing
  assert.ok(Math.abs(val(pc, 'amt') - 10) < 1e-9);
  assert.equal(L.playWheel(pc, 10, 10, 1), false); // wheel 0: off (and nothing scrolls here)
  pc.now = 5000;
  L.playMidiIn(pc, { cc: 1, value: 1 });
  assert.equal(find(pc, 'in').source, 1);
  pc.now = 5200; pc.rebuild();
  assert.equal(find(pc, 'in').source, 0);
  // editOn "click": one click edits the text data
  st.events.length = 0;
  click(pc, 180, 115);
  assert.equal(st.events[0].edit, 'main/note');
  // a fixed velocity, and no retrigger without glide
  st.events.length = 0;
  L.playDown(pc, 101, 140); L.playMove(pc, 111, 140);
  assert.equal(find(pc, 'voices').text, '1');
  L.playUp(pc, 111, 140);
  assert.deepEqual(st.events.map(e => e.midi), ['note on 60 velocity 90', 'note off 60']);
  // enableIf on an embed disables everything inside it
  mw.find(w => w.name === 'pages').enableIf = 'x.on';
  pc.rebuild();
  assert.equal(find(pc, 'amt').enabled, false);
  assert.equal(L.playHit(pc.root, 10, 10), null);
});

await test('play: fit, stack conditions, animated flow', () => {
  const d = playDocs(), mw = d['views/main.json'].widgets;
  mw.find(w => w.name === 'list').fit = true;
  mw.push({ type: 'plate', name: 'marker', rect: [0, 0, 1, 1], showIf: { stack: 'pages', view: 'page_b' } },
    { type: 'plate', name: 'marker_b', rect: [0, 0, 1, 1], showIf: { stack: 'pages', view: 'page_b', vars: { op: 'b' } } });
  let st = L.playState(d), pc = L.playCtx(d, st, 'main');
  assert.deepEqual(find(pc, 'list').rect, [0, 100, 100, 46]); // the embed takes its view's size
  assert.equal(find(pc, 'list').bar, undefined);
  assert.deepEqual([find(pc, 'marker').show, find(pc, 'marker_b').show], [false, false]);
  click(pc, 125, 5);
  assert.deepEqual([find(pc, 'marker').show, find(pc, 'marker_b').show], [true, true]);
  // a fitting root view shrinks to the widgets it shows (the editor may keep hidden ones)
  d['views/main.json'].fit = true;
  for (const n of ['piano', 'note', 'status']) mw.find(w => w.name === n).hidden = true;
  pc = L.playCtx(d, L.playState(d), 'main');
  assert.deepEqual([pc.root.w, pc.root.h], [190, 146]);
  assert.deepEqual([L.playCtx(d, L.playState(d), 'main', { showAll: true }).root.w, L.playCtx(d, L.playState(d), 'main', { showAll: true }).root.h], [200, 158]); // the list keeps its hidden row too
  // animated flow: a widget grows by `animate` px per frame, pushing the rest down, then shrinks
  const d2 = playDocs();
  Object.assign(d2['views/col.json'], { animate: 4 });
  d2['views/col.json'].widgets[4].showIf = 'x.mode';
  d2['views/main.json'].widgets.find(w => w.name === 'list').scroll.follow = true;
  st = L.playState(d2); pc = L.playCtx(d2, st, 'main');
  const row = n => find(pc, `row_${n}`);
  assert.deepEqual([row(1).vis, row(2).rect[1]], [false, 112]);
  st.values['x.on'] = 1; pc.rebuild();
  assert.equal(L.playAnimating(pc), true);
  assert.equal(row(1).vis, false); // still 0 px tall
  L.playFrame(pc);
  assert.deepEqual([row(1).vis, row(1).box[3], row(2).rect[1]], [true, 4, 118]);
  L.playFrame(pc); L.playFrame(pc);
  assert.deepEqual([row(1).box[3], row(2).rect[1], L.playAnimating(pc)], [10, 124, false]);
  assert.equal(L.playFrame(pc), false);
  st.values['x.on'] = 0; pc.rebuild();
  L.playFrame(pc);
  assert.deepEqual([row(1).vis, row(1).box[3]], [true, 6]); // shrinking before it hides
  L.playFrame(pc); L.playFrame(pc);
  assert.equal(row(1).vis, false);
  // scroll "follow" keeps a widget growing at the bottom in view
  const key = find(pc, 'list').key;
  st.values['x.mode'] = 1; pc.rebuild();
  while (L.playFrame(pc));
  assert.equal(st.scroll[key], 16);
  assert.equal(find(pc, 'list').bar.max, 16);
});

await test('list columns and rows in the inspector', () => {
  const cols = L.parseColumns('33 right int\n196\n', [{ width: 1, extra: 'kept' }]);
  assert.deepEqual(cols, [{ extra: 'kept', width: 33, align: 'right', edit: 'int' }, { width: 196 }]);
  assert.equal(L.formatColumns(cols), '33 right int\n196');
  assert.equal(L.parseColumns('  '), undefined);
  assert.deepEqual(L.parseRows('1 | Mod wheel\n\n64|Sustain'), [['1', 'Mod wheel'], ['64', 'Sustain']]);
  assert.equal(L.formatRows([['1', 'Mod wheel'], ['64', 'Sustain']]), '1 | Mod wheel\n64 | Sustain');
  const w = { rowHeight: 10, rowGap: 2, header: { text: 'H' }, scroll: { width: 6, always: true } };
  const lay = L.listLayout(w, [0, 0, 100, 40], 1, 0);
  assert.deepEqual([lay.head, lay.body, lay.bar.track, lay.rows.length, lay.rows[0].rect], [10, [0, 10, 100, 30], [94, 10, 6, 30], 3, [0, 10, 94, 10]]);
});

await test('play: sequence insert and delete', () => {
  const d = playDocs();
  for (let n = 1; n <= 4; n++) d['params.json'].push({ id: `seq.${n}.on`, name: `On ${n}`, min: 0, max: 1, steps: 1 }, { id: `seq.${n}.vel`, name: `Vel ${n}`, min: 0, max: 127, default: 100 });
  d['views/main.json'].widgets.push(
    { type: 'button', name: 'ins', rect: [140, 20, 10, 10], action: { sequence: 'insert', prefix: 'seq.', index: '{step}', count: 4 } },
    { type: 'button', name: 'del', rect: [140, 30, 10, 10], action: { sequence: 'delete', prefix: 'seq.', index: 2, count: 4 } });
  d['skin.json'].vars.step = '2';
  const st = L.playState(d), pc = L.playCtx(d, st, 'main');
  Object.assign(st.values, { 'seq.1.vel': 11, 'seq.2.vel': 22, 'seq.3.vel': 33, 'seq.4.vel': 44, 'seq.2.on': 1 });
  const vels = () => [1, 2, 3, 4].map(n => st.values[`seq.${n}.vel`] ?? 100), ons = () => [1, 2, 3, 4].map(n => st.values[`seq.${n}.on`] ?? 0);
  click(pc, 145, 25); // insert at {step} = 2: 2..3 move up, 4 drops off, 2 resets
  assert.deepEqual([vels(), ons()], [[11, 100, 22, 33], [0, 0, 1, 0]]);
  click(pc, 145, 35); // delete at 2: 3..4 move down, 4 resets
  assert.deepEqual([vels(), ons()], [[11, 22, 33, 100], [0, 1, 0, 0]]);
  st.vars.step = '9'; pc.rebuild(); click(pc, 145, 25); // out of range: nothing
  assert.deepEqual(vels(), [11, 22, 33, 100]);
});

await test('compound conditions, mirrored params, number drag details', () => {
  const vals = { a: 1, b: 0 }, ps = new Map([['a', { min: 0, max: 1 }], ['b', { min: 0, max: 1 }]]);
  const env = { param: id => (ps.has(id) ? { v: vals[id], p: ps.get(id) } : null), variable: k => ({ op: 'x' })[k], sub: s => s };
  const c = { any: [{ all: ['a', { param: 'b', equals: 1 }] }, { all: [{ var: 'op', equals: 'x' }, { param: 'a', atLeast: 1 }] }] };
  assert.equal(L.evalCondition(c, env), true);
  vals.a = 0;
  assert.equal(L.evalCondition(c, env), false);
  assert.equal(L.evalCondition({ all: [] }, env), true);
  assert.equal(L.evalCondition({ any: [] }, env), false);
  assert.deepEqual(L.makeCond(L.condForm(c)), c); // edited as JSON
  assert.equal(L.condForm(c).kind, 'json:all');
  assert.equal(L.makeCond({ kind: 'json:all', value: '{ nope' }), undefined);
  // references inside compound conditions and mirror lists follow renames
  const d = playDocs(), mw = d['views/main.json'].widgets;
  d['params.json'].push({ id: 'm.a', name: 'Mirror A', min: 0, max: 10 }, { id: 'm.b', name: 'Mirror B', min: 0, max: 1, steps: 1 });
  mw.push({ type: 'plate', name: 'cmp', rect: [0, 0, 1, 1], showIf: { any: [{ param: 'x.on' }, { all: [{ stack: 'pages', view: 'page_b' }, 'x.mode'] }] } });
  assert.deepEqual(L.refs(d).filter(r => r.widget === 'cmp').map(r => `${r.kind} ${r.name}`), ['param x.on', 'stack pages', 'view page_b', 'param x.mode']);
  // Play: mirror writes the same normalized value, {var} resolved; zones set the drag rate; drag false does not drag
  const pa = d['views/page_a.json'].widgets;
  pa.find(w => w.name === 'amt').mirror = ['m.{op}', 'm.b', 'no.such'];
  Object.assign(pa.find(w => w.name === 'plain'), { rect: [20, 10, 40, 10], sensitivityZones: [1, 0.1] });
  Object.assign(pa.find(w => w.name === 'num'), { drag: false });
  const st = L.playState(d), pc = L.playCtx(d, st, 'main');
  L.playDown(pc, 10, 10); L.playMove(pc, 10, -40); L.playUp(pc, 10, -40);
  assert.ok(Math.abs(val(pc, 'amt') - 25) < 1e-9);
  assert.deepEqual([st.values['m.a'], st.values['m.b']], [2.5, 0]);
  L.playWheel(pc, 10, 10, 10); // 0.5 of the range more: the stepped mirror snaps
  assert.deepEqual([st.values['m.a'], st.values['m.b']], [7.5, 1]);
  L.playDown(pc, 25, 15); L.playMove(pc, 25, 5); L.playUp(pc, 25, 5); // left column: 1 unit per pixel
  assert.ok(Math.abs(val(pc, 'plain') - (75 + 10)) < 1e-9);
  L.playDown(pc, 55, 15); L.playMove(pc, 55, 5); L.playUp(pc, 55, 5); // right column: 0.1 per pixel
  assert.ok(Math.abs(val(pc, 'plain') - 86) < 1e-9);
  L.playDown(pc, 30, 5); L.playMove(pc, 30, -50); L.playUp(pc, 30, -50);
  assert.equal(val(pc, 'num'), 0);
  st.events.length = 0;
  L.playDown(pc, 30, 5, { dbl: true });
  assert.equal(st.events[0].edit, 'main/pages>page_a/num'); // still editable
});

await test('key image layout, wheel snapping, presets', () => {
  // key images: a 130 px octave, keys at the art's offsets, black keys the black image's size
  const k = L.pianoKeys(36, 13, [5, 0, 300, 100], [12, 60]);
  const at = n => k.find(x => x.note === n).rect;
  assert.deepEqual([at(36), at(38), at(47), at(48), at(37), at(46)], [[5, 0, 19, 100], [24, 0, 19, 100], [117, 0, 18, 100], [135, 0, 19, 100], [16, 0, 12, 60], [112, 0, 12, 60]]);
  assert.deepEqual(L.pianoKeys(38, 3, [0, 0, 300, 100], [12, 60]).map(x => x.rect[0]), [0, 19, 14]); // starting on D: D sits at 0
  // at density 1.5 the key art's offsets scale: a 195 px octave, E at 57, A at 141
  const k2 = L.pianoKeys(36, 13, [0, 0, 450, 150], [18, 90], 1.5), at2 = n => k2.find(x => x.note === n).rect;
  assert.deepEqual([at2(40), at2(45), at2(48), at2(37)], [[57, 0, 27, 150], [141, 0, 27, 150], [195, 0, 29, 150], [17, 0, 18, 90]]);
  assert.equal(L.padHandle([0, 0, 200, 200], 1.5), 21);
  assert.deepEqual(L.menuStyleOf({ density: 1.5, menu: { font: 'f', rowHeight: 24 } }), { pad: [6, 3, 6, 3], separatorHeight: 5, font: 'f', rowHeight: 24 });
  assert.deepEqual(L.menuStyleOf({ menu: { font: 'f' } }), { font: 'f' });
  assert.equal(L.dp(-1, 1.5), -2);
  // the wheel accumulates the fraction and snaps a stepped value to the nearest step
  const d = playDocs();
  d['views/main.json'].widgets.push({ type: 'dial', name: 'sw', rect: [140, 20, 10, 10], range: [0, 10], steps: 2, wheel: 0.1 });
  const st = L.playState(d), pc = L.playCtx(d, st, 'main');
  const seen = [1, 2, 3, 4, 5, 6, 7, 8].map(() => (L.playWheel(pc, 145, 25, 1), val(pc, 'sw')));
  assert.deepEqual(seen, [0, 0, 5, 5, 5, 5, 5, 10]);
  L.playWheel(pc, 145, 25, -2, { shift: true }); // Shift: fine (0.1) of the fraction
  assert.equal(val(pc, 'sw'), 10);
  // presets: a native menu of the table's slots; a slot loads its content and name into text data
  d['views/main.json'].widgets.push({ type: 'button', name: 'pre', rect: [140, 30, 10, 10], action: { presets: 'env', key: 'env.{op}', name: 'env.{op}.name', save: true, columns: 1 } });
  const data = { env: { presets: [{ name: 'Gate', points: [1] }, { name: 'DR', points: [2] }] } };
  const st2 = L.playState(d), pc2 = L.playCtx(d, st2, 'main', { data });
  click(pc2, 145, 35);
  assert.deepEqual(st2.menu.levels[0].items.map(x => x.label ?? '-'), ['Gate', 'DR', '-', 'Save']);
  assert.equal(L.menuStyleNow(pc2), pc2.nativeMenu);
  const lay = L.menuLayouts(pc2)[0];
  assert.equal(lay.rows[1].rect[0] > lay.rows[0].rect[0], true); // `columns: 1`: one item a column
  click(pc2, lay.rows[1].rect[0] + 1, lay.rows[1].rect[1] + 1);
  assert.deepEqual([st2.text['env.a'], st2.text['env.a.name']], ['{"points":[2]}', 'DR']);
  // Save stores the current content and name into a slot, which the menu then offers
  Object.assign(st2.text, { 'env.a': '{"points":[9]}', 'env.a.name': 'Mine' });
  click(pc2, 145, 35);
  L.playKey(pc2, 'ArrowUp'); L.playKey(pc2, 'ArrowRight'); L.playKey(pc2, 'Enter');
  assert.deepEqual(JSON.parse(st2.text['presets.env']), [{ name: 'Mine', points: [9] }, { name: 'DR', points: [2] }]);
  click(pc2, 145, 35);
  assert.equal(st2.menu.levels[0].items[0].label, 'Mine');
});

await test('edit all and sequence reset / random', () => {
  const d = playDocs();
  d['skin.json'].editAll = { param: 'x.all', var: 'op', values: ['a', 'b'] };
  d['params.json'].push({ id: 'x.all', name: 'Edit all', min: 0, max: 1, steps: 1, host: false });
  const st = L.playState(d), pc = L.playCtx(d, st, 'main');
  L.playDown(pc, 10, 10); L.playMove(pc, 10, -40); L.playUp(pc, 10, -40); // op.a.amt only
  assert.deepEqual([st.values['op.a.amt'], st.values['op.b.amt']], [25, undefined]);
  st.values['x.all'] = 1; pc.rebuild();
  L.playDown(pc, 10, 10); L.playMove(pc, 10, -30); L.playUp(pc, 10, -30); // every value's param, same gesture
  assert.deepEqual([st.values['op.a.amt'], st.values['op.b.amt']], [45, 45]);
  // sequence reset / random on one leaf
  for (let n = 1; n <= 3; n++) d['params.json'].push({ id: `s.${n}.on`, name: `On ${n}`, min: 0, max: 2, steps: 2, default: 1 }, { id: `s.${n}.v`, name: `V ${n}`, min: 0, max: 1 });
  d['views/main.json'].widgets.push({ type: 'button', name: 'rnd', rect: [140, 20, 10, 10], action: { sequence: 'random', prefix: 's.', leaf: 'on', count: 3 } },
    { type: 'button', name: 'rst', rect: [140, 30, 10, 10], action: { sequence: 'reset', prefix: 's.', leaf: 'on', count: 3 } });
  const rolls = [0.1, 0.5, 0.99];
  const st2 = L.playState(d), pc2 = L.playCtx(d, st2, 'main', { random: () => rolls.shift() });
  st2.values['s.2.v'] = 0.5;
  click(pc2, 145, 25);
  assert.deepEqual([1, 2, 3].map(n => st2.values[`s.${n}.on`]), [0, 1, 2]); // valid stepped values
  click(pc2, 145, 35);
  assert.deepEqual([1, 2, 3].map(n => st2.values[`s.${n}.on`]), [undefined, undefined, undefined]); // defaults
  assert.equal(st2.values['s.2.v'], 0.5); // other leaves untouched
});

await test('learn defaults, fixed rows, data action, key scaling points', () => {
  const d = playDocs();
  d['skin.json'].learn = { param: 'gui.learn', defaults: { 7: 'op.a.amt' } };
  d['params.json'].push({ id: 'gui.learn', name: 'Learn', min: 0, max: 1, steps: 1, host: false }, { id: 'cc.one', name: 'CC One', min: 0, max: 127, steps: 127, default: 4, host: false });
  d['views/page_a.json'].widgets.push(
    { type: 'list', name: 'map', rect: [0, 50, 100, 50], source: 'midi_map', rowHeight: 10, columns: [{ width: 30, edit: 'int' }, { width: 60 }],
      fixed: [{ param: 'x.mode' }, { param: 'op.a.amt' }, { label: 'Controller 1', ccParam: 'cc.one' }], context: [{ label: 'Reset', action: { midi_map: 'reset' } }] },
    { type: 'button', name: 'one', rect: [60, 30, 10, 10], action: { data: { 'shape.{op}': '0 -60 0.5|127 67 0.5' } } });
  const st = L.playState(d), pc = L.playCtx(d, st, 'main');
  assert.deepEqual(st.midiMap, { 7: 'op.a.amt' }); // a fresh instance starts with the defaults
  const rows = () => find(pc, 'map').rows.map(r => r.cells);
  assert.deepEqual(rows(), [['7', 'A Amount'], ['-1', 'Mode'], ['4', 'Controller 1']]); // op.a.amt is listed already
  const key = find(pc, 'map').key;
  L.playCommit(pc, key, '21', [1, 0]); // a fixed param row: assigns that CC
  L.playCommit(pc, key, '12', [2, 0]); // a ccParam row (now the third, as Mode is listed as assigned): sets the param
  assert.deepEqual([st.midiMap, st.values['cc.one']], [{ 7: 'op.a.amt', 21: 'x.mode' }, 12]);
  assert.deepEqual(rows(), [['7', 'A Amount'], ['21', 'Mode'], ['12', 'Controller 1']]);
  L.runAction(pc, find(pc, 'map'), { midi_map: 'reset' }); // back to the defaults
  assert.deepEqual(st.midiMap, { 7: 'op.a.amt' });
  assert.deepEqual(L.parseFixed('x.mode\nController 1 | cc.one\n'), [{ param: 'x.mode' }, { label: 'Controller 1', ccParam: 'cc.one' }]);
  assert.equal(L.formatFixed([{ param: 'x.mode' }, { label: 'Controller 1', ccParam: 'cc.one' }]), 'x.mode\nController 1 | cc.one');
  click(pc, 65, 35); // the data action stores its strings as text data, {var} resolved
  assert.equal(st.text['shape.a'], '0 -60 0.5|127 67 0.5');
  assert.deepEqual(L.keyscalePoints(st.text['shape.a']), [[0, -60, 0.5], [127, 67, 0.5]]);
  assert.deepEqual(L.keyscalePoints('0 0 0.5|60 -3 0.7|130 0 0.5|127 1 1;127 0 0.5'), [[0, 0, 0.5], [60, -3, 0.7], [127, 0, 0.5]]);
  assert.deepEqual(L.keyscalePoints(undefined), []);
});

await test('menuAt and native menu checks', () => {
  const d = playDocs();
  d['views/page_a.json'].widgets.push({ type: 'dropdown', name: 'at', rect: [60, 60, 30, 10], param: 'x.mode', menuAt: [5, 70], items: [{ label: 'A', value: 0 }, { label: 'On', check: true, action: { set: { timbre: 'tl' } } }] },
    { type: 'button', name: 'pre', rect: [60, 80, 30, 10], menuAt: [40, 20], action: { presets: 'env', key: 'k', name: 'n' } });
  const st = L.playState(d), pc = L.playCtx(d, st, 'main', { data: { env: { presets: [{ name: 'Gate' }] } } });
  click(pc, 65, 65);
  assert.deepEqual(L.menuLayouts(pc)[0].rect.slice(0, 2), [5, 70]); // top-left at the view point (page_a sits at 0, 0)
  // native (no skin menu): only check items show a check, never the current value
  assert.deepEqual(st.menu.levels[0].items.map(x => L.menuChecked(pc, x)), [false, true]);
  L.playKey(pc, 'Escape');
  click(pc, 65, 85);
  assert.deepEqual(L.menuLayouts(pc)[0].rect.slice(0, 2), [40, 20]);
  // with a skin menu the item for the value is checked as before
  d['skin.json'].menu = { pad: [1, 1, 1, 1] };
  const st2 = L.playState(d), pc2 = L.playCtx(d, st2, 'main');
  click(pc2, 65, 65);
  assert.equal(L.menuChecked(pc2, st2.menu.levels[0].items[0]), true);
});

await test('references and checks for the new fields', () => {
  const d = playDocs();
  d['skin.json'].menu = { font: 'menu_font', check: 'icons/check' };
  d['skin.json'].images = { 'icons/check': { tiles: 2 } };
  d['views/main.json'].widgets.push({ type: 'plate', name: 'fx', rect: [0, 0, 1, 1], showIf: 'x.on', enableIf: { param: 'y.{op}' } });
  const rs = L.refs(d);
  assert.deepEqual(rs.filter(r => r.kind === 'param' && r.widget === 'fx').map(r => r.name), ['x.on', 'y.{op}']);
  assert.ok(rs.some(r => r.kind === 'image' && r.name === 't' && r.widget === 'list'));
  assert.ok(rs.some(r => r.kind === 'font' && r.name === 'menu_font' && r.path === 'skin.json'));
  assert.deepEqual(rs.filter(r => r.name === 'icons/check').map(r => !!r.meta), [true, false]);
  assert.equal(L.renameRefs(d, 'param', 'x.on', 'x.enabled'), 4); // two bindings, two showIfs
  assert.equal(d['views/main.json'].widgets.at(-1).showIf, 'x.enabled');
  d['skin.json'].menu.styles = { plain: { font: 'plain_font' } };
  d['skin.json'].learn = { param: 'x.mode' };
  d['skin.json'].fonts = { menu_font: { caret: '#ffffffff' } };
  d['views/main.json'].widgets.push({ type: 'custom', name: 'keys2', rect: [0, 0, 1, 1], kind: 'piano', keyImages: { c: 'keyboard/c', black: 'keyboard/black' }, showIf: { stack: 'pages', view: 'page_b' } });
  const rs2 = L.refs(d);
  assert.ok(rs2.some(r => r.kind === 'font' && r.name === 'plain_font'));
  assert.ok(rs2.some(r => r.kind === 'param' && r.name === 'x.mode' && r.path === 'skin.json'));
  assert.deepEqual(rs2.filter(r => r.widget === 'keys2').map(r => `${r.kind} ${r.name}`), ['stack pages', 'view page_b', 'image keyboard/c', 'image keyboard/black']);
  assert.equal(L.renameRefs(d, 'font', 'menu_font', 'menu_small'), 2); // the menu style and the font settings follow
  assert.deepEqual(Object.keys(d['skin.json'].fonts), ['menu_small']);
  d['views/col.json'].flow = 'row';
  const msgs = L.validate(d, { images: new Set(['t', 'h', 'icons/check']), fonts: new Set(['menu_font']) }).map(i => i.msg);
  assert.ok(msgs.includes('unknown flow "row"'));
  assert.ok(msgs.includes('missing param "y.{op}"'));
});

console.log(process.exitCode ? 'Some tests FAILED' : `All ${count} test groups passed`);
