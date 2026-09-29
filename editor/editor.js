// Hollow skin editor, browser side: DOM, canvas and file traffic. The format rules themselves
// (tiles, nine-slice, fonts, button states, references, JSON layout) live in logic.mjs.
import * as L from './logic.mjs';

const $ = s => document.querySelector(s);
function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v == null || v === false) continue;
    if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k in el && k !== 'list') el[k] = v;
    else el.setAttribute(k, v);
  }
  el.append(...kids.flat(Infinity).filter(c => c != null && c !== false).map(c => (c instanceof Node ? c : String(c))));
  return el;
}
const clone = o => JSON.parse(JSON.stringify(o));
const validRect = L.validRect;
const groupOf = n => (n && n.includes('/') ? n.slice(0, n.lastIndexOf('/')) : '');
const where = r => r.path + (r.widget ? ` / ${r.widget}` : '');
const GROUPS = ['backgrounds', 'panels', 'knobs', 'faders', 'buttons', 'switches', 'leds', 'meters', 'displays', 'icons', 'keyboard', 'branding', 'scrollbars', 'library', 'dialogs', 'misc'];
const MARGIN = 40; // screen px around the canvas content

// ---- state ------------------------------------------------------------------------------------

const S = {
  docs: {},   // 'skin.json', 'params.json', 'views/<name>.json' -> parsed JSON
  saved: {},  // path -> compact JSON text as last loaded or saved
  bins: {},   // path -> Blob to write, or null to delete, on the next save (images and fonts)
  binRev: 0,
  files: {},  // path -> { size, mtime } on disk
  hist: [], hi: -1,
  view: null, mode: 'view', image: null, font: null, param: null, showParam: false,
  sel: [], hover: null, drag: null, guides: [], mouse: null, back: [], clip: null,
  zoom: 2, showHidden: true, showBoxes: false, explode: false, pv: null, cells: [], pvRect: null,
  sample: 'The quick brown fox jumps over the lazy dog\n0123456789 !?%&()', tab: 'views',
  pmap: new Map(),
  data: {},   // data/<table>.json, read-only (preset tables for Play mode)
  items: new Map(), // widget of the open view -> its play-tree item (rect after flow, shown or not)
  pc: null,         // the play context the canvas was last drawn from
  playing: false, play: null, inline: null, // Play mode: on, its state (logic.mjs playState), the inline editor
  loadingJson: false, imgTotal: 0, imgDone: 0, task: null, // what the busy indicator shows
};
const skin = () => S.docs['skin.json'];
const params = () => S.docs['params.json'] || [];
const views = () => L.viewNames(S.docs);
const vdoc = n => S.docs[L.viewPath(n)];
const cur = () => vdoc(S.view);
const ip = n => `images/${n}.png`, fp = n => `fonts/${n}.png`;
const imeta = n => skin()?.images?.[n] || {};
const exists = p => (p in S.bins ? !!S.bins[p] : !!S.files[p]);
function binPaths(prefix, flat) {
  const all = new Set([...Object.keys(S.files), ...Object.keys(S.bins)]);
  return [...all].filter(p => p.startsWith(prefix) && p.endsWith('.png') && exists(p) && !(flat && p.slice(prefix.length).includes('/'))).sort();
}
const imageNames = () => binPaths('images/').map(p => p.slice(7, -4));
const fontNames = () => binPaths('fonts/', true).map(p => p.slice(6, -4));

// ---- server -------------------------------------------------------------------------------------

const enc = p => p.split('/').map(encodeURIComponent).join('/');
async function req(method, url, body) {
  const headers = { ...(method === 'GET' ? {} : { 'X-Hollow': '1' }), ...(S.token ? { 'X-Hollow-Skin': S.token } : {}) };
  const r = await fetch(url, { method, body, headers });
  if (!r.ok) throw new Error(`${method} ${url}: ${r.status} ${await r.text()}`);
  return r;
}
const api = {
  list: async (root = 'skin') => (await req('GET', `/api/list?root=${root}`)).json(),
  get: (root, p) => req('GET', `/${root}/${enc(p)}`),
  put: (root, p, body) => req('PUT', `/${root}/${enc(p)}`, body),
  del: (root, p) => req('DELETE', `/${root}/${enc(p)}`),
};
const bytesOf = async r => new Uint8Array(await r.arrayBuffer());

// ---- messages -----------------------------------------------------------------------------------

function msg(text, bad) {
  const el = $('#msg');
  el.textContent = text;
  el.className = bad ? 'bad' : '';
  clearTimeout(msg.t);
  msg.t = setTimeout(() => { el.textContent = ''; }, bad ? 15000 : 6000);
}
const run = fn => async (...a) => { try { await fn(...a); } catch (e) { console.error(e); msg(e.message, true); } };

// The busy indicator over the canvas: skin files loading, images decoding, or a running task.
function showBusy() {
  const [text, frac] = S.task ? [S.task.text, S.task.frac] : S.loadingJson ? ['Loading skin files', null]
    : S.imgTotal > S.imgDone ? [`Loading images ${S.imgDone} / ${S.imgTotal}`, S.imgDone / S.imgTotal] : [''];
  $('#busy').hidden = !text;
  if (!text) return;
  $('#busy-text').textContent = text;
  $('#busy-bar').style.width = frac == null ? '100%' : `${Math.round(frac * 100)}%`;
}
// Runs fn over items, 8 at a time, with a progress readout.
async function task(label, items, fn) {
  try { return await L.pool(items, 8, fn, (d, n) => { S.task = { text: `${label} ${d} / ${n}`, frac: d / n }; showBusy(); }); }
  finally { S.task = null; showBusy(); }
}

// ---- history (snapshots of every JSON doc as text, plus pending binaries) --------------------------

function snapshot() {
  const prev = S.hist[S.hi]?.texts || {}, texts = {};
  for (const [p, d] of Object.entries(S.docs)) { const t = JSON.stringify(d); texts[p] = t === prev[p] ? prev[p] : t; }
  return { texts, bins: { ...S.bins } };
}
function sameSnap(a, b) {
  const ka = Object.keys(a.texts), ba = Object.keys(a.bins);
  return ka.length === Object.keys(b.texts).length && ka.every(k => a.texts[k] === b.texts[k])
    && ba.length === Object.keys(b.bins).length && ba.every(k => a.bins[k] === b.bins[k]);
}
// Record the current state as one undo step. Steps with the same merge key within 1.5 s fold together.
function commit(key) {
  const snap = snapshot(), top = S.hist[S.hi];
  if (top && sameSnap(top, snap)) return;
  snap.key = key; snap.t = Date.now();
  if (key && top?.key === key && snap.t - top.t < 1500 && S.hi > 0 && S.hi === S.hist.length - 1) S.hist[S.hi] = snap;
  else {
    S.hist.length = S.hi + 1;
    S.hist.push(snap);
    if (S.hist.length > 400) S.hist.shift();
    S.hi = S.hist.length - 1;
  }
  changed();
}
function go(i) {
  const now = S.hist[S.hi].texts, snap = S.hist[i];
  for (const p of Object.keys(S.docs)) if (!(p in snap.texts)) delete S.docs[p];
  for (const [p, t] of Object.entries(snap.texts)) if (now[p] !== t || !(p in S.docs)) S.docs[p] = JSON.parse(t);
  S.bins = { ...snap.bins }; S.binRev++; S.hi = i;
  if (!cur()) S.view = vdoc(skin()?.root) ? skin().root : views()[0];
  if (S.param && !params().some(p => p.id === S.param)) S.showParam = false;
  const names = S.sel.map(w => w.name), ws = cur()?.widgets || [];
  S.sel = names.map(n => ws.find(w => w.name === n)).filter(Boolean);
  if (S.mode === 'image' && !exists(ip(S.image)) || S.mode === 'font' && !exists(fp(S.font))) S.mode = 'view';
  changed();
}
function undo() { if (S.playing) return; commit(); if (S.hi > 0) go(S.hi - 1); }
function redo() { if (S.playing) return; commit(); if (S.hi < S.hist.length - 1) go(S.hi + 1); }
function dirtyPaths() {
  const t = S.hist[S.hi]?.texts || {};
  return [...Object.keys(t).filter(p => S.saved[p] !== t[p]), ...Object.keys(S.saved).filter(p => !(p in t)), ...Object.keys(S.bins)];
}

// ---- load and save --------------------------------------------------------------------------------

async function load() {
  S.loadingJson = true;
  showBusy();
  const bad = [];
  try {
    S.skins = await (await req('GET', '/api/skins')).json();
    S.token = S.skins.token;
    $('#b-skin').textContent = `${S.skins.current.name} ▾`;
    $('#b-skin').title = `${S.skins.current.path}
Open another skin`;
    document.title = `${S.skins.current.name} - Hollow skin editor`;
    const list = await api.list();
    S.files = Object.fromEntries(list.map(f => [f.path, f]));
    await Promise.all(list.map(f => f.path).filter(p => /^data\/[^/]+\.json$/.test(p)).map(async p => {
      try { S.data[p.slice(5, -5)] = JSON.parse(await (await api.get('skin', p)).text()); } catch (e) { bad.push(`${p}: ${e.message}`); }
    }));
    await Promise.all(list.map(f => f.path).filter(p => p === 'skin.json' || p === 'params.json' || /^views\/[^/]+\.json$/.test(p)).map(async p => {
      try { S.docs[p] = JSON.parse(await (await api.get('skin', p)).text()); S.saved[p] = JSON.stringify(S.docs[p]); }
      catch (e) { bad.push(`${p}: ${e.message}`); }
    }));
  } finally { S.loadingJson = false; showBusy(); }
  if (!skin()) {
    S.docs['skin.json'] = { format: 1, name: 'Untitled', root: 'main', images: {} };
    if (!views().length) S.docs['views/main.json'] = { size: [640, 400], fill: { colour: '#202020ff' }, widgets: [] };
    msg('No skin.json in this folder: started a new skin (Save creates the files).');
  }
  S.hist = [snapshot()]; S.hi = 0;
  S.play = L.playState(S.docs);
  if (bad.length) alert('These files could not be read and are left alone:\n' + bad.join('\n'));
  const root = skin().root;
  openView(vdoc(root) ? root : views()[0], false);
  fit();
  if (!S.skins.current.exists) skinDialog();
}

// ---- choosing the skin (the server serves one folder at a time; switching reloads the page) -----------------------

async function openSkin(path) {
  const n = dirtyPaths().length;
  if (n && !confirm(`Discard ${n} unsaved change${n === 1 ? '' : 's'} and open another skin?`)) return;
  await req('POST', '/api/open', JSON.stringify({ path }));
  S.leaving = true;
  location.reload();
}
function skinDialog() {
  const dlg = $('#dlg'), cur = S.skins.current;
  const path = h('input', { type: 'text', placeholder: 'A skin folder, e.g. E:/MySynth/skin', spellcheck: false, value: cur.exists ? '' : cur.path });
  const go = () => (path.value.trim() ? openSkin(path.value.trim()) : path.focus());
  path.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); run(go)(); } });
  const rows = S.skins.skins.map(e => h('div', { className: `skin${e.path === cur.path ? ' on' : ''}` },
    h('div', { className: 'what' }, h('b', {}, e.name), ' ', e.path === cur.path ? h('span', { className: 'tag' }, 'open') : null,
      h('div', { className: 'dim' }, e.rel, e.exists ? '' : ' (empty: a new skin)')),
    h('button', { type: 'button', onclick: run(() => openSkin(e.path)), disabled: e.path === cur.path && e.exists }, 'Open')));
  dlg.replaceChildren(h('form', { method: 'dialog', className: 'skins' }, h('h3', {}, 'Open a skin'),
    cur.exists ? null : h('p', { className: 'note' }, `${cur.path} holds no skin yet. Editing starts a new one, which Save writes there; or open another skin.`),
    rows,
    h('h3', {}, 'Another folder'),
    h('div', { className: 'ctl' }, path,
      h('button', { type: 'button', onclick: run(async () => { const r = await (await req('POST', '/api/browse')).json(); if (r.path) path.value = r.path; }) }, 'Browse...'),
      h('button', { type: 'button', onclick: run(go) }, 'Open')),
    h('p', { className: 'note' }, 'A folder without skin.json opens as a new, empty skin; Save creates its files.'),
    h('div', { className: 'btns' }, h('button', {}, 'Close'))));
  dlg.onclose = null;
  dlg.showModal();
}

async function save() {
  commit();
  const t = S.hist[S.hi].texts;
  const puts = Object.keys(t).filter(p => S.saved[p] !== t[p]), dels = Object.keys(S.saved).filter(p => !(p in t));
  const bins = Object.entries(S.bins);
  if (!puts.length && !dels.length && !bins.length) return msg('Nothing to save');
  let n = 0;
  try {
    // Images first, then JSON, then deletes, so a reloading runtime never sees a reference to a missing file.
    await task('Saving images', bins, async ([p, b]) => { if (b) await api.put('skin', p, b); else if (S.files[p]) await api.del('skin', p); delete S.bins[p]; n++; });
    await task('Saving', puts, async p => { await api.put('skin', p, L.stringify(JSON.parse(t[p])) + '\n'); S.saved[p] = t[p]; n++; });
    await task('Deleting', dels, async p => { if (S.files[p]) await api.del('skin', p); delete S.saved[p]; n++; });
    msg(`Saved ${n} file${n === 1 ? '' : 's'}`);
  } catch (e) { msg(`Saved ${n}, then failed: ${e.message}`, true); }
  S.files = Object.fromEntries((await api.list()).map(f => [f.path, f]));
  // Undo across a save restores JSON; binaries already written stay on disk (the limit of this simple history).
  S.hist[S.hi].bins = { ...S.bins };
  S.binRev++;
  changed();
}

// ---- images --------------------------------------------------------------------------------------

const cache = new Map(); // path -> { want, promise, rgba, bmp, w, h, font }
function srcKey(path) {
  if (path in S.bins) return S.bins[path];
  const f = S.files[path];
  return f ? `${f.size}:${f.mtime}` : null;
}
async function decodeAny(bytes) {
  try { return await L.pngDecode(bytes); } catch {
    // Not a PNG this decoder reads (interlaced, JPEG...): let the browser decode it.
    const bmp = await createImageBitmap(new Blob([bytes]), { premultiplyAlpha: 'none', colorSpaceConversion: 'none' });
    const c = h('canvas', { width: bmp.width, height: bmp.height }), g = c.getContext('2d');
    g.drawImage(bmp, 0, 0);
    const d = g.getImageData(0, 0, c.width, c.height);
    return { width: d.width, height: d.height, data: new Uint8Array(d.data.buffer) };
  }
}
const toBitmap = img => createImageBitmap(new ImageData(new Uint8ClampedArray(img.data.buffer, img.data.byteOffset, img.data.length), img.width, img.height));
const pngBlob = async img => new Blob([await L.pngEncode(img)], { type: 'image/png' });
async function loadImage(path, key) {
  const rgba = await decodeAny(key instanceof Blob ? new Uint8Array(await key.arrayBuffer()) : await bytesOf(await api.get('skin', path)));
  return { rgba, w: rgba.width, h: rgba.height, bmp: await toBitmap(rgba), font: path.startsWith('fonts/') ? L.parseFont(rgba) : null };
}
function entry(path) {
  const key = srcKey(path);
  if (key == null) return null;
  let e = cache.get(path);
  if (!e) cache.set(path, (e = {}));
  if (e.want !== key) {
    e.want = key;
    S.imgTotal++;
    showBusy();
    e.promise = loadImage(path, key).then(d => {
      if (e.want === key) { Object.assign(e, d); scheduleRender(); inspector(); }
      return e;
    }, err => { if (e.want === key) msg(`${path}: ${err.message}`, true); throw err; });
    e.promise.catch(() => {}).finally(() => { if (++S.imgDone >= S.imgTotal) S.imgTotal = S.imgDone = 0; showBusy(); });
  }
  return e;
}
// Loaded image (possibly the previous version while a new one decodes), or null.
function getImage(path) { const e = entry(path); return e?.bmp ? e : null; }
async function ensureImage(path) {
  const e = entry(path);
  if (!e) throw new Error(`missing ${path}`);
  await e.promise;
  return e;
}
async function blobOf(path) { return S.bins[path] || (await api.get('skin', path)).blob(); }
function setBin(path, blob) { S.bins[path] = blob; S.binRev++; }
function dropBin(path) { if (S.files[path]) S.bins[path] = null; else delete S.bins[path]; S.binRev++; }

// ---- rendering at scale 1 (docs/skin-format.md) ------------------------------------------------------

const base = document.createElement('canvas'), bx = base.getContext('2d');
const mx = document.createElement('canvas').getContext('2d'); // measures system-font text
const SYS_FONT = '11px system-ui, sans-serif';
const clipRect = (g, r) => { g.beginPath(); g.rect(r[0], r[1], r[2], r[3]); g.clip(); };
const fillCol = (g, r, c) => { const k = L.cssColour(c); if (k) { g.fillStyle = k; g.fillRect(...r); } };
// Tile `tile` of an image into r: nine-slice or 1:1 clipped (docs/skin-format.md); `stretch`
// scales an image without slice bands to the rect instead (scrollbar parts).
function drawImage(g, name, tile, r, stretch) {
  const e = getImage(ip(name));
  if (!e) return;
  const m = imeta(name), src = L.tileRect(e.w, e.h, m.tiles, m.axis, tile), pieces = stretch && !m.slice ? [[src, r]] : L.imagePieces(src, m.slice, r);
  for (let k = 0; k < Math.max(1, m.passes | 0); k++) for (const [s, d] of pieces) g.drawImage(e.bmp, ...s, ...d); // "passes": composited that many times
}
// The skin's surface (skin.json "surface") as the runtime lays it over widget artwork and fills, not
// text or menus: on the view canvas, a drawing is made apart, passed through L.applySurface at its
// window place and cached until the skin changes.
let surf = { key: null };
function surfaceFor(image) {
  const sf = skin()?.surface, tex = sf?.image && getImage(ip(sf.image)), nrm = sf?.normals && getImage(ip(sf.normals));
  if (!tex || (sf.normals && !nrm) || (image && imeta(image).surface === false)) return null;
  const key = JSON.stringify(sf) + srcKey(ip(sf.image)) + (sf.normals ? srcKey(ip(sf.normals)) : '');
  if (surf.key !== key) {
    const rel = Array.isArray(sf.relief) ? sf.relief : [];
    surf = { key, ramps: L.surfaceRamps(sf), tex: tex.rgba, normals: nrm?.rgba || null, relief: Math.max(0, rel[0] | 0), dents: Math.max(0, rel[1] | 0), out: new Map() };
  }
  return surf;
}
// A reflecting surface bends each image pixel's lookup by the drawing's own slope (fills have none).
function throughSurface(g, sf, r, key, paint, image = true) {
  const x0 = Math.max(0, Math.floor(r[0])), y0 = Math.max(0, Math.floor(r[1]));
  const x1 = Math.min(base.width, Math.ceil(r[0] + r[2])), y1 = Math.min(base.height, Math.ceil(r[1] + r[3]));
  if (x1 <= x0 || y1 <= y0) return;
  key += `|${x0},${y0},${x1},${y1}`;
  let c = sf.out.get(key);
  if (!c) {
    c = h('canvas', { width: x1 - x0, height: y1 - y0 });
    const cg = c.getContext('2d', { willReadFrequently: true });
    cg.imageSmoothingEnabled = false;
    cg.translate(-x0, -y0);
    paint(cg);
    const d = cg.getImageData(0, 0, c.width, c.height);
    const reflect = sf.relief || sf.normals ? { slope: image && sf.relief ? L.surfaceSlope(d.data, c.width, c.height) : null, relief: sf.relief, normals: sf.normals, dents: sf.dents } : null;
    L.applySurface(d.data, c.width, c.height, x0, y0, sf.tex, sf.ramps, reflect);
    cg.putImageData(d, 0, 0);
    if (sf.out.size > 600) sf.out.clear();
    sf.out.set(key, c);
  }
  g.drawImage(c, x0, y0);
}
// Widget artwork: an image through the surface (on the view canvas), else as drawImage.
let artOn = true;   // false inside a widget with "surface": false (and all an embed of it shows)
function drawArt(g, name, tile, r, stretch) {
  const sf = artOn && g === bx && getImage(ip(name)) && surfaceFor(name);
  if (!sf) return drawImage(g, name, tile, r, stretch);
  throughSurface(g, sf, r, `${name}|${tile}|${r.join()}|${stretch ? 1 : 0}|${srcKey(ip(name))}`, cg => drawImage(cg, name, tile, r, stretch));
}
function fillArt(g, r, colour) {
  const sf = artOn && g === bx && L.cssColour(colour) && surfaceFor(null);
  if (!sf) return fillCol(g, r, colour);
  throughSurface(g, sf, r, `#${colour}|${r.join()}`, cg => fillCol(cg, r, colour), false);
}
function drawFill(g, fill, r, tile) {
  if (!fill) return;
  fillArt(g, r, fill.colour);
  if (fill.image) drawArt(g, fill.image, fill.tile ?? tile, r);
}
function drawText(g, t, r, pad, str, dx = 0, dy = 0) {
  const e = t?.font && getImage(fp(t.font));
  if (!e?.font || !str) return;
  for (const q of L.layoutText(e.font, str, [r[0] + dx, r[1] + dy, r[2], r[3]], t, pad)) g.drawImage(e.bmp, q.sx, q.sy, q.w, q.h, q.dx, q.dy, q.w, q.h);
}
// One widget of the play tree, from its value in the tree's state (while editing: the defaults).
function paintItem(g, it) {
  const pc = S.pc, st = pc.st, w = it.w, r = it.rect, tiles = n => imeta(n).tiles || 1;
  switch (w.type) {
    case 'button': case 'dropdown': {
      fillArt(g, r, w.fill?.colour);
      const pressed = st.press?.key === it.key;
      if (w.fill?.image) drawArt(g, w.fill.image, w.fill.tile ?? L.buttonTile(w, tiles(w.fill.image), L.buttonValue(pc, it), { hover: st.hover === it.key, pressed, disabled: !it.enabled }), r);
      const v = L.getPlain(st, it);
      const chosen = w.captionFromItem ? L.itemForValue(w.items, v) : null, caption = chosen ? chosen.short ?? chosen.label : undefined;
      const off = pressed && w.pressedTiles && Array.isArray(w.pressOffset) ? w.pressOffset : [0, 0];
      drawText(g, w.text, r, w.pad, caption ?? it.text, off[0] | 0, off[1] | 0);
      break;
    }
    case 'dial': case 'meter': {
      if (w.type === 'dial') drawFill(g, w.fill, r, 0);
      const norm = L.getNorm(st, it), e = w.image && getImage(ip(w.image));
      if (w.type === 'dial' && w.slider && e) {
        const m = imeta(w.image), t = L.tileRect(e.w, e.h, m.tiles, m.axis, 0), [x, y] = L.sliderPos(r, t[2], t[3], norm, w.vertical ?? true);
        drawArt(g, w.image, 0, [x, y, t[2], t[3]]);
      } else if (w.image) drawArt(g, w.image, L.valueTile(tiles(w.image), norm), r);
      break;
    }
    case 'number': drawFill(g, w.fill, r, 0); drawText(g, w.text, r, w.pad, L.numberText(w, it.p, L.getPlain(st, it), pc, it)); break;
    case 'plot': // no signal: a trace lies on the bottom row, bars draw nothing
      drawFill(g, w.fill, r, 0);
      if (!w.draw || w.draw === 'trace' || w.draw === 'bars_trace') fillCol(g, [r[0], r[1] + r[3] - 1, r[2], 1], L.cssColour(w.line) ? w.line : '#ffffffff');
      break;
    case 'list': paintList(g, it); break;
    case 'embed':
      if (it.child) paintNode(g, it.child);
      paintBar(g, w.scroll, it.bar);
      break;
    case 'custom':
      drawFill(g, w.fill, r, 0);
      if (w.kind === 'pad' || w.kind === 'morph_pad') { // the runtime's handle: an outlined square, red on pads, grey on the morph pad
        const hs = L.padHandle(r, L.densityOf(S.docs)), hx = r[0] + Math.round(L.getNorm(st, it, 0) * (r[2] - hs)), hy = r[1] + r[3] - hs - Math.round(L.getNorm(st, it, 1) * (r[3] - hs));
        g.strokeStyle = w.kind === 'pad' ? '#b33733' : '#8b8b8b'; g.lineWidth = 1; g.strokeRect(hx + 0.5, hy + 0.5, hs - 1, hs - 1);
      }
      if (w.kind === 'piano') paintPiano(g, it);
      if (w.kind === 'keyscale') paintKeyscale(g, it);
      break;
    default: // a runtime value (`source`) midi_in or modified picks the fill's tile
      drawFill(g, w.fill, r, w.source === 'midi_in' || w.source === 'modified' ? it.source | 0 : 0);
      if (w.type === 'plate' || w.type === 'textbox') drawText(g, w.text, r, w.pad, it.text);
  }
}
function paintBar(g, s, bar) {
  if (!bar || !s) return;
  if (s.track) drawArt(g, s.track, 0, bar.track, true);
  if (s.thumb) drawArt(g, s.thumb, 0, bar.thumb, true); else fillCol(g, bar.thumb, '#80808099');
}
// A list: gapFill behind the rows, each row in rowFill (selectFill when selected) with its cells in
// the row text style aligned per column, rows down to the bottom, then the scrollbar.
function paintList(g, it) {
  const w = it.w, lay = it.list, sel = S.pc.st.sel[it.key];
  drawFill(g, w.fill, it.rect, 0);
  if (w.header) drawText(g, w.header, [it.rect[0], it.rect[1], it.rect[2], lay.head || it.rect[3]], null, L.resolveVars(w.header.text, it.node.all));
  g.save();
  clipRect(g, lay.body);
  fillCol(g, lay.body, w.gapFill);
  for (const row of lay.rows) {
    const cells = it.rows[row.i]?.cells;
    fillCol(g, row.rect, cells && row.i === sel && w.selectFill ? w.selectFill : w.rowFill);
    if (cells) row.cells.forEach((c, j) => drawText(g, { ...w.row, align: c.align || w.row?.align, valign: w.valign || w.row?.valign }, c.rect, null, cells[j] ?? ''));
  }
  g.restore();
  paintBar(g, w.scroll, lay.bar);
}
// The runtime's piano: with a fill it draws its own keys, without one only the held key over the art beneath.
// A key scaling curve from its text data: a preview only, straight segments between the points
// (the runtime bends them by each slope) inside the rect inset by 6, 3, 4, 4, level over levelRange,
// with point handles unless it is a strip. Nothing without stored data.
function paintKeyscale(g, it) {
  const w = it.w, pts = L.keyscalePoints(S.pc.st.text[L.resolveVars(w.key, it.node.all)]), r = it.rect;
  if (pts.length < 2) return;
  const d = L.densityOf(S.docs), [lo, hi] = Array.isArray(w.levelRange) ? w.levelRange : [-80, 0];
  const P = [r[0] + L.dp(6, d), r[1] + L.dp(3, d), r[2] - L.dp(10, d), r[3] - L.dp(7, d)];
  const xy = ([n, l]) => [P[0] + n / 127 * (P[2] - 1), P[1] + (1 - L.clamp((l - lo) / ((hi - lo) || 1), 0, 1)) * (P[3] - 1)];
  g.strokeStyle = '#e8e8e8'; g.lineWidth = 1;
  g.beginPath();
  pts.forEach((p, i) => { const [x, y] = xy(p); i ? g.lineTo(x + 0.5, y + 0.5) : g.moveTo(x + 0.5, y + 0.5); });
  g.stroke();
  if (w.mode !== 'strip') for (const p of pts) { const [x, y] = xy(p); fillCol(g, [Math.round(x) - 2, Math.round(y) - 2, 5, 5], '#c80800ff'); }
}
const KEY_NAMES = { 0: 'c', 2: 'd', 4: 'e', 5: 'f', 7: 'g', 9: 'a', 11: 'b' };
function paintPiano(g, it) {
  const w = it.w, st = S.pc.st, own = !!(w.fill?.image || L.cssColour(w.fill?.colour)), held = new Set(st.notesIn);
  if (st.press?.key === it.key && st.press.note != null) held.add(st.press.note);
  const keys = L.pianoLayout(S.pc, it), tint = 'rgba(96,136,192,.5)', fill = (r, c) => { g.fillStyle = c; g.fillRect(...r); };
  const imgs = w.keyImages && typeof w.keyImages === 'object' ? w.keyImages : null, top = (w.first ?? 36) + (w.count ?? 61) - 1;
  if (imgs) { // key pictures (normal, pressed): every key draws its tile 0, or tile 1 while pressed
    for (const k of keys) {
      const name = k.black ? imgs.black : k.note === top && k.note % 12 === 0 && imgs.top ? imgs.top : imgs[KEY_NAMES[((k.note % 12) + 12) % 12]], e = name && getImage(ip(name));
      if (!e) continue;
      const t = L.tileRect(e.w, e.h, imeta(name).tiles, imeta(name).axis, 0);
      drawImage(g, name, held.has(k.note) ? 1 : 0, [k.rect[0], k.rect[1], t[2], t[3]]);
    }
    return;
  }
  for (const k of keys) {
    const lit = held.has(k.note), [x, y, kw, kh] = k.rect;
    if (k.black) { if (own || lit) fill(k.rect, !lit ? '#1f1f1f' : own ? '#5078b0' : tint); }
    else if (own) { fill(k.rect, lit ? '#a8c8f0' : '#ffffff'); fill([x + kw - 2, y, 2, kh], '#3c3c3c'); }
    else if (lit) { // tint the white key around the black keys beside it
      const top = k.rect.slice();
      for (const b of keys) if (b.black && b.rect[0] < top[0] + top[2] && b.rect[0] + b.rect[2] > top[0]) {
        top[3] = b.rect[3];
        if (b.rect[0] <= top[0]) { top[2] -= b.rect[0] + b.rect[2] - top[0]; top[0] = b.rect[0] + b.rect[2]; } else top[2] = b.rect[0] - top[0];
      }
      fill(top, tint); fill([x, y + top[3], kw, kh - top[3]], tint);
    }
  }
}
function paintNode(g, n) {
  g.save();
  clipRect(g, n.clip);
  drawFill(g, n.doc?.fill, [n.x, n.y, n.w, n.h], 0);
  for (const it of L.drawOrder(n.items, i => i.w)) {
    const off = !it.vis;
    if (!it.box || (off && (S.playing || !S.showHidden))) continue;
    g.save();
    if (off) g.globalAlpha *= 0.3; // the editor shows hidden widgets (and those whose showIf is false) faintly
    clipRect(g, it.box);
    const was = artOn;
    artOn = was && it.w.surface !== false;
    try { paintItem(g, it); } catch (err) { console.warn(it.key, err); } finally { artOn = was; }
    g.restore();
  }
  g.restore();
}

// ---- menus and tooltips (skin.json "menu" and "tooltip"; a plain style when the skin has none) -----------

const PLAIN_MENU = { native: true, fill: '#f0f0f0ff', border: '#808080ff', hoverFill: '#3875d7ff', separator: '#c8c8c8ff', pad: [6, 2, 12, 2] };
const PLAIN_TIP = { fill: '#ffffe1ff', border: '#000000ff', pad: [4, 2, 4, 2] };
// Metrics of a menu or tip font: the skin font once loaded, else the system font.
function fontInfo(name, check, arrow) {
  const e = name && getImage(fp(name)), wOf = (n, d) => { const c = n && getImage(ip(n)); return c ? L.tileRect(c.w, c.h, imeta(n).tiles, imeta(n).axis, 0)[2] + 2 : d; };
  const d = L.densityOf(S.docs), checkW = wOf(check, L.dp(8, d)), arrowW = wOf(arrow, L.dp(8, d));
  if (e?.font) return { e, measure: s => L.textWidth(e.font, s), lineH: e.font.h, checkW, arrowW };
  return { e: null, measure: s => { mx.font = SYS_FONT; return Math.ceil(mx.measureText(s).width); }, lineH: 13, checkW, arrowW };
}
const metricsOf = style => fontInfo(style?.font, style?.check, style?.arrow);
// Text in a menu or tip font, vertically centred in r shrunk by pad; the system font without one.
function styleText(g, font, text, r, pad, light) {
  const e = font && getImage(fp(font));
  if (e?.font) return drawText(g, { font, valign: 'middle' }, r, pad, text);
  const lines = String(text).split('\\n'), mid = r[1] + pad[1] + (r[3] - pad[1] - pad[3]) / 2;
  g.font = SYS_FONT; g.textBaseline = 'middle'; g.fillStyle = light ? '#ffffff' : '#000000';
  lines.forEach((l, i) => g.fillText(l, r[0] + pad[0], mid + (i - (lines.length - 1) / 2) * 13));
}
function drawBox(g, r, style) {
  fillCol(g, r, style.fill);
  if (style.border) for (const b of [[r[0], r[1], r[2], 1], [r[0], r[1] + r[3] - 1, r[2], 1], [r[0], r[1], 1, r[3]], [r[0] + r[2] - 1, r[1], 1, r[3]]]) fillCol(g, b, style.border);
}
// A menu layout (logic.mjs menuLayout) with item `hover` highlighted and checked(item) marked: the
// shadow, box, rows (hoverFill, then hoverBand behind the text line), separators (a line one pixel
// below the row's top), check marks and submenu arrows.
function drawMenu(g, lay, style, hover, f, checked) {
  if (style.shadow) fillCol(g, [lay.rect[0] + 1, lay.rect[1] + 1, lay.rect[2], lay.rect[3]], style.shadow);
  drawBox(g, lay.rect, style);
  const plain = !style.font;
  for (const row of lay.rows) {
    const r = row.rect, item = row.item;
    if (item.separator) { fillCol(g, [r[0], r[1] + 1, r[2], 1], style.separator || style.border); continue; }
    const on = row.i === hover, chk = style.check && getImage(ip(style.check));
    if (on) {
      fillCol(g, r, style.hoverFill);
      if (style.hoverBand) fillCol(g, [r[0], r[1] + lay.pad[1] + Math.trunc((r[3] - lay.pad[1] - lay.pad[3] - f.lineH) / 2), r[2], f.lineH], style.hoverBand);
    }
    if (Array.isArray(item.items)) { // a submenu: the arrow image, or a small triangle
      const a = style.arrow && getImage(ip(style.arrow)), ax = r[0] + r[2] - f.arrowW;
      if (a) { const t = L.tileRect(a.w, a.h, imeta(style.arrow).tiles, imeta(style.arrow).axis, 0); drawImage(g, style.arrow, 0, [ax, r[1] + ((r[3] - t[3]) >> 1), t[2], t[3]]); }
      else for (let k = 0; k < 3; k++) fillCol(g, [ax + 2 + k, r[1] + (r[3] >> 1) - 2 + k, 1, 5 - 2 * k], plain ? (on ? '#ffffffff' : '#000000ff') : style.border || '#ffffffff');
    }
    if (checked(item)) {
      const x = r[0] + lay.pad[0] - f.checkW;
      if (chk) {
        const t = L.tileRect(chk.w, chk.h, imeta(style.check).tiles, imeta(style.check).axis, 0);
        drawImage(g, style.check, 0, [x, r[1] + ((r[3] - t[3]) >> 1), t[2], t[3]]);
      } else fillCol(g, [x + 1, r[1] + (r[3] >> 1) - 2, 4, 4], plain ? (on ? '#ffffffff' : '#000000ff') : style.border || '#ffffffff'); // no check image: a small square
    }
    styleText(g, on && style.hoverFont ? style.hoverFont : style.font, String(row.item.label ?? ''), r, lay.pad, plain && on);
  }
}
function drawTip(g, text, at, style, win) {
  const f = fontInfo(style.font), lay = L.tipLayout(text, style, f.measure, f.lineH, at, win, L.densityOf(S.docs));
  drawBox(g, lay.rect, style);
  styleText(g, style.font, lay.text, lay.rect, lay.pad, false);
}
function playOverlays(g, pc) {
  const s = skin() || {}, st = pc.st, l = s.learn, style = L.menuStyleNow(pc);
  if (st.learn && l && typeof l === 'object') for (const it of L.treeItems(pc.root)) if (it.pid === st.learn && it.vis && it.box) drawBox(g, it.rect, { border: l.outline || '#ff000099' });
  if (style) { const f = metricsOf(style); L.menuLayouts(pc).forEach((lay, k) => drawMenu(g, lay, style, st.menu.levels[k].hover, f, item => L.menuChecked(pc, item))); }
  if (s.tooltip && L.tipVisible(pc)) drawTip(g, st.tip.text, [st.tip.x, st.tip.y], s.tooltip, [pc.root.w, pc.root.h]);
}

function sizeBase(w, h) {
  w = Math.max(1, w | 0); h = Math.max(1, h | 0);
  if (base.width !== w || base.height !== h) { base.width = w; base.height = h; layout(); } else bx.clearRect(0, 0, w, h);
  bx.imageSmoothingEnabled = false;
}
// The play context of the open view: Play mode's state, or the defaults while editing.
function playCtx() {
  return (S.pc = L.playCtx(S.docs, S.playing ? S.play : L.playState(S.docs), S.view, {
    pmap: S.pmap, now: performance.now(), metrics: metricsOf, nativeMenu: PLAIN_MENU, showAll: !S.playing && S.showHidden, data: S.data,
    tileSize: n => { const e = getImage(ip(n)); return e ? L.tileRect(e.w, e.h, imeta(n).tiles, imeta(n).axis, 0).slice(2) : null; },
  }));
}
function renderBase() {
  S.pmap = L.paramMap(S.docs);
  if (S.mode === 'image') return renderImageBase();
  if (S.mode === 'font') return renderFontBase();
  const pc = playCtx();
  S.items = new Map(pc.root.items.map(it => [it.w, it]));
  sizeBase(pc.root.w, pc.root.h);
  if (!cur()) return;
  paintNode(bx, pc.root);
  if (S.playing) playOverlays(bx, pc);
}
function renderImageBase() {
  const e = getImage(ip(S.image));
  S.cells = []; S.pvRect = null;
  if (!e) return sizeBase(1, 1);
  const m = imeta(S.image), n = Math.max(1, m.tiles | 0 || 1), t0 = L.tileRect(e.w, e.h, n, m.axis, 0), [, , tw, th] = t0;
  let W = e.w, H = e.h;
  if (S.explode) {
    const cols = Math.max(1, Math.ceil(Math.sqrt(n * th / Math.max(1, tw))));
    for (let i = 0; i < n; i++) S.cells.push([(i % cols) * (tw + 4), Math.floor(i / cols) * (th + 4), tw, th]);
    W = Math.min(n, cols) * (tw + 4) - 4; H = Math.ceil(n / cols) * (th + 4) - 4;
  } else for (let i = 0; i < n; i++) S.cells.push(L.tileRect(e.w, e.h, n, m.axis, i));
  const pv = m.slice ? S.pv || [tw * 2, th * 2] : null;
  if (pv) S.pvRect = [W + 16, 0, Math.max(1, pv[0] | 0), Math.max(1, pv[1] | 0)];
  sizeBase(pv ? W + 16 + S.pvRect[2] : W, Math.max(H, pv ? S.pvRect[3] : 0));
  if (S.explode) S.cells.forEach((c, i) => bx.drawImage(e.bmp, ...L.tileRect(e.w, e.h, n, m.axis, i), ...c));
  else bx.drawImage(e.bmp, 0, 0);
  if (pv) for (const [s, d] of L.imagePieces(t0, m.slice, S.pvRect)) bx.drawImage(e.bmp, ...s, ...d);
}
function fontSampleQuads(e) { return L.layoutText(e.font, S.sample.replace(/\r?\n/g, '\\n'), [0, e.h + 10, 1e6, 1e6], {}); }
function renderFontBase() {
  const e = getImage(fp(S.font));
  if (!e?.font) return sizeBase(1, 1);
  const quads = fontSampleQuads(e);
  sizeBase(Math.max(e.w, ...quads.map(q => q.dx + q.w)), Math.max(e.h + 10 + e.font.h, ...quads.map(q => q.dy + q.h)));
  bx.drawImage(e.bmp, 0, 0);
  for (const q of quads) bx.drawImage(e.bmp, q.sx, q.sy, q.w, q.h, q.dx, q.dy, q.w, q.h);
}

// ---- canvas: paint, overlay, rulers ---------------------------------------------------------------------

const cv = $('#cv'), cx = cv.getContext('2d'), sc = $('#scroller'), rx = $('#ruler-x'), ry = $('#ruler-y');
let raf = 0, need = 0;
function tick() {
  raf ||= requestAnimationFrame(() => { raf = 0; const n = need; need = 0; if (n & 2) renderBase(); paint(); });
}
function scheduleRender() { need |= 2; tick(); }
function schedulePaint() { need |= 1; tick(); }
const dpr = () => window.devicePixelRatio || 1;
function origin() { const d = dpr(); return { ox: Math.round((MARGIN - sc.scrollLeft) * d) / d, oy: Math.round((MARGIN - sc.scrollTop) * d) / d }; }
function toView(e) { const r = sc.getBoundingClientRect(), { ox, oy } = origin(); return { x: (e.clientX - r.left - ox) / S.zoom, y: (e.clientY - r.top - oy) / S.zoom }; }
function toScreen(x, y) { const { ox, oy } = origin(); return [ox + x * S.zoom, oy + y * S.zoom]; }
function layout() {
  $('#spacer').style.width = `${base.width * S.zoom + 2 * MARGIN}px`;
  $('#spacer').style.height = `${base.height * S.zoom + 2 * MARGIN}px`;
}
function fitCanvas(c) {
  const W = c.clientWidth, H = c.clientHeight, d = dpr();
  if (c.width !== Math.round(W * d) || c.height !== Math.round(H * d)) { c.width = Math.round(W * d); c.height = Math.round(H * d); }
  const g = c.getContext('2d');
  g.setTransform(d, 0, 0, d, 0, 0);
  g.imageSmoothingEnabled = false;
  return [g, W, H];
}
const checker = (() => {
  const c = h('canvas', { width: 16, height: 16 }), g = c.getContext('2d');
  g.fillStyle = '#2a2d33'; g.fillRect(0, 0, 16, 16);
  g.fillStyle = '#33363d'; g.fillRect(0, 0, 8, 8); g.fillRect(8, 8, 8, 8);
  return cx.createPattern(c, 'repeat');
})();
function paint() {
  const [g, W, H] = fitCanvas(cv), { ox, oy } = origin(), z = S.zoom;
  g.fillStyle = '#131519'; g.fillRect(0, 0, W, H);
  g.fillStyle = checker; g.fillRect(ox, oy, base.width * z, base.height * z);
  g.drawImage(base, ox, oy, base.width * z, base.height * z);
  overlay(g, W, H);
  rulers();
}
function handles(r) {
  const [x0, y0] = toScreen(r[0], r[1]), [x1, y1] = toScreen(r[0] + r[2], r[1] + r[3]), xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
  return [[x0, y0], [xm, y0], [x1, y0], [x1, ym], [x1, y1], [xm, y1], [x0, y1], [x0, ym]];
}
const HANDLE_EDGES = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w'];
const HANDLE_CURSORS = ['nwse-resize', 'ns-resize', 'nesw-resize', 'ew-resize', 'nwse-resize', 'ns-resize', 'nesw-resize', 'ew-resize'];
function overlay(g, W, H) {
  const z = S.zoom;
  const box = (r, col, dash) => {
    const [x, y] = toScreen(r[0], r[1]);
    g.save(); g.strokeStyle = col; g.lineWidth = 1; g.setLineDash(dash || []);
    g.strokeRect(Math.round(x) + 0.5, Math.round(y) + 0.5, Math.max(0, Math.round(r[2] * z) - 1), Math.max(0, Math.round(r[3] * z) - 1));
    g.restore();
  };
  const label = (text, x, y) => {
    g.font = '10px system-ui'; const w = g.measureText(text).width + 4;
    g.fillStyle = 'rgba(0,0,0,.7)'; g.fillRect(x, y, w, 13); g.fillStyle = '#fff'; g.fillText(text, x + 2, y + 10);
  };
  if (S.mode === 'view') {
    const v = cur();
    if (!v) return;
    box([0, 0, base.width, base.height], '#5a606b');
    if (S.playing) return;
    if (S.showBoxes) for (const w of v.widgets || []) if (validRect(rectOf(w))) box(rectOf(w), w.type === 'embed' ? 'rgba(123,216,143,.45)' : 'rgba(255,255,255,.2)');
    if (S.hover && !S.sel.includes(S.hover) && validRect(rectOf(S.hover))) box(rectOf(S.hover), 'rgba(255,255,255,.65)', [3, 3]);
    if (S.deep && validRect(S.deep.box)) box(S.deep.box, 'rgba(78,161,255,.95)', [2, 2]); // what a double-click opens
    for (const w of S.sel) if (validRect(rectOf(w))) box(rectOf(w), '#4ea1ff');
    if (S.sel.length === 1 && validRect(rectOf(S.sel[0]))) for (const [x, y] of handles(rectOf(S.sel[0]))) {
      g.fillStyle = '#4ea1ff'; g.fillRect(Math.round(x) - 3, Math.round(y) - 3, 7, 7);
      g.strokeStyle = '#0b0d10'; g.lineWidth = 1; g.strokeRect(Math.round(x) - 3.5, Math.round(y) - 3.5, 8, 8);
    }
    if (S.drag?.kind === 'marquee' && S.drag.rect) box(S.drag.rect, '#ffffff', [4, 3]);
    g.fillStyle = '#ff4fd8';
    for (const gd of S.guides) {
      if (gd.x != null) g.fillRect(Math.round(toScreen(gd.x, 0)[0]), 0, 1, H);
      else g.fillRect(0, Math.round(toScreen(0, gd.y)[1]), W, 1);
    }
  } else if (S.mode === 'image') {
    const m = imeta(S.image);
    S.cells.forEach((c, i) => {
      box(c, 'rgba(78,161,255,.6)');
      if (m.slice) {
        const [t, r, b, l] = m.slice;
        box([c[0] + l, c[1] + t, c[2] - l - r, c[3] - t - b], 'rgba(255,180,84,.8)', [3, 2]);
      }
      if (S.explode && c[2] * z >= 14 && c[3] * z >= 12) { const [x, y] = toScreen(c[0], c[1]); label(String(i), x + 1, y + 1); }
    });
    if (S.pvRect) {
      box(S.pvRect, '#7bd88f');
      const [x, y] = toScreen(S.pvRect[0], S.pvRect[1]);
      label('slice preview', x, y - 15);
    }
  } else if (S.mode === 'font') {
    const e = getImage(fp(S.font));
    if (!e?.font) return;
    e.font.glyphs.forEach(([gx, gw], i) => {
      if (!gw) return;
      const [x0, y0] = toScreen(gx, 0);
      g.fillStyle = i % 2 ? 'rgba(78,161,255,.10)' : 'rgba(255,255,255,.04)';
      g.fillRect(x0, y0, gw * z, e.h * z);
      g.fillStyle = 'rgba(78,161,255,.5)'; g.fillRect(Math.round(x0), y0, 1, e.h * z);
    });
    if (S.hoverGlyph != null) { const [gx, gw] = e.font.glyphs[S.hoverGlyph]; box([gx, 0, gw, e.h], '#ffb454'); }
  }
}
function rulers() {
  const { ox, oy } = origin(), z = S.zoom;
  const major = [10, 20, 50, 100, 200, 500, 1000, 2000].find(s => s * z >= 60) || 5000, minor = major / (major * z >= 150 ? 10 : 5);
  for (const [c, horiz] of [[rx, true], [ry, false]]) {
    const [g, W, H] = fitCanvas(c), o = horiz ? ox : oy, len = horiz ? W : H;
    g.fillStyle = '#1e2127'; g.fillRect(0, 0, W, H);
    g.fillStyle = '#6d737d'; g.font = '9px system-ui';
    for (let v = Math.floor(-o / z / minor) * minor; v <= (len - o) / z; v += minor) {
      const p = Math.round(o + v * z), big = v % major === 0, t = big ? 18 : 4;
      if (horiz) { g.fillRect(p, 18 - t, 1, t); if (big) g.fillText(String(v), p + 2, 9); }
      else { g.fillRect(18 - t, p, t, 1); if (big) { g.save(); g.translate(9, p + 2); g.rotate(Math.PI / 2); g.fillText(String(v), 0, 0); g.restore(); } }
    }
    if (S.mouse) {
      g.fillStyle = '#4ea1ff';
      const p = Math.round(o + (horiz ? S.mouse.x : S.mouse.y) * z);
      if (horiz) g.fillRect(p, 0, 1, 18); else g.fillRect(0, p, 18, 1);
    }
  }
}
function setZoom(z, anchor) {
  z = L.clamp(Math.round(z) || 1, 1, 6);
  $('#zoom').value = z;
  if (z === S.zoom) return;
  const a = anchor || { x: sc.clientWidth / 2, y: sc.clientHeight / 2 };
  const vx = (a.x + sc.scrollLeft - MARGIN) / S.zoom, vy = (a.y + sc.scrollTop - MARGIN) / S.zoom;
  S.zoom = z;
  layout();
  sc.scrollLeft = vx * z + MARGIN - a.x;
  sc.scrollTop = vy * z + MARGIN - a.y;
  schedulePaint();
}
function fit() {
  setZoom(Math.floor(Math.min((sc.clientWidth - 2 * MARGIN) / base.width, (sc.clientHeight - 2 * MARGIN) / base.height)));
}

// ---- canvas: mouse --------------------------------------------------------------------------------------

// Where a widget of the open view shows (after flow) and whether it is off (hidden, or showIf false).
const rectOf = w => S.items.get(w)?.rect || w.rect;
const isOff = w => { const it = S.items.get(w); return it ? it.hidden || !it.show : !!w.hidden; };
const selectable = w => validRect(rectOf(w)) && (S.showHidden || !isOff(w));
function hitAll(p) {
  const hits = L.drawOrder(cur()?.widgets || []).reverse().filter(w => selectable(w) && L.contains(rectOf(w), p.x, p.y));
  return [...hits.filter(w => !isOff(w)), ...hits.filter(isOff)]; // what shows wins a click; Alt+click reaches the hidden ones
}
function handleAt(e) {
  if (S.mode !== 'view' || S.playing || S.sel.length !== 1 || !validRect(rectOf(S.sel[0]))) return -1;
  const r = sc.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
  return handles(rectOf(S.sel[0])).findIndex(([hx, hy]) => Math.abs(x - hx) <= 5 && Math.abs(y - hy) <= 5);
}
function snapTargets() {
  const xs = [0, base.width / 2, base.width], ys = [0, base.height / 2, base.height];
  for (const w of cur().widgets || []) if (!S.sel.includes(w) && selectable(w)) {
    const [x, y, ww, hh] = rectOf(w);
    xs.push(x, x + ww / 2, x + ww); ys.push(y, y + hh / 2, y + hh);
  }
  return { xs, ys };
}
const bounds = rs => {
  const x = Math.min(...rs.map(r => r[0])), y = Math.min(...rs.map(r => r[1]));
  return [x, y, Math.max(...rs.map(r => r[0] + r[2])) - x, Math.max(...rs.map(r => r[1] + r[3])) - y];
};
function statusAt(p) {
  let s = `${Math.floor(p.x)}, ${Math.floor(p.y)}`;
  if (S.mode === 'view' && S.playing && S.play.hover) s += `  ${S.play.hover.slice(S.play.hover.lastIndexOf('/') + 1)}`;
  else if (S.mode === 'view' && S.deep) s += `  ${S.deep.w.name} (${S.deep.w.type}) in view ${S.deep.node.view}: double-click to edit it`;
  else if (S.mode === 'view' && S.hover) s += `  ${S.hover.name} (${S.hover.type})${typeof S.hover.text?.text === 'string' ? ': double-click to edit its text' : ''}`;
  if (S.mode === 'image') { const i = S.cells.findIndex(c => p.x >= c[0] && p.y >= c[1] && p.x < c[0] + c[2] && p.y < c[1] + c[3]); if (i >= 0) s += `  tile ${i}`; }
  if (S.mode === 'font' && S.hoverGlyph != null) {
    const e = getImage(fp(S.font)), [gx, gw] = e.font.glyphs[S.hoverGlyph];
    s += `  glyph ${S.hoverGlyph} '${String.fromCharCode(S.hoverGlyph)}' x ${gx} w ${gw}`;
  }
  $('#pos').textContent = s;
}
// ---- Play mode: the canvas works like the runtime (the rules are in logic.mjs) ----------------------------

let lastDown = { t: 0, x: 0, y: 0 }, repeat = 0, tipTimer = 0, statusTimer = 0, animTimer = 0;
const vpos = e => { const p = toView(e); return [Math.floor(p.x), Math.floor(p.y)]; };
// Runs a play handler on a fresh context, then does what the state asks for and redraws.
function playEvent(fn) {
  const pc = playCtx(), used = fn(pc), st = pc.st, tip = skin()?.tooltip;
  for (const ev of st.events.splice(0)) {
    if (ev.url) { msg(`Opening ${ev.url}`); window.open(ev.url, '_blank', 'noopener'); }
    else if (ev.midi) msg(`MIDI out: ${ev.midi}`);
    else if (ev.edit) openInline(pc, ev);
  }
  sc.title = !tip && st.tip ? st.tip.text : ''; // without a tooltip style the browser shows the tip, as native ones
  clearTimeout(tipTimer); clearTimeout(statusTimer);
  if (tip && st.tip && !L.tipVisible(pc)) tipTimer = setTimeout(scheduleRender, st.tip.at + pc.tipDelay - pc.now + 20);
  if (st.status && Number.isFinite(st.status.until) && st.status.until > pc.now) statusTimer = setTimeout(scheduleRender, st.status.until - pc.now + 20);
  if (st.press && !repeat) repeat = setInterval(() => {
    if (!S.playing || !S.play.press) { clearInterval(repeat); repeat = 0; return; }
    if (L.playTick(playCtx())) playEvent(() => {});
  }, 30);
  if (L.playAnimating(pc) && !animTimer) animTimer = setInterval(() => { // animated flows move at 30 Hz
    if (!S.playing || !L.playFrame(playCtx())) { clearInterval(animTimer); animTimer = 0; }
    scheduleRender();
  }, 33);
  scheduleRender();
  inspector();
  return used;
}
// An editable number or textbox edits in a plain input over the widget (not in the skin font).
function openInline(pc, ev) {
  closeInline();
  const it = L.findItem(pc.root, ev.edit), r = ev.rect || it?.rect;
  if (!it || !r) return;
  const el = h(ev.multiline ? 'textarea' : 'input', { className: 'inline-edit', spellcheck: false }), [x, y] = toScreen(r[0], r[1]);
  el.value = ev.text ?? '';
  Object.assign(el.style, { left: `${x}px`, top: `${y}px`, width: `${Math.max(40, r[2] * S.zoom)}px`, height: `${Math.max(18, r[3] * S.zoom)}px` });
  const fs = skin()?.fonts?.[(ev.cell ? it.w.row : it.w.text)?.font]; // the font's caret and selection colours
  if (L.cssColour(fs?.caret)) el.style.caretColor = L.cssColour(fs.caret);
  if (L.cssColour(fs?.selection)) el.style.setProperty('--sel', L.cssColour(fs.selection));
  const end = ok => {
    if (S.inline !== el) return;
    S.inline = null;
    el.remove();
    if (ok) playEvent(q => { if (!L.playCommit(q, ev.edit, el.value, ev.cell)) msg(`"${el.value}" is not a value`, true); });
  };
  el.addEventListener('keydown', e => {
    e.stopPropagation();
    if (e.key === 'Escape') { e.preventDefault(); end(false); }
    else if (e.key === 'Enter' && (!ev.multiline || e.ctrlKey)) { e.preventDefault(); end(true); }
  });
  el.addEventListener('blur', () => end(true));
  $('#viewport').append(el);
  S.inline = el;
  el.focus(); el.select();
}
function closeInline() { const el = S.inline; S.inline = null; el?.remove(); }
function setPlay(on) {
  on = !!on && S.mode === 'view' && !!cur();
  if (on === S.playing) return;
  S.playing = on;
  closeInline();
  Object.assign(S.play, { press: null, hover: null, menu: null, tip: null });
  S.hover = null; S.drag = null; S.guides = [];
  sc.title = '';
  document.body.classList.toggle('playing', on);
  $('#b-play').classList.toggle('on', on);
  msg(on ? 'Play mode: the widgets work as in the runtime; editing is off (P to stop)' : 'Editing');
  changed();
}
function resetPlay() { closeInline(); S.play = L.playState(S.docs); scheduleRender(); inspector(true); }

sc.addEventListener('contextmenu', e => { if (S.playing) e.preventDefault(); }); // Play mode: right-click opens the skin's context menus
sc.addEventListener('pointerdown', e => {
  if (e.button !== 0 && !(S.playing && e.button === 2)) return;
  const r = sc.getBoundingClientRect();
  if (e.clientX - r.left >= sc.clientWidth || e.clientY - r.top >= sc.clientHeight) return; // scrollbar
  document.activeElement?.blur?.();
  if (S.mode !== 'view' || !cur()) return;
  if (S.playing) {
    const [x, y] = vpos(e), now = performance.now(), right = e.button === 2;
    const dbl = !right && now - lastDown.t < 400 && Math.abs(x - lastDown.x) <= 4 && Math.abs(y - lastDown.y) <= 4;
    if (!right) lastDown = { t: dbl ? 0 : now, x, y };
    sc.setPointerCapture(e.pointerId);
    return playEvent(pc => L.playDown(pc, x, y, { dbl, shift: e.shiftKey, right }));
  }
  if (S.showParam) { S.showParam = false; inspector(); }
  sc.setPointerCapture(e.pointerId);
  const p = toView(e), hi = handleAt(e);
  if (hi >= 0) { S.drag = { kind: 'resize', h: hi, start: p, orig: S.sel[0].rect.slice(), T: snapTargets() }; return; }
  const marquee = () => { S.drag = { kind: 'marquee', start: p, add: e.shiftKey, prev: S.sel.slice() }; };
  if (e.ctrlKey || e.metaKey) return marquee();
  const hits = hitAll(p);
  if (!hits.length) { if (!e.shiftKey) S.sel = []; selChanged(); return marquee(); }
  let w = hits[0];
  if (e.altKey) { const i = S.sel.length === 1 ? hits.indexOf(S.sel[0]) : -1; w = hits[(i + 1) % hits.length]; S.sel = [w]; }
  else if (e.shiftKey) {
    if (S.sel.includes(w)) { S.sel = S.sel.filter(x => x !== w); return selChanged(); }
    S.sel = [...S.sel, w];
  } else if (!S.sel.includes(w)) S.sel = [w];
  selChanged();
  S.drag = { kind: 'move', start: p, sx: e.clientX, sy: e.clientY, orig: S.sel.map(x => x.rect.slice()), moved: false, T: snapTargets() };
});
sc.addEventListener('pointermove', e => {
  const p = toView(e), d = S.drag, thr = 6 / S.zoom;
  S.mouse = p;
  if (S.playing) {
    const [x, y] = vpos(e);
    playEvent(pc => L.playMove(pc, x, y, { shift: e.shiftKey }));
    return statusAt(p);
  }
  if (!d) {
    if (S.mode === 'view' && cur()) {
      S.hover = hitAll(p)[0] || null;
      const deep = deepAt(p);
      S.deep = deep && deep.node.view !== S.view ? deep : null;
      const hi = handleAt(e);
      sc.style.cursor = hi >= 0 ? HANDLE_CURSORS[hi] : 'default';
    } else if (S.mode === 'font') {
      const e2 = getImage(fp(S.font));
      const i = e2?.font && p.y >= 0 && p.y < e2.h ? e2.font.glyphs.findIndex(([gx, gw]) => p.x >= gx && p.x < gx + gw) : -1;
      S.hoverGlyph = i >= 0 ? i : null;
    }
    statusAt(p);
    return schedulePaint();
  }
  S.guides = [];
  if (d.kind === 'move') {
    if (!d.moved && Math.abs(e.clientX - d.sx) + Math.abs(e.clientY - d.sy) < 3) return;
    d.moved = true;
    let dx = Math.round(p.x - d.start.x), dy = Math.round(p.y - d.start.y);
    if (!e.altKey) {
      const b = bounds(d.orig.map(r => [r[0] + dx, r[1] + dy, r[2], r[3]]));
      const sx = L.snapDelta([b[0], b[0] + b[2] / 2, b[0] + b[2]], d.T.xs, thr), sy = L.snapDelta([b[1], b[1] + b[3] / 2, b[1] + b[3]], d.T.ys, thr);
      if (sx) { dx += Math.round(sx.d); S.guides.push({ x: sx.at }); }
      if (sy) { dy += Math.round(sy.d); S.guides.push({ y: sy.at }); }
    }
    S.sel.forEach((w, i) => { const o = d.orig[i]; w.rect = [o[0] + dx, o[1] + dy, o[2], o[3]]; });
  } else if (d.kind === 'resize') {
    const [x, y, w, hh] = d.orig, k = HANDLE_EDGES[d.h], dx = Math.round(p.x - d.start.x), dy = Math.round(p.y - d.start.y);
    let l = x, t = y, r = x + w, b = y + hh;
    const snap = (v, list, axis) => {
      if (e.altKey) return v;
      const s = L.snapDelta([v], list, thr);
      if (!s) return v;
      S.guides.push({ [axis]: s.at });
      return v + Math.round(s.d);
    };
    if (k.includes('w')) l = snap(l + dx, d.T.xs, 'x');
    if (k.includes('e')) r = snap(r + dx, d.T.xs, 'x');
    if (k.includes('n')) t = snap(t + dy, d.T.ys, 'y');
    if (k.includes('s')) b = snap(b + dy, d.T.ys, 'y');
    if (r - l < 1) { if (k.includes('w')) l = r - 1; else r = l + 1; }
    if (b - t < 1) { if (k.includes('n')) t = b - 1; else b = t + 1; }
    S.sel[0].rect = [l, t, r - l, b - t];
  } else if (d.kind === 'marquee') {
    const x0 = Math.min(d.start.x, p.x), y0 = Math.min(d.start.y, p.y), x1 = Math.max(d.start.x, p.x), y1 = Math.max(d.start.y, p.y);
    d.rect = [x0, y0, x1 - x0, y1 - y0];
    const inside = (cur().widgets || []).filter(w => { const r = rectOf(w); return selectable(w) && r[0] >= x0 && r[1] >= y0 && r[0] + r[2] <= x1 && r[1] + r[3] <= y1; });
    S.sel = d.add ? [...new Set([...d.prev, ...inside])] : inside;
    selInfo();
    return schedulePaint();
  }
  statusAt(p);
  scheduleRender();
  inspector();
});
function endDrag() {
  const d = S.drag;
  S.drag = null; S.guides = [];
  if (!d) return;
  if ((d.kind === 'move' && d.moved) || d.kind === 'resize') commit();
  else selChanged();
  schedulePaint();
}
function pointerUp(e) {
  if (!S.playing) return endDrag();
  const [x, y] = vpos(e);
  playEvent(pc => L.playUp(pc, x, y, { shift: e.shiftKey, right: e.button === 2 }));
}
sc.addEventListener('pointerup', pointerUp);
sc.addEventListener('pointercancel', pointerUp);
sc.addEventListener('pointerleave', () => {
  if (S.playing) { S.mouse = null; return playEvent(L.playLeave); }
  if (!S.drag) { S.hover = null; S.deep = null; S.mouse = null; S.hoverGlyph = null; schedulePaint(); }
});
sc.addEventListener('dblclick', e => { if (S.mode === 'view' && cur() && !S.playing) dblClick(toView(e)); });
sc.addEventListener('wheel', () => { if (!S.playing) S.inline?.blur(); }, { passive: true }); // scrolling away keeps a text being edited
sc.addEventListener('wheel', e => {
  const d = e.deltaY || e.deltaX; // Shift+wheel arrives as horizontal in some browsers
  if (S.playing && !e.ctrlKey && d) {
    const [x, y] = vpos(e), notches = -Math.sign(d) * Math.max(1, Math.round(Math.abs(d) / 100));
    if (playEvent(pc => L.playWheel(pc, x, y, notches, { shift: e.shiftKey }))) e.preventDefault();
    return;
  }
  if (!e.ctrlKey) return;
  e.preventDefault();
  const r = sc.getBoundingClientRect();
  setZoom(S.zoom + (e.deltaY < 0 ? 1 : -1), { x: e.clientX - r.left, y: e.clientY - r.top });
}, { passive: false });
sc.addEventListener('scroll', schedulePaint);
new ResizeObserver(schedulePaint).observe($('#viewport'));

// ---- selection and widget operations ---------------------------------------------------------------------

function selInfo() {
  const s = S.sel;
  $('#selinfo').textContent = s.length === 1 ? `${s[0].name} (${s[0].type}) [${s[0].rect}]` : s.length ? `${s.length} selected` : '';
}
function selChanged() {
  if (S.sel.length) S.showParam = false;
  selInfo();
  schedulePaint();
  inspector();
  datalists();
}
function openTarget(w) {
  const t = w.type === 'embed' ? w.view : w.action?.goto;
  if (t && vdoc(t)) openView(t); else if (t) msg(`No view "${t}"`, true);
}
function visibleCentre() {
  return [(sc.scrollLeft + sc.clientWidth / 2 - MARGIN) / S.zoom, (sc.scrollTop + sc.clientHeight / 2 - MARGIN) / S.zoom];
}
function mostUsed(pick) {
  const n = {};
  for (const v of views()) for (const w of vdoc(v).widgets || []) { const x = pick(w); if (x) n[x] = (n[x] || 0) + 1; }
  return Object.keys(n).sort((a, b) => n[b] - n[a])[0];
}
function defaults(type) {
  const font = mostUsed(w => w.text?.font) || fontNames()[0];
  const img = g => mostUsed(w => w.type === g && !w.slider && w.image) || imageNames().find(n => n.startsWith(g === 'dial' ? 'knobs/' : 'meters/'));
  const txt = (text, extra) => (font ? { text, font, align: 'center', valign: 'middle', ...extra } : undefined);
  return {
    plate: { rect: [0, 0, 80, 18], fill: { colour: '#3a3f47ff' }, text: txt('Label') },
    button: { rect: [0, 0, 60, 18], fill: { colour: '#4a505aff' }, text: txt('Button'), toggle: true },
    dropdown: { rect: [0, 0, 100, 18], fill: { colour: '#4a505aff' }, text: txt('One'), items: [{ label: 'One', value: 0 }, { label: 'Two', value: 1 }], captionFromItem: true },
    dial: { rect: [0, 0, 40, 40], image: img('dial') },
    number: { rect: [0, 0, 50, 16], text: font ? { font, align: 'right', valign: 'middle' } : undefined, range: [0, 100] },
    textbox: { rect: [0, 0, 120, 40], fill: { colour: '#202226ff' }, text: font ? { text: 'Text', font } : undefined, pad: [4, 4, 4, 4] },
    meter: { rect: [0, 0, 8, 60], image: img('meter') },
    plot: { rect: [0, 0, 160, 80], fill: { colour: '#101214ff' }, line: '#7bd88fff' },
    veil: { rect: [0, 0, 120, 80], fill: { colour: '#00000080' } },
    list: { rect: [0, 0, 160, 120], fill: { colour: '#181a1eff' }, header: txt('Name', { align: 'left' }), rowHeight: 16, row: font ? { font } : undefined },
    tree: { rect: [0, 0, 160, 120], fill: { colour: '#181a1eff' }, row: font ? { font } : undefined },
    embed: { rect: [0, 0, 200, 120], view: views().find(n => n !== S.view) },
    custom: { rect: [0, 0, 120, 120], kind: 'pad', fill: { colour: '#202226ff' } },
  }[type];
}
function addWidget(type) {
  if (S.playing) return;
  if (S.mode !== 'view' || !cur()) return msg('Open a view first', true);
  const v = cur(), ws = (v.widgets ||= []), { rect, ...rest } = defaults(type), [cxv, cyv] = visibleCentre();
  const x = L.clamp(Math.round(cxv - rect[2] / 2), 0, Math.max(0, (v.size?.[0] ?? 0) - rect[2]));
  const y = L.clamp(Math.round(cyv - rect[3] / 2), 0, Math.max(0, (v.size?.[1] ?? 0) - rect[3]));
  const w = clone({ type, name: L.uniqueName(`${type}_1`, new Set(ws.map(w => w.name))), rect: [x, y, rect[2], rect[3]], ...rest });
  ws.push(w);
  S.sel = [w];
  commit();
  selChanged();
}
function copies(items, offset) {
  const ws = (cur().widgets ||= []), taken = new Set(ws.map(w => w.name));
  return items.map(w => {
    const c = clone(w);
    c.name = L.uniqueName(c.name || c.type, taken);
    taken.add(c.name);
    if (offset && validRect(c.rect)) c.rect = [c.rect[0] + offset, c.rect[1] + offset, c.rect[2], c.rect[3]];
    return c;
  });
}
function duplicate() {
  if (!S.sel.length) return;
  const made = copies(S.sel, 8);
  cur().widgets.push(...made);
  S.sel = made;
  commit(); selChanged();
}
function copy() { if (S.sel.length) { S.clip = { view: S.view, items: S.sel.map(clone) }; msg(`Copied ${S.sel.length} widget(s)`); } }
function paste() {
  if (!S.clip || S.mode !== 'view' || !cur()) return;
  const made = copies(S.clip.items, S.clip.view === S.view ? 8 : 0);
  cur().widgets.push(...made);
  S.clip.items = made.map(clone); // paste again cascades
  S.sel = made;
  commit(); selChanged();
}
function del() {
  if (!S.sel.length) return;
  cur().widgets = cur().widgets.filter(w => !S.sel.includes(w));
  S.sel = [];
  commit(); selChanged();
}
function nudge(key, n, resize) {
  if (!S.sel.length) return;
  const dx = key === 'ArrowLeft' ? -n : key === 'ArrowRight' ? n : 0, dy = key === 'ArrowUp' ? -n : key === 'ArrowDown' ? n : 0;
  for (const w of S.sel) if (validRect(w.rect)) {
    const r = w.rect;
    w.rect = resize ? [r[0], r[1], Math.max(1, r[2] + dx), Math.max(1, r[3] + dy)] : [r[0] + dx, r[1] + dy, r[2], r[3]];
  }
  commit(`nudge:${S.view}:${S.sel.map(w => w.name)}`);
}
function layer(d) {
  for (const w of S.sel) w.layer = L.clamp((w.layer ?? 3) + d, 0, 7);
  commit(`layer:${S.view}:${S.sel.map(w => w.name)}`);
}
// Move each selected widget past its neighbour in the same layer (file order = draw order within a layer).
function reorder(dir) {
  const ws = cur().widgets || [];
  for (const i of S.sel.map(w => ws.indexOf(w)).sort((a, b) => dir * (b - a))) {
    const w = ws[i], lay = w.layer ?? 3;
    let j = i + dir;
    while (j >= 0 && j < ws.length && (ws[j].layer ?? 3) !== lay) j += dir;
    if (j < 0 || j >= ws.length || S.sel.includes(ws[j])) continue;
    ws.splice(i, 1);
    ws.splice(j, 0, w);
  }
  commit();
}
function alignSel(how) {
  const ws = S.sel.filter(w => validRect(w.rect));
  if (ws.length < 2) return;
  L.align(ws.map(w => w.rect), how).forEach((r, i) => { ws[i].rect = r; });
  commit();
}
function distributeSel(axis) {
  const ws = S.sel.filter(w => validRect(w.rect));
  if (ws.length < 3) return msg('Distribute needs three or more widgets', true);
  L.distribute(ws.map(w => w.rect), axis).forEach((r, i) => { ws[i].rect = r; });
  commit();
}
function renameWidget(t, v) {
  if (!v || v === t.name) return;
  if (!L.validName(v)) return msg('Names are lowercase snake_case', true);
  if (cur().widgets.some(x => x !== t && x.name === v)) return msg(`"${v}" is already used in this view`, true);
  // Toggle actions in the same view follow the rename; other views are checked by the Check tab.
  for (const w of cur().widgets) if (w.action?.toggle === t.name) w.action.toggle = v;
  t.name = v;
}

// ---- views and pages -------------------------------------------------------------------------------------

function crumb() {
  const el = $('#crumb');
  if (S.mode !== 'view' || !S.view) return void (el.textContent = S.mode === 'image' ? `image: ${S.image}` : S.mode === 'font' ? `font: ${S.font}` : '');
  const chain = L.viewChain(L.viewParents(S.docs), S.view), sig = chain.join('>');
  if (el.dataset.sig === sig) return;
  el.dataset.sig = sig;
  el.replaceChildren(...chain.flatMap((v, i) => [...(i ? [h('span', { className: 'dim' }, ' \u203A ')] : []),
    i < chain.length - 1 ? h('a', { href: '#', title: `Open ${v}`, onclick: e => { e.preventDefault(); openView(v); } }, v) : h('span', {}, v)]));
  el.title = chain.length > 1 ? `${S.view} shows inside ${chain.slice(0, -1).join(' \u203A ')}` : '';
}
function openView(name, push = true) {
  if (!name || !vdoc(name)) return;
  commit();
  if (name !== S.view) {
    if (push && S.view && vdoc(S.view)) S.back.push(S.view);
    S.sel = [];
    sc.scrollLeft = 0; sc.scrollTop = 0;
  }
  S.view = name; S.mode = 'view'; S.hover = null; S.showParam = false;
  if (S.play) { closeInline(); Object.assign(S.play, { press: null, hover: null, menu: null, tip: null }); }
  renderBase();
  changed();
}
function openImage(n) {
  setPlay(false);
  commit();
  S.mode = 'image'; S.image = n; S.explode = false; S.pv = null; S.showParam = false;
  sc.scrollLeft = 0; sc.scrollTop = 0;
  renderBase(); changed();
}
function openFont(n) {
  setPlay(false);
  commit();
  S.mode = 'font'; S.font = n; S.showParam = false;
  sc.scrollLeft = 0; sc.scrollTop = 0;
  renderBase(); changed();
}
function back() { const v = S.back.pop(); if (v && vdoc(v)) openView(v, false); else if (S.mode !== 'view') openView(S.view, false); }
async function newView() {
  const r = await ask('New view', [
    { key: 'name', label: 'Name', value: L.uniqueName('view_1', new Set(views())) },
    { key: 'w', label: 'Width', value: 400, type: 'number' }, { key: 'h', label: 'Height', value: 300, type: 'number' },
    { key: 'colour', label: 'Fill colour', value: '#202020ff' }, { key: 'image', label: 'or fill image', value: '', list: 'dl-images' },
  ]);
  if (!r) return;
  if (!L.validName(r.name) || vdoc(r.name)) return msg(`"${r.name}" is not a free snake_case name`, true);
  const fill = r.image ? { image: r.image } : L.cssColour(r.colour) ? { colour: r.colour } : undefined;
  S.docs[L.viewPath(r.name)] = clone({ size: [Math.max(1, r.w | 0), Math.max(1, r.h | 0)], fill, widgets: [] });
  commit();
  openView(r.name);
}
function renameViewTo(from, to) {
  if (!to || to === from) return;
  if (!L.validName(to)) return msg('View names are lowercase snake_case', true);
  if (vdoc(to)) return msg(`A view "${to}" exists`, true);
  S.docs[L.viewPath(to)] = S.docs[L.viewPath(from)];
  delete S.docs[L.viewPath(from)];
  const n = L.renameRefs(S.docs, 'view', from, to);
  S.back = S.back.map(v => (v === from ? to : v));
  if (S.view === from) S.view = to;
  commit();
  msg(`Renamed ${from} to ${to}; ${n} reference(s) updated`);
}
async function renameView() {
  const r = await ask(`Rename view ${S.view}`, [{ key: 'name', label: 'New name', value: S.view }]);
  if (r) renameViewTo(S.view, r.name);
}
function refsTo(kind, name) { return L.refs(S.docs).filter(r => r.kind === kind && r.name === name); }
function refuse(what, rs) { alert(`${what} is still referenced by:\n` + rs.slice(0, 30).map(where).join('\n') + (rs.length > 30 ? `\n... and ${rs.length - 30} more` : '')); }
function deleteView() {
  const n = S.view, rs = refsTo('view', n);
  if (rs.length) return refuse(`View "${n}"`, rs);
  if (!confirm(`Delete view ${n}?`)) return;
  delete S.docs[L.viewPath(n)];
  S.back = S.back.filter(v => v !== n);
  S.sel = [];
  S.view = vdoc(skin().root) ? skin().root : views()[0];
  commit();
  openView(S.view, false);
}
async function addPage() {
  const stacks = embedNames(), stack0 = stacks.includes('pages') ? 'pages' : stacks[0] || 'pages';
  const count = n => (vdoc(n).widgets || []).filter(w => w.action?.goto && w.action.stack === stack0).length;
  const target0 = views().reduce((a, n) => (count(n) > count(a) ? n : a), S.view);
  const r = await ask('Add page', [
    { key: 'name', label: 'Page view', value: L.uniqueName('page_new', new Set(views())) },
    { key: 'caption', label: 'Button caption', value: 'New' },
    { key: 'target', label: 'Button goes in', value: target0, options: views() },
    { key: 'stack', label: 'Stack (embed)', value: stack0, list: 'dl-stacks' },
    { key: 'colour', label: 'Page fill colour', value: '#202020ff' },
  ]);
  if (!r) return;
  if (!L.validName(r.name) || vdoc(r.name)) return msg(`"${r.name}" is not a free snake_case name`, true);
  const emb = L.findEmbed(S.docs, skin().root, r.stack);
  if (!emb || !validRect(emb.widget.rect)) return msg(`No embed named "${r.stack}"`, true);
  S.docs[L.viewPath(r.name)] = clone({ size: emb.widget.rect.slice(2), fill: L.cssColour(r.colour) ? { colour: r.colour } : undefined, widgets: [] });
  const tv = vdoc(r.target), ws = (tv.widgets ||= []);
  const navs = ws.filter(w => w.action?.goto && w.action.stack === r.stack && validRect(w.rect));
  const name = L.uniqueName('nav_' + r.name.replace(/^page_/, ''), new Set(ws.map(w => w.name)));
  let btn;
  if (navs.length) {
    // Copy the last navigation button in the file and put it below the lowest one of the same
    // size, with the smallest gap found between same-size buttons in one column.
    const last = navs[navs.length - 1], [, , w0, h0] = last.rect;
    const same = navs.filter(w => w.rect[2] === w0 && w.rect[3] === h0).sort((a, b) => a.rect[1] - b.rect[1] || a.rect[0] - b.rect[0]);
    const bottom = same.reduce((a, w) => (w.rect[1] > a.rect[1] ? w : a));
    const gaps = same.flatMap((w, i) => same.slice(i + 1).filter(o => o.rect[0] === w.rect[0]).map(o => o.rect[1] - w.rect[1] - h0)).filter(g => g >= 0);
    btn = clone(last);
    btn.name = name;
    btn.rect = [bottom.rect[0], bottom.rect[1] + h0 + (gaps.length ? Math.min(...gaps) : 2), w0, h0];
    btn.action = { goto: r.name, stack: r.stack };
    if (btn.text) btn.text.text = r.caption;
  } else {
    const font = fontNames()[0];
    btn = clone({ type: 'button', name, rect: [0, 0, 80, 18], fill: { colour: '#4a505aff' }, text: font ? { text: r.caption, font, align: 'center', valign: 'middle' } : undefined, action: { goto: r.name, stack: r.stack } });
  }
  ws.push(btn);
  commit();
  openView(r.target);
  S.sel = [btn];
  selChanged();
  msg(`Added view ${r.name} and button ${name} in ${r.target}`);
}
function embedNames() {
  return [...new Set(views().flatMap(n => (vdoc(n).widgets || []).filter(w => w.type === 'embed' && w.name).map(w => w.name)))].sort();
}

// ---- params ------------------------------------------------------------------------------------------------

const validParamId = s => /^[a-z0-9_]+(\.[a-z0-9_]+)*$/.test(s);
function paramUses(id) { return L.refs(S.docs).filter(r => r.kind === 'param' && L.paramMatcher(r.name)(id)); }
async function addParam() {
  const r = await ask('Add param', [{ key: 'id', label: 'Id', value: '' }, { key: 'name', label: 'Name', value: '' }]);
  if (!r) return;
  if (!validParamId(r.id)) return msg('Param ids are dotted snake_case, e.g. arp.tempo', true);
  if (params().some(p => p.id === r.id)) return msg(`"${r.id}" exists`, true);
  (S.docs['params.json'] ||= []).push({ id: r.id, name: r.name || r.id, min: 0, max: 1, default: 0 });
  S.param = r.id; S.showParam = true;
  commit();
}
function renameParam(to) {
  const p = params().find(q => q.id === S.param);
  if (!p || !to || to === p.id) return;
  if (!validParamId(to)) return msg('Param ids are dotted snake_case', true);
  if (params().some(q => q.id === to)) return msg(`"${to}" exists`, true);
  const n = L.renameRefs(S.docs, 'param', p.id, to);
  p.id = to; S.param = to;
  msg(`Renamed; ${n} widget binding(s) updated (templates such as op.{op}.x are not rewritten)`);
}
function deleteParam() {
  const uses = paramUses(S.param);
  if (!confirm(uses.length ? `${S.param} is used by ${uses.length} widget(s). Delete it anyway?` : `Delete ${S.param}?`)) return;
  S.docs['params.json'] = params().filter(p => p.id !== S.param);
  S.showParam = false;
  commit();
}

// ---- images, fonts, tiles ----------------------------------------------------------------------------------

function pickFiles(multiple) {
  const inp = $('#file');
  inp.multiple = multiple; inp.accept = 'image/png,image/*'; inp.value = '';
  return new Promise(res => { inp.onchange = () => res([...inp.files]); inp.oncancel = () => res([]); inp.click(); });
}
async function importImages(files) {
  let last = null;
  for (const f of files) {
    const r = await ask(`Import ${f.name}`, [
      { key: 'kind', label: 'As', value: 'image', options: ['image', 'font'] },
      { key: 'group', label: 'Group (images)', value: groupOf(S.image) || 'misc', list: 'dl-groups' },
      { key: 'name', label: 'Name', value: L.snake(f.name.replace(/\.[^.]*$/, '')) },
    ], 'Import');
    if (!r) continue;
    const font = r.kind === 'font', name = font ? r.name : `${r.group}/${r.name}`, path = font ? fp(name) : ip(name);
    if (!(font ? L.validName(name) : L.validPath(name))) { msg(`"${name}" is not a valid snake_case name`, true); continue; }
    if (exists(path) && !confirm(`${path} exists. Replace it?`)) continue;
    setBin(path, await pngBlob(await decodeAny(new Uint8Array(await f.arrayBuffer()))));
    last = [font, name];
  }
  if (!last) return;
  commit();
  switchTab('images');
  last[0] ? openFont(last[1]) : openImage(last[1]);
}
async function download(path) {
  const a = h('a', { href: URL.createObjectURL(await blobOf(path)), download: path.split('/').pop() });
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 2000);
}
function setMeta(key, v, def) {
  const s = skin(), m = (s.images ||= {})[S.image] || {};
  L.setPath(m, key, v, def);
  if (Object.keys(m).length) s.images[S.image] = m; else delete s.images[S.image];
}
async function renameFile(kind) {
  const font = kind === 'font', from = font ? S.font : S.image, path = font ? fp : ip;
  const r = await ask(`Rename ${kind} ${from}`, [{ key: 'name', label: font ? 'Name' : 'group/name', value: from }]);
  if (!r || r.name === from) return;
  if (!(font ? L.validName(r.name) : L.validPath(r.name))) return msg(`"${r.name}" is not a valid snake_case name`, true);
  if (exists(path(r.name))) return msg(`${path(r.name)} exists`, true);
  setBin(path(r.name), await blobOf(path(from)));
  dropBin(path(from));
  const n = L.renameRefs(S.docs, kind, from, r.name);
  if (font) S.font = r.name; else S.image = r.name;
  commit();
  msg(`Renamed to ${r.name}; ${n} reference(s) updated`);
}
function deleteFile(kind) {
  const font = kind === 'font', n = font ? S.font : S.image, rs = refsTo(kind, n).filter(r => !r.meta);
  if (rs.length) return refuse(`${font ? 'Font' : 'Image'} "${n}"`, rs);
  if (!confirm(`Delete ${kind} ${n}?`)) return;
  dropBin(font ? fp(n) : ip(n));
  if (!font && skin().images) delete skin().images[n];
  commit();
  openView(S.view, false);
}
function useImage(n) {
  if (!S.sel.length) return msg('Select widgets in a view first', true);
  for (const w of S.sel) if (w.type === 'dial' || w.type === 'meter') w.image = n; else L.setPath(w, 'fill.image', n);
  commit();
  msg(`${n} set on ${S.sel.length} widget(s)`);
}
const byName = (a, b) => a.localeCompare(b, undefined, { numeric: true });
async function workFiles(prefix) {
  return (await api.list('work')).map(f => f.path).filter(p => p.startsWith(prefix) && p.endsWith('.png') && !p.slice(prefix.length).includes('/')).sort(byName);
}
async function clearWork(prefix) { await task('Removing old files', await workFiles(prefix), p => api.del('work', p)); }
const decodeFiles = files => task('Reading files', files, async f => decodeAny(new Uint8Array(await f.arrayBuffer())));
const decodeWork = paths => task('Reading files', paths, async p => decodeAny(await bytesOf(await api.get('work', p))));
async function exportTiles() {
  const n = S.image, e = await ensureImage(ip(n)), m = imeta(n), tiles = L.splitTiles(e.rgba, m.tiles || 1, m.axis || 'y');
  await clearWork(`${n}/`);
  const digits = Math.max(3, String(tiles.length - 1).length);
  await task('Exporting tiles', tiles, async (t, i) => api.put('work', `${n}/${String(i).padStart(digits, '0')}.png`, await pngBlob(t)));
  msg(`Exported ${tiles.length} tiles to tiles/${n}/`);
}
async function importTiles(files) {
  const n = S.image, m = imeta(n);
  let imgs;
  if (files) { if (!files.length) return; imgs = await decodeFiles([...files].sort((a, b) => byName(a.name, b.name))); }
  else {
    const ps = await workFiles(`${n}/`);
    if (!ps.length) return msg(`No PNG files in tiles/${n}/`, true);
    imgs = await decodeWork(ps);
  }
  const strip = L.joinTiles(imgs, m.axis || 'y');
  setBin(ip(n), await pngBlob(strip));
  setMeta('tiles', imgs.length, 1);
  commit();
  await save();
  msg(`Rebuilt ${n} from ${imgs.length} tiles and saved`);
}
async function exportGlyphs() {
  const n = S.font, e = await ensureImage(fp(n)), dir = `fonts/${n}/`;
  await clearWork(dir);
  const list = [...L.fontGlyphs(e.rgba, e.font).entries()].filter(([, g]) => g.width && !L.isEmpty(g));
  await task('Exporting glyphs', list, async ([code, g]) => api.put('work', `${dir}${code}.png`, await pngBlob(g)));
  msg(`Exported ${list.length} glyphs to tiles/${dir}`);
}
async function importGlyphs(files) {
  const n = S.font, e = await ensureImage(fp(n));
  let list;
  if (files) { if (!files.length) return; list = (await decodeFiles(files)).map((g, i) => [parseInt(files[i].name, 10), g]); }
  else { const ps = await workFiles(`fonts/${n}/`); list = (await decodeWork(ps)).map((g, i) => [parseInt(ps[i].split('/').pop(), 10), g]); }
  list = list.filter(([c]) => c >= 0 && c <= 255);
  if (!list.length) return msg('No glyph files named <code>.png (0..255)', true);
  const hgt = list[0][1].height;
  if (list.some(([, g]) => g.height !== hgt)) return msg('Glyph files differ in height', true);
  // Glyphs without a file keep their current pixels and width (spaces, for example).
  const glyphs = L.fontGlyphs(e.rgba, e.font).map(g => L.fitGlyph(g, hgt));
  for (const [c, g] of list) glyphs[c] = g;
  setBin(fp(n), await pngBlob(L.buildFont(glyphs)));
  commit();
  await save();
  msg(`Rebuilt ${n} from ${list.length} glyph files and saved`);
}

// ---- dialog --------------------------------------------------------------------------------------------------

function ask(title, fields, okLabel = 'OK') {
  const dlg = $('#dlg'), inputs = {};
  const rows = fields.map(f => {
    const el = f.options ? h('select', {}, f.options.map(o => h('option', { value: o }, o))) : h('input', { type: f.type || 'text' });
    if (f.list) el.setAttribute('list', f.list);
    el.value = f.value ?? '';
    inputs[f.key] = el;
    return h('div', { className: 'f' }, h('label', {}, f.label), el);
  });
  dlg.replaceChildren(h('form', { method: 'dialog' }, h('h3', {}, title), rows,
    h('div', { className: 'btns' }, h('button', { type: 'button', onclick: () => dlg.close('') }, 'Cancel'), h('button', { value: 'ok' }, okLabel))));
  return new Promise(res => {
    dlg.onclose = () => res(dlg.returnValue === 'ok' ? Object.fromEntries(Object.entries(inputs).map(([k, el]) => [k, el.type === 'number' ? +el.value : el.value.trim()])) : null);
    dlg.returnValue = '';
    dlg.showModal();
    const first = dlg.querySelector('input, select');
    first?.focus(); first?.select?.();
  });
}

// ---- inspector ---------------------------------------------------------------------------------------------------

let inspSig = '', updaters = [];
const keep = el => document.activeElement === el; // never overwrite the field being typed in
function upd(fn) { updaters.push(fn); fn(); }
function live() { if (menuEd) return menuEd.live(); scheduleRender(); }
function done() { if (menuEd) return menuEd.changed(); commit(); scheduleRender(); inspector(); }
function row(label, ctl, title) { return h('div', { className: 'f', title }, h('label', {}, label), h('div', { className: 'ctl' }, ctl)); }
const note = t => h('div', { className: 'note' }, t);
const btns = (...list) => h('div', { className: 'btns' }, list.filter(Boolean).map(([label, fn, title, cls]) => h('button', { onclick: run(fn), title, className: cls || '' }, label)));

function textCtl(get, set, opt = {}) {
  const el = h('input', { type: 'text', placeholder: opt.placeholder || '', spellcheck: false });
  if (opt.list) el.setAttribute('list', opt.list);
  el.addEventListener('change', () => { set(opt.parse ? opt.parse(el.value) : el.value === '' ? undefined : el.value); done(); });
  upd(() => { if (!keep(el)) el.value = opt.fmt ? opt.fmt(get()) : get() ?? ''; });
  return el;
}
function numCtl(get, set, opt = {}) {
  const el = h('input', { type: 'number', step: opt.int ? 1 : 'any', placeholder: opt.def ?? '' });
  el.addEventListener('input', () => { if (el.value !== '' && Number.isFinite(+el.value)) { set(+el.value); live(); } });
  el.addEventListener('change', () => { set(el.value === '' ? undefined : +el.value); done(); });
  upd(() => { if (!keep(el)) el.value = get() ?? ''; });
  return el;
}
function boolCtl(get, set, def = false) {
  const el = h('input', { type: 'checkbox' });
  el.addEventListener('change', () => { set(el.checked === def ? undefined : el.checked); done(); });
  upd(() => { el.checked = get() ?? def; });
  return el;
}
function enumCtl(get, set, options, def, noBlank) {
  const el = h('select', {}, noBlank ? null : h('option', { value: '' }, def ? `(${def})` : '(none)'), options.map(o => h('option', { value: o }, o)));
  el.addEventListener('change', () => { set(el.value || undefined); done(); });
  upd(() => {
    const v = get() ?? '';
    if (v && ![...el.options].some(o => o.value === v)) el.append(h('option', { value: v }, v));
    el.value = v;
  });
  return el;
}
function numsCtl(get, set, labels) {
  const els = labels.map(l => h('input', { type: 'number', step: 1, title: l, placeholder: l }));
  const ok = () => els.every(e => e.value !== '' && Number.isFinite(+e.value));
  for (const el of els) {
    el.addEventListener('input', () => { if (ok()) { set(els.map(e => +e.value)); live(); } });
    el.addEventListener('change', () => { if (ok()) set(els.map(e => +e.value)); else if (els.every(e => e.value === '')) set(undefined); done(); });
  }
  upd(() => { const v = get(); els.forEach((e, i) => { if (!keep(e)) e.value = Array.isArray(v) ? v[i] ?? '' : ''; }); });
  return els;
}
function colourCtl(get, set) {
  const pick = h('input', { type: 'color', title: 'Pick a colour (alpha kept from the text)' }), txt = h('input', { type: 'text', placeholder: '#rrggbbaa', spellcheck: false });
  pick.addEventListener('input', () => { set(pick.value + ((get() || '').slice(7, 9) || 'ff')); txt.value = get(); live(); });
  pick.addEventListener('change', done);
  txt.addEventListener('change', () => {
    const v = txt.value.trim();
    if (v && !L.cssColour(v)) return msg('Colours are #rrggbb or #rrggbbaa', true);
    set(v || undefined); done();
  });
  upd(() => { const v = get() || ''; if (!keep(txt)) txt.value = v; pick.value = /^#[0-9a-f]{6}/i.test(v) ? v.slice(0, 7).toLowerCase() : '#000000'; });
  return [pick, txt];
}
function areaCtl(get, set, fmt, parse, rows = 5) {
  const el = h('textarea', { rows, spellcheck: false });
  el.addEventListener('change', () => { set(parse(el.value)); done(); });
  upd(() => { if (!keep(el)) el.value = fmt(get()); });
  return el;
}
const jump = (title, fn) => h('button', { title, onclick: fn }, '→');

// One inspector row from a spec [path, kind, opt] on the object target() returns.
function field(target, [path, kind, opt = {}]) {
  const get = () => L.getPath(target() || {}, path), set = v => { const t = target(); if (t) L.setPath(t, path, v, opt.def); };
  const label = opt.label || path.split('.').pop();
  switch (kind) {
    case 'str': return row(label, textCtl(get, set, opt), opt.title);
    case 'int': case 'num': return row(label, numCtl(get, set, { int: kind === 'int', def: opt.def ?? opt.placeholder }), opt.title);
    case 'bool': return row(label, boolCtl(get, set, opt.def ?? false), opt.title);
    case 'enum': return row(label, enumCtl(get, set, opt.options, opt.def), opt.title);
    case 'nums': return row(label, numsCtl(get, set, opt.labels));
    case 'colour': return row(label, colourCtl(get, set));
    case 'image': return row(label, [textCtl(get, set, { list: 'dl-images' }), jump('Show in Images', () => get() && exists(ip(get())) && (switchTab('images'), openImage(get())))]);
    case 'font': return row(label, [textCtl(get, set, { list: 'dl-fonts' }), jump('Show in Images', () => get() && exists(fp(get())) && (switchTab('images'), openFont(get())))]);
    case 'view': return row(label, [textCtl(get, set, { list: 'dl-views' }), jump('Open this view', () => get() && openView(get()))]);
    case 'vars': return row(label, textCtl(get, set, { fmt: L.formatVars, parse: L.parseVars, placeholder: opt.placeholder || 'name=value, ...' }));
    case 'strlist': return row(label, textCtl(get, set, { fmt: v => (v || []).join(', '), parse: t => t.split(',').map(s => s.trim()).filter(Boolean), list: opt.list }), opt.title);
    case 'numlist': return row(label, textCtl(get, set, { fmt: v => (v || []).join(', '), parse: t => t.split(/[\s,]+/).filter(Boolean).map(Number).filter(Number.isFinite), placeholder: opt.placeholder }), opt.title);
    case 'menu': return row(label, menuCtl(target, path), opt.title);
    case 'columns': return row(label, areaCtl(get, set, L.formatColumns, t => L.parseColumns(t, get()), 3), 'One column per line: width, then left, center or right, then int for an editable number cell');
    case 'rows': return row(label, areaCtl(get, set, L.formatRows, L.parseRows, 3), 'One row per line, cells separated by |');
    case 'fixed': return row(label, areaCtl(get, set, L.formatFixed, L.parseFixed, 3), 'One per line: a param id (its CC, or -1), or "label | internal param" (a row whose CC cell is that param)');
    case 'reset': return row(label, enumCtl(() => (get() === false ? 'off' : get()), v => set(v === 'off' ? false : v), ['default', 'zero', 'off'], 'default'), 'What a double-click resets to');
    case 'rawstr': return row(label, textCtl(get, v => { const t = target(); if (!t) return; if (v === undefined) delete t[path]; else t[path] = v; },
      { parse: v => (v === '' ? undefined : v === '""' ? '' : v), fmt: v => (v === '' ? '""' : v ?? ''), placeholder: opt.placeholder }), opt.title);
    case 'param': {
      const hint = h('div', { className: 'hint' });
      upd(() => {
        const id = get(), p = id && S.pmap.get(id);
        const n = id && id.includes('{') ? params().filter(q => L.paramMatcher(id)(q.id)).length : 0;
        hint.textContent = !id ? '' : p ? `${p.name}: ${p.min ?? 0} .. ${p.max ?? 1}${p.steps ? `, ${p.steps} steps` : ''}${p.taper === 'log' ? ', log' : ''}` : n ? `template, matches ${n} params` : 'no such param';
      });
      return [row(label, [textCtl(get, set, { list: 'dl-params' }), jump('Show in Params', () => { const id = get(); if (S.pmap.has(id)) { S.param = id; S.showParam = true; switchTab('params'); inspector(); } })]), hint];
    }
  }
  throw new Error(`unknown field kind ${kind}`);
}

const TEXT = (k, title, withText = true) => [title, [
  ...(withText ? [[`${k}.text`, 'str', { label: 'text' }]] : []),
  [`${k}.font`, 'font', { label: 'font' }],
  [`${k}.align`, 'enum', { label: 'align', options: ['left', 'center', 'right'], def: 'left' }],
  [`${k}.valign`, 'enum', { label: 'valign', options: ['top', 'middle', 'bottom'], def: 'top' }],
]];
const FILL = ['Fill', [['fill.colour', 'colour', { label: 'colour' }], ['fill.image', 'image', { label: 'image' }], ['fill.tile', 'int', { label: 'tile' }]]];
const PAD = ['pad', 'nums', { labels: ['left', 'top', 'right', 'bottom'], def: [0, 0, 0, 0] }];
const RANGE = ['range', 'nums', { labels: ['min', 'max'] }];
const MENU_AT = ['menuAt', 'nums', { labels: ['x', 'y'], title: 'The menu opens with its top-left at this point of the view' }];
const STATUS = ['status', 'bool', { title: 'Shows "Name: value" of the param under the pointer' }];
const SOURCE = ['Runtime value', [['source', 'enum', { options: ['cpu', 'midi_in', 'modified', 'voices', 'scale'], title: 'A value the runtime knows (the editor shows 0, or what Play mode knows)' }],
  ['format', 'str', { placeholder: '%.0f', when: w => w.source === 'cpu' || w.source === 'voices' }]]];
const SCROLL = list => ['Scroll', [['scroll.track', 'image', { label: 'track' }], ['scroll.thumb', 'image', { label: 'thumb' }], ['scroll.width', 'int', { label: 'width', def: 12 }],
  ['scroll.always', 'bool', { label: 'always' }], ...(list ? [] : [['scroll.follow', 'bool', { label: 'follow', title: 'Keeps a widget growing in an animated flow in view' }],
    ['scroll.reveal', 'bool', { label: 'reveal', title: 'Scrolls the radio button inside that is on to the middle after a goto, set or toggle' }]])]];
const BUTTON = ['Button', [['toggle', 'bool'], ['press', 'enum', { options: ['down', 'momentary', 'repeat', 'up'], def: 'down' }],
  ['hoverTiles', 'bool'], ['pressedTiles', 'bool'], ['disabledTile', 'bool'], ['pressOffset', 'nums', { labels: ['dx', 'dy'] }]]];
const SPECS = {
  plate: [FILL, TEXT('text', 'Text'), ['Layout', [PAD, STATUS]], SOURCE],
  button: [FILL, TEXT('text', 'Text'), ['Layout', [PAD]], BUTTON, 'action', ['Runtime value', [SOURCE[1][0]]]],
  dropdown: [FILL, TEXT('text', 'Text'), ['Layout', [PAD]], BUTTON, ['Items', [['items', 'menu'], ['captionFromItem', 'bool'], MENU_AT]], 'action'],
  dial: [['Dial', [['image', 'image'], ['slider', 'bool'], ['drag', 'enum', { options: ['linear', 'absolute', 'rotary'], def: 'linear' }], ['vertical', 'bool', { def: true }],
    ['sensitivity', 'num', { def: 1 }], ['fine', 'num', { def: 0.1 }], ['margin', 'int'], RANGE, ['steps', 'int'], ['default', 'num'],
    ['reset', 'reset', { def: 'default' }], ['wheel', 'num', { placeholder: '0.05 of the range', title: 'Fraction of the range per wheel notch (0: off)' }]]],
  ['MIDI', [['midi', 'str', { list: 'dl-midi', placeholder: 'bend or cc<n>' }], ['spring', 'num', { placeholder: 'normalized, e.g. 0.5' }]]], FILL],
  number: [FILL, TEXT('text', 'Text style', false), ['Number', [PAD, ['format', 'str', { placeholder: "param's format" }], RANGE, ['sensitivity', 'num'], ['editable', 'bool'],
    ['zeroText', 'rawstr', { placeholder: 'text at 0; "" for none', title: 'Shown instead of the value while it is exactly 0; type "" for an empty text' }],
    ['reset', 'reset', { def: 'default' }], ['wheel', 'num', { placeholder: 'off', title: 'Fraction of the range per wheel notch (0 or empty: off)' }],
    ['sensitivityZones', 'numlist', { label: 'zones', placeholder: 'e.g. 1, 0.1, 0.01', title: 'Value units per pixel for drags started in each of these equal columns, left to right' }],
    ['drag', 'bool', { def: true, title: 'Off: the box does not drag' }]]]],
  textbox: [FILL, TEXT('text', 'Text'), ['Layout', [PAD, ['multiline', 'bool'], STATUS]],
    ['Text data', [['key', 'str', { placeholder: 'data key, may use {var}' }], ['editable', 'bool'], ['editOn', 'enum', { options: ['click'], def: 'double' }]]], SOURCE],
  meter: [['Meter', [['image', 'image'], RANGE]]],
  plot: [FILL, ['Plot', [['line', 'colour'], ['line2', 'colour'], ['draw', 'enum', { options: ['trace', 'bars', 'bars_trace', 'centre_bars'], def: 'trace' }]]]],
  veil: [FILL],
  list: [FILL, TEXT('header', 'Header'), ['Rows', [['rowHeight', 'int', { placeholder: '16' }], ['rowGap', 'int'], ['colGap', 'int'], ['columns', 'columns'], ['rows', 'rows'],
    ['source', 'enum', { options: ['midi_map'], title: 'midi_map: the MIDI learn assignments, then the fixed rows, then the rows above' }],
    ['fixed', 'fixed', { when: w => w.source === 'midi_map' }], ['valign', 'enum', { options: ['top', 'middle', 'bottom'], def: 'top' }],
    ['gapFill', 'colour'], ['rowFill', 'colour'], ['selectFill', 'colour']]], TEXT('row', 'Row style', false), SCROLL(true)],
  tree: [FILL, TEXT('row', 'Row style', false)],
  embed: [['Embed', [['view', 'view'], ['vars', 'vars'], ['fit', 'bool', { title: 'Takes the size of the view it shows' }]]], SCROLL(false)],
  custom: [['Custom', [['kind', 'str', { list: 'dl-kinds' }], ['params', 'strlist', { list: 'dl-params' }],
    ['first', 'int', { label: 'first note', when: w => w.kind === 'piano' }], ['count', 'int', { when: w => w.kind === 'piano' }],
    ['velocity', 'int', { placeholder: 'from the height', when: w => w.kind === 'piano' }], ['glide', 'bool', { def: true, when: w => w.kind === 'piano' }],
    ['keyImages', 'vars', { label: 'key images', when: w => w.kind === 'piano', placeholder: 'c=keyboard/c, black=..., top=...' }]]], FILL],
};
const ACTIONS = ['goto', 'set', 'toggle', 'url', 'step', 'sequence', 'presets', 'data', 'midi_map', 'scale'];
const actionKind = w => ACTIONS.find(k => w?.action && typeof w.action === 'object' && k in w.action) || '';

function actionSection(W, item) {
  const kind = actionKind(W()), out = [h('h4', {}, 'Action')];
  const sel = h('select', {}, h('option', { value: '' }, '(none)'), ACTIONS.map(k => h('option', { value: k }, k)));
  sel.value = kind;
  sel.addEventListener('change', () => {
    const k = sel.value, t = W();
    if (!k) delete t.action;
    else t.action = k === 'goto' ? { goto: '', stack: embedNames().includes('pages') ? 'pages' : embedNames()[0] || 'pages' } : k === 'set' ? { set: {} } : k === 'step' ? { step: 0.01 }
      : k === 'sequence' ? { sequence: 'insert', prefix: '', index: '', count: 1 } : k === 'presets' ? { presets: Object.keys(S.data)[0] || '', key: '', name: '' } : k === 'data' ? { data: {} } : k === 'midi_map' ? { midi_map: 'remove' } : k === 'scale' ? { scale: 'menu' } : { [k]: '' };
    done();
  });
  out.push(row('kind', sel));
  const f = (label, path, k, opt) => out.push(field(W, [path, k, { label, ...opt }]));
  if (kind === 'goto') { f('view', 'action.goto', 'view'); f('stack', 'action.stack', 'str', { list: 'dl-stacks' }); f('vars', 'action.vars', 'vars'); }
  else if (kind === 'set') f('vars', 'action.set', 'vars');
  else if (kind === 'toggle') f('widget', 'action.toggle', 'str', { list: 'dl-widgets' });
  else if (kind === 'url') f('url', 'action.url', 'str');
  else if (kind === 'step') { f('step', 'action.step', 'num'); out.push(note('Adds this amount to the bound param.')); }
  else if (kind === 'data') { f('data', 'action.data', 'vars', { placeholder: 'key=text, ... (keys may use {var})' }); out.push(note('Stores these strings as text data, for buttons that restore a shape.')); }
  else if (kind === 'presets') {
    f('table', 'action.presets', 'str', { list: 'dl-tables' }); f('key', 'action.key', 'str', { placeholder: 'text data for the content, may use {var}' });
    f('name', 'action.name', 'str', { placeholder: 'text data for the name' }); f('save', 'action.save', 'bool'); f('columns', 'action.columns', 'int', { placeholder: 'items a column' });
    if (!item) out.push(field(W, MENU_AT));
    out.push(note('A native menu of the slots in data/<table>.json: choosing one copies its content and name into the text data; with save, a Save submenu stores them into a slot.'));
  }
  else if (kind === 'midi_map') {
    f('midi_map', 'action.midi_map', 'enum', { options: ['remove', 'reset'] });
    out.push(note('For a midi_map list (as its context menu): remove drops the selected row\'s assignment, reset restores the skin\'s learn defaults.'));
  }
  else if (kind === 'sequence') {
    f('sequence', 'action.sequence', 'enum', { options: ['insert', 'delete', 'reset', 'random'] });
    f('prefix', 'action.prefix', 'str', { placeholder: 'e.g. seq.step.' }); f('count', 'action.count', 'int');
    f('index', 'action.index', 'str', { placeholder: '{step} or a number (insert, delete)' }); f('leaf', 'action.leaf', 'str', { placeholder: 'e.g. on (reset, random)' });
    out.push(note('Params <prefix><n>.<leaf>, n = 1..count, form a step sequence: insert shifts steps up from index (the last drops off) and resets step index; delete shifts them down onto index and resets the last; reset and random set one leaf of every step to its default or a random value.'));
  }
  return out;
}


// ---- finding things: Ctrl+F searches every view's texts, menu items, widget names, tips and bindings, plus
// view, param, image and font names; choosing a hit opens it (a text for editing, a menu item in the menu editor).
function findHits(query) {
  const q = query.trim().toLowerCase();
  if (!q) return [];
  const parents = L.viewParents(S.docs), where = v => L.viewChain(parents, v).join(' \u203A '), out = [];
  const has = s => typeof s === 'string' && s.toLowerCase().includes(q);
  const rank = s => (s.toLowerCase() === q ? 0 : s.toLowerCase().startsWith(q) ? 1 : 2);
  const add = (score, kind, label, sub, go) => out.push({ score, kind, label, sub, go });
  for (const v of views()) {
    if (has(v)) add(rank(v) + 4, 'view', v, where(v), () => openView(v));
    for (const w of vdoc(v).widgets || []) {
      const who = `${w.type} ${w.name ?? ''} \u00B7 ${where(v)}`;
      if (has(w.text?.text)) add(rank(w.text.text), 'text', w.text.text, who, () => showWidget(v, w, 'text'));
      for (const key of ['items', 'context']) for (const p of L.menuPaths(w[key])) {
        const it = L.menuItemAt(w[key], p);
        if (has(it?.label)) add(rank(it.label) + 0.5, 'menu item', it.label, `${key === 'context' ? 'right-click menu' : 'menu'} of ${w.name ?? w.type} \u00B7 ${where(v)}`, () => showWidget(v, w, { menu: key, path: p }));
      }
      if (has(w.name)) add(rank(w.name) + 1, w.type, w.name, where(v), () => showWidget(v, w));
      if (has(w.tip)) add(rank(w.tip) + 2, 'tip', w.tip, who, () => showWidget(v, w));
      for (const id of [w.param, ...(Array.isArray(w.params) ? w.params : [])]) if (has(id)) add(rank(id) + 3, 'bound to', id, who, () => showWidget(v, w));
    }
  }
  for (const p of params()) if (has(p.id) || has(p.name)) add(Math.min(has(p.id) ? rank(p.id) : 9, has(p.name) ? rank(p.name) : 9) + 3, 'param', p.name || p.id, p.id, () => { setPlay(false); S.param = p.id; S.showParam = true; switchTab('params'); inspector(); });
  for (const n of imageNames()) if (has(n)) add(rank(n.slice(n.lastIndexOf('/') + 1)) + 4, 'image', n, 'Images', () => { switchTab('images'); openImage(n); });
  for (const n of fontNames()) if (has(n)) add(rank(n) + 4, 'font', n, 'Images', () => { switchTab('images'); openFont(n); });
  return out.sort((a, b) => a.score - b.score || a.label.localeCompare(b.label)).slice(0, 80);
}
// Opens view v with widget w selected and in sight; then edits its text, or opens its menu at an item.
function showWidget(v, w, then) {
  setPlay(false);
  if (S.mode !== 'view' || S.view !== v) openView(v);
  S.sel = [w];
  selChanged();
  scrollToRect(rectOf(w));
  if (then === 'text') editText(w);
  else if (then?.menu) menuEditor(() => w, then.menu, then.path);
}
function scrollToRect(r) {
  if (!validRect(r)) return;
  const x = r[0] * S.zoom + MARGIN, y = r[1] * S.zoom + MARGIN, w = r[2] * S.zoom, hh = r[3] * S.zoom;
  if (x < sc.scrollLeft || x + w > sc.scrollLeft + sc.clientWidth) sc.scrollLeft = x + w / 2 - sc.clientWidth / 2;
  if (y < sc.scrollTop || y + hh > sc.scrollTop + sc.clientHeight) sc.scrollTop = y + hh / 2 - sc.clientHeight / 2;
  schedulePaint();
}
{
  const input = $('#find'), list = $('#find-results');
  let hits = [], on = 0;
  const show = () => {
    list.hidden = !input.value.trim();
    list.replaceChildren(...(hits.length ? hits.map((r, i) => h('div', { className: `hit${i === on ? ' on' : ''}`, onmousedown: e => { e.preventDefault(); choose(i); } },
      h('span', { className: 'k' }, r.kind), h('span', { className: 'l' }, r.label), h('span', { className: 'dim' }, r.sub))) : [h('div', { className: 'none' }, 'Nothing matches')]));
    list.querySelector('.on')?.scrollIntoView({ block: 'nearest' });
  };
  const choose = i => { const r = hits[i]; if (!r) return; list.hidden = true; input.blur(); r.go(); };
  input.addEventListener('input', () => { hits = findHits(input.value); on = 0; show(); });
  input.addEventListener('focus', () => { if (input.value.trim()) { hits = findHits(input.value); show(); } });
  input.addEventListener('blur', () => { list.hidden = true; });
  input.addEventListener('keydown', e => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); on = L.clamp(on + (e.key === 'ArrowDown' ? 1 : -1), 0, Math.max(0, hits.length - 1)); show(); }
    else if (e.key === 'Enter') { e.preventDefault(); choose(on); }
    else if (e.key === 'Escape') { e.preventDefault(); input.value = ''; list.hidden = true; input.blur(); }
  });
}

// Double-click on the canvas: a widget inside an embed opens its own view with it selected; a text is
// edited in place; a dropdown opens its menu; an embed or a goto button without text opens its view.
function dblClick(p) {
  const deep = deepAt(p);
  if (deep && deep.node.view !== S.view && vdoc(deep.node.view)) {
    const from = S.view;
    showWidget(deep.node.view, deep.w);
    return msg(`Opened ${deep.node.view}, shown in ${from}; Back (Alt+Left) returns`);
  }
  const w = S.sel.length === 1 && L.contains(rectOf(S.sel[0]), p.x, p.y) ? S.sel[0] : hitAll(p)[0];
  if (!w) return;
  if (w.type === 'dropdown') return menuEditor(() => w, 'items');
  if (['plate', 'button', 'textbox'].includes(w.type) && (w.text?.text != null || !w.action?.goto)) return editText(w);
  openTarget(w);
}
// The innermost shown widget under p, through embeds: a play-tree item ({ w, rect, box, node: { view } }).
function deepAt(p) {
  const walk = n => {
    for (const it of L.drawOrder(n.items, i => i.w).reverse()) {
      if (!it.vis || !validRect(it.box) || !L.contains(it.box, p.x, p.y)) continue;
      if (it.child && L.contains(it.child.clip, p.x, p.y)) { const d = walk(it.child); if (d) return d; }
      return it;
    }
    return null;
  };
  return S.mode === 'view' && S.pc?.root ? walk(S.pc.root) : null;
}
// A widget's text edited in a plain input over it (Enter keeps it, Escape drops it; Ctrl+Enter in a multiline box).
function editText(w) {
  closeInline();
  const r = rectOf(w);
  if (!validRect(r)) return;
  const [x, y] = toScreen(r[0], r[1]), multi = !!w.multiline, old = w.text?.text ?? '';
  const el = h(multi ? 'textarea' : 'input', { className: 'inline-edit', spellcheck: false, title: multi ? 'Ctrl+Enter keeps it, Escape drops it' : 'Enter keeps it, Escape drops it' });
  el.value = old;
  Object.assign(el.style, { left: `${x}px`, top: `${y}px`, width: `${Math.max(80, r[2] * S.zoom)}px`, height: `${Math.max(20, r[3] * S.zoom)}px` });
  const end = ok => {
    if (S.inline !== el) return;
    S.inline = null;
    el.remove();
    if (ok && el.value !== old) { L.setPath(w, 'text.text', el.value); commit(); scheduleRender(); inspector(); msg(`Text of ${w.name}: "${el.value}" (Ctrl+S saves)`); }
  };
  el.addEventListener('keydown', e => {
    e.stopPropagation();
    if (e.key === 'Escape') { e.preventDefault(); end(false); }
    else if (e.key === 'Enter' && (!multi || e.ctrlKey)) { e.preventDefault(); end(true); }
  });
  el.addEventListener('blur', () => end(true));
  $('#viewport').append(el);
  S.inline = el;
  el.focus(); el.select();
}

// ---- menu editor: a dropdown's items or a widget's context menu as a tree, the selected item's
// fields, and a preview in the widget's menu style. It edits a copy; Apply writes it back as one undo
// step. While it is open, done() and live() (the field controls' hooks) refresh it instead.
let menuEd = null;
function menuCtl(W, key) {
  const sum = h('span', { className: 'sum' });
  upd(() => {
    const items = W()?.[key], all = L.menuPaths(items).map(p => L.menuKind(L.menuItemAt(items, p)));
    const n = all.filter(k => k !== 'separator').length, subs = all.filter(k => k === 'submenu').length;
    sum.textContent = n ? `${n} item${n === 1 ? '' : 's'}${subs ? `, ${subs} submenu${subs === 1 ? '' : 's'}` : ''}` : 'none';
  });
  return [sum, h('button', { onclick: () => menuEditor(W, key), title: 'Edit the items, submenus, check marks, columns and actions' }, 'Edit menu...')];
}
function menuEditor(W, key, at) {
  const dlg = $('#menu-dlg'), w = W();
  if (!w) return;
  const M = { items: clone(Array.isArray(w[key]) ? w[key] : []), sel: [], mode: 'item', sig: '', updaters: [], lays: [], drag: null };
  M.orig = JSON.stringify(M.items); M.hist = [M.orig]; M.hi = 0;
  M.sel = at && L.menuItemAt(M.items, at) ? at.slice() : M.items.length ? [0] : [];
  const cur = () => L.menuItemAt(M.items, M.sel);
  const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
  const tree = h('div', { className: 'mtree', tabIndex: 0 }), props = h('div', { className: 'mprops' });
  const pv = h('canvas', { className: 'preview', title: 'Click an item to select it' }), pvBox = h('div', { className: 'pv' }, pv);
  const state = h('span', { className: 'dim' });

  const snap = () => { const t = JSON.stringify(M.items); if (t !== M.hist[M.hi]) { M.hist.length = M.hi + 1; M.hist.push(t); M.hi = M.hist.length - 1; } };
  const restore = i => {
    if (i < 0 || i >= M.hist.length) return;
    M.hi = i; M.items = JSON.parse(M.hist[i]);
    if (!cur()) M.sel = M.items.length ? [0] : [];
    refresh(true);
  };
  function refresh(force) {
    drawTree(); drawPreview();
    const it = cur(), sig = JSON.stringify([M.mode, M.sel, !!it, L.menuKind(it), actionKind(it)]);
    if (force || sig !== M.sig) { M.sig = sig; buildProps(); } else M.updaters.forEach(u => u());
    const n = M.hist.length - 1 - M.hi;
    if (JSON.stringify(M.sel) !== M.shown) { M.shown = JSON.stringify(M.sel); reveal(); }
    state.textContent = JSON.stringify(M.items) === M.orig ? 'no changes' : 'changed';
    bUndo.disabled = M.hi <= 0; bRedo.disabled = !n;
  }
  // Scrolls the tree and the preview to the selected item.
  function reveal() {
    tree.querySelector('.mrow.on')?.scrollIntoView({ block: 'nearest' });
    const lay = M.lays.find(l => l.pre.length === M.sel.length - 1)?.lay, r = lay?.rows[M.sel.at(-1)]?.rect;
    if (!r) return;
    const k = pv.getBoundingClientRect().width / pv.width * 2 || 2, x = r[0] * k, y = r[1] * k, rw = r[2] * k, rh = r[3] * k;
    if (y < pvBox.scrollTop || y + rh > pvBox.scrollTop + pvBox.clientHeight) pvBox.scrollTop = y + rh / 2 - pvBox.clientHeight / 2;
    if (x < pvBox.scrollLeft || x + rw > pvBox.scrollLeft + pvBox.clientWidth) pvBox.scrollLeft = x + rw - pvBox.clientWidth + 16;
  }
  const select = p => { M.sel = p; if (M.mode !== 'item') M.mode = 'item'; refresh(); };
  menuEd = { changed: () => { snap(); refresh(); }, live: () => { drawTree(); drawPreview(); }, redraw: () => drawPreview() };

  // edits of the tree
  const after = () => (M.sel.length ? [...M.sel.slice(0, -1), M.sel.at(-1) + 1] : [M.items.length]);
  const moved = p => { if (p) { M.sel = p; done(); } };
  const ops = {
    add: (...list) => { M.sel = L.menuInsert(M.items, after(), ...list); M.mode = 'item'; done(); },
    item: () => ops.add({ label: 'Item', value: L.menuNextValue(M.items) }),
    separator: () => ops.add({ separator: true }),
    submenu: () => ops.add({ label: 'Submenu', items: [{ label: 'Item', value: L.menuNextValue(M.items) }] }),
    duplicate: () => cur() && ops.add(clone(cur())),
    remove: () => {
      if (!cur()) return;
      L.menuRemove(M.items, M.sel);
      const s = M.sel.slice();
      if (!L.menuItemAt(M.items, s)) { if (s.at(-1) > 0) s[s.length - 1]--; else s.pop(); }
      M.sel = L.menuItemAt(M.items, s) ? s : [];
      done();
    },
    up: () => cur() && M.sel.at(-1) > 0 && moved(L.menuMove(M.items, M.sel, [...M.sel.slice(0, -1), M.sel.at(-1) - 1])),
    down: () => cur() && L.menuItemAt(M.items, [...M.sel.slice(0, -1), M.sel.at(-1) + 1]) && moved(L.menuMove(M.items, M.sel, [...M.sel.slice(0, -1), M.sel.at(-1) + 2])),
    indent: () => cur() && moved(L.menuIndent(M.items, M.sel)),
    outdent: () => cur() && moved(L.menuOutdent(M.items, M.sel)),
  };
  const tool = (label, fn, title) => h('button', { type: 'button', title, onclick: () => { fn(); tree.focus(); } }, label);
  const bUndo = tool('Undo', () => restore(M.hi - 1), 'Undo in this menu (Ctrl+Z)'), bRedo = tool('Redo', () => restore(M.hi + 1), 'Redo (Ctrl+Y)');

  function drawTree() {
    const top = tree.scrollTop;
    const rows = L.menuPaths(M.items).map(p => {
      const it = L.menuItemAt(M.items, p), k = L.menuKind(it), a = actionKind(it);
      const badges = [k === 'item' && !a && it.value !== undefined ? `= ${it.value}` : null, it.check ? 'check' : null, it.columnBreak ? 'column' : null,
        a ? `${a}: ${typeof it.action[a] === 'object' ? L.formatVars(it.action[a]) : String(it.action[a] ?? '')}`.slice(0, 28) : null, it.short ? `short "${it.short}"` : null].filter(Boolean);
      const r = h('div', { className: `mrow ${k}${same(p, M.sel) ? ' on' : ''}`, draggable: true, style: `padding-left:${6 + (p.length - 1) * 18}px` },
        h('span', { className: 'grip' }, '\u2261'),
        k === 'separator' ? h('span', { className: 'sepline' }) : h('span', { className: 'name' }, it.label ?? '(no label)'),
        k === 'submenu' ? h('span', { className: 'dim' }, `\u25B8 ${it.items.length}`) : null,
        badges.map(b => tag(b)));
      const zone = e => {
        const b = r.getBoundingClientRect(), f = (e.clientY - b.top) / b.height;
        return k === 'submenu' ? (f < 0.3 ? 'before' : 'into') : f < 0.5 ? 'before' : 'after';
      };
      r.addEventListener('click', () => { select(p); tree.focus(); });
      r.addEventListener('dragstart', e => { M.drag = p; e.dataTransfer.effectAllowed = 'move'; e.dataTransfer.setData('text/plain', it.label ?? ''); });
      r.addEventListener('dragend', () => { M.drag = null; });
      r.addEventListener('dragover', e => { if (!M.drag) return; e.preventDefault(); e.stopPropagation(); r.dataset.drop = zone(e); });
      r.addEventListener('dragleave', () => { delete r.dataset.drop; });
      r.addEventListener('drop', e => {
        e.preventDefault(); e.stopPropagation();
        const z = zone(e), from = M.drag;
        delete r.dataset.drop; M.drag = null;
        if (from) moved(L.menuMove(M.items, from, z === 'into' ? [...p, 0] : z === 'after' ? [...p.slice(0, -1), p.at(-1) + 1] : p));
      });
      return r;
    });
    tree.replaceChildren(...(rows.length ? rows : [note('No items yet: add one above.')]));
    tree.scrollTop = top;
  }
  tree.addEventListener('dragover', e => { if (M.drag) e.preventDefault(); });
  tree.addEventListener('drop', e => { e.preventDefault(); const from = M.drag; M.drag = null; if (from) moved(L.menuMove(M.items, from, [M.items.length])); });

  function drawPreview() {
    const t = W() || {}, s = skin() || {};
    const style = L.menuStyleOf(s, t.menuStyle ?? (key === 'context' && s.menu?.styles?.plain ? 'plain' : undefined)) || PLAIN_MENU;
    const f = metricsOf(style), p = t.param && S.pmap.get(t.param), v = t.value ?? p?.default ?? p?.min ?? 0;
    // check items show their mark; a skin-styled dropdown also marks the item for the current value
    const checked = it => L.menuKind(it) === 'item' && (!!it.check || (!style.native && key === 'items' && !actionKind(it) && Math.abs((+it.value || 0) - v) < 1e-9));
    M.lays = [];
    let items = M.items, anchor = [4, 4, key === 'items' ? t.rect?.[2] || 0 : 0, 0];
    for (let d = 0; Array.isArray(items) && items.length; d++) {
      const lay = L.menuLayout(items, style, f, anchor, [1e4, 1e4], d ? 'side' : 'below'), hover = d < M.sel.length ? M.sel[d] : -1;
      M.lays.push({ lay, hover, pre: M.sel.slice(0, d) });
      if (!lay.rows[hover] || !Array.isArray(items[hover]?.items)) break;
      anchor = lay.rows[hover].rect; items = items[hover].items;
    }
    const Wd = Math.max(60, ...M.lays.map(({ lay }) => lay.rect[0] + lay.rect[2] + 4)), Hd = Math.max(24, ...M.lays.map(({ lay }) => lay.rect[1] + lay.rect[3] + 4));
    const o = h('canvas', { width: Wd, height: Hd }), g = o.getContext('2d');
    g.imageSmoothingEnabled = false;
    fillCol(g, [0, 0, Wd, Hd], '#3a3d44ff');
    for (const { lay, hover } of M.lays) drawMenu(g, lay, style, hover, f, checked);
    pv.width = Wd * 2; pv.height = Hd * 2;
    const cg = pv.getContext('2d');
    cg.imageSmoothingEnabled = false;
    cg.drawImage(o, 0, 0, Wd * 2, Hd * 2);
  }
  pv.addEventListener('click', e => {
    const r = pv.getBoundingClientRect(), x = (e.clientX - r.left) * pv.width / r.width / 2, y = (e.clientY - r.top) * pv.height / r.height / 2;
    for (let k = M.lays.length - 1; k >= 0; k--) { const row = L.menuRowAt(M.lays[k].lay, x, y); if (row) return select([...M.lays[k].pre, row.i]); }
  });

  // the right-hand panel: the selected item, the whole menu as JSON, or lines to add
  function buildProps() {
    const prev = updaters;
    updaters = M.updaters = [];
    try { props.replaceChildren(...(M.mode === 'json' ? jsonPanel() : M.mode === 'lines' ? linesPanel() : itemPanel()).flat(Infinity)); }
    finally { updaters = prev; }
  }
  function itemPanel() {
    const I = () => cur(), it = I();
    if (!it) return [note(M.items.length ? 'Select an item in the list or the preview.' : 'The menu is empty: add an item, a separator or a submenu.')];
    const kind = L.menuKind(it), ks = h('select', {}, ['item', 'separator', 'submenu'].map(k => h('option', { value: k }, k)));
    ks.value = kind;
    ks.addEventListener('change', () => {
      const x = I(), label = x.label;
      for (const k of Object.keys(x)) delete x[k];
      if (ks.value === 'separator') x.separator = true;
      else {
        x.label = label ?? (ks.value === 'submenu' ? 'Submenu' : 'Item');
        if (ks.value === 'submenu') x.items = [{ label: 'Item', value: L.menuNextValue(M.items) }]; else x.value = L.menuNextValue(M.items);
      }
      done();
    });
    const out = [h('h4', {}, 'Item'), row('kind', ks, 'A separator is a line; a submenu opens its items to the right')];
    if (kind !== 'separator') out.push(field(I, ['label', 'str']));
    if (kind === 'item') {
      out.push(field(I, ['value', 'num', { title: 'The value choosing it sets (when it has no action)' }]),
        field(I, ['short', 'str', { title: 'With captionFromItem the dropdown shows this instead of the label' }]),
        field(I, ['check', 'bool', { title: 'Shows a check mark: while its action is on, or always in a native menu' }]));
    }
    if (kind === 'submenu') out.push(note(`Opens its ${it.items.length} item${it.items.length === 1 ? '' : 's'} to the right on hover. Drag items onto it, or select one below it and indent.`));
    if (kind !== 'separator') out.push(field(I, ['columnBreak', 'bool', { label: 'column break', title: 'Starts a new column at this item (native menus)' }]));
    if (kind === 'item') out.push(...actionSection(I, true), note('An item with an action runs it instead of setting the value.'));
    const ta = h('textarea', { rows: 6, spellcheck: false });
    upd(() => { if (!keep(ta)) ta.value = L.stringify(I() || {}); });
    out.push(h('h4', {}, 'This item as JSON'), note('Any field, including ones this panel does not show.'), h('div', { className: 'f', style: 'display:block' }, ta),
      btns(['Apply JSON', () => {
        let o;
        try { o = JSON.parse(ta.value); } catch (e) { return msg(`JSON: ${e.message}`, true); }
        if (!o || typeof o !== 'object' || Array.isArray(o)) return msg('A menu item is an object', true);
        const x = I();
        for (const k of Object.keys(x)) delete x[k];
        Object.assign(x, o);
        done();
      }]));
    return out;
  }
  function jsonPanel() {
    const ta = h('textarea', { rows: 22, spellcheck: false, value: L.stringify(M.items) });
    return [h('h4', {}, 'The whole menu as JSON'), note('An array of items: { "label", "value" }, { "separator": true }, { "label", "items": [...] }, plus check, short, columnBreak and action.'),
      h('div', { className: 'f', style: 'display:block' }, ta),
      btns(['Use this JSON', () => {
        let o;
        try { o = JSON.parse(ta.value); } catch (e) { return msg(`JSON: ${e.message}`, true); }
        if (!Array.isArray(o) || !o.every(x => x && typeof x === 'object' && !Array.isArray(x))) return msg('A menu is an array of item objects', true);
        M.items = o; M.sel = o.length ? [0] : []; M.mode = 'item';
        done();
      }], ['Back', () => { M.mode = 'item'; refresh(true); }])];
  }
  function linesPanel() {
    const ta = h('textarea', { rows: 14, spellcheck: false, placeholder: 'Sine = 0\nTriangle\n-\nNoise' });
    return [h('h4', {}, 'Add items from lines'), note(`One item per line: label = value, or - for a separator. Lines without a value count on from the largest value (${L.menuNextValue(M.items)} next). They go after the selected item, at its level.`),
      h('div', { className: 'f', style: 'display:block' }, ta),
      btns(['Add', () => { const add = L.parseItems(ta.value, [], L.menuNextValue(M.items)); if (add) ops.add(...add); }], ['Back', () => { M.mode = 'item'; refresh(true); }])];
  }

  function close(apply) {
    if (apply) {
      const t = W();
      if (t) { if (M.items.length) t[key] = M.items; else delete t[key]; }
    } else if (JSON.stringify(M.items) !== M.orig && !confirm('Discard the changes to this menu?')) return;
    menuEd = null;
    dlg.close();
    if (apply) done();
  }
  dlg.oncancel = e => { e.preventDefault(); close(false); };
  dlg.onkeydown = e => {
    const t = e.target, k = e.key, ctrl = e.ctrlKey || e.metaKey;
    const typing = t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || (t.tagName === 'INPUT' && !['checkbox', 'button'].includes(t.type));
    if (ctrl && k === 'Enter') { e.preventDefault(); return close(true); }
    if (typing) return;
    const lk = k.toLowerCase();
    if (ctrl && (lk === 'z' || lk === 'y')) { e.preventDefault(); return restore(M.hi + (lk === 'y' || e.shiftKey ? 1 : -1)); }
    if (t !== tree) return;
    const paths = L.menuPaths(M.items), i = paths.findIndex(p => same(p, M.sel));
    const act = e.altKey ? { ArrowUp: ops.up, ArrowDown: ops.down, ArrowLeft: ops.outdent, ArrowRight: ops.indent }[k]
      : ctrl ? { d: ops.duplicate }[lk]
      : { ArrowUp: () => paths[i - 1] && select(paths[i - 1]), ArrowDown: () => paths[i + 1] && select(paths[i + 1]), Home: () => paths[0] && select(paths[0]),
        End: () => paths.length && select(paths.at(-1)), Delete: ops.remove, Insert: ops.item }[k];
    if (act) { e.preventDefault(); act(); }
  };

  const title = `${key === 'context' ? 'Right-click menu' : 'Dropdown menu'} of ${w.name || w.type}`;
  const styleName = w.menuStyle || (key === 'context' && skin()?.menu?.styles?.plain ? 'plain' : skin()?.menu ? 'the skin menu style' : 'native');
  dlg.replaceChildren(h('div', { className: 'menued' },
    h('div', { className: 'head' }, h('h3', {}, title), h('span', { className: 'dim' }, `drawn in ${styleName}`), h('span', { className: 'grow' }), state),
    h('div', { className: 'tools' },
      tool('+ Item', ops.item, 'Add an item after the selected one (Insert)'), tool('+ Separator', ops.separator, 'Add a separator line'), tool('+ Submenu', ops.submenu, 'Add a submenu'),
      h('span', { className: 'sep' }),
      tool('Duplicate', ops.duplicate, 'Ctrl+D'), tool('Delete', ops.remove, 'Delete'),
      h('span', { className: 'sep' }),
      tool('\u25B2', ops.up, 'Move up (Alt+Up)'), tool('\u25BC', ops.down, 'Move down (Alt+Down)'),
      tool('\u25C0 Out', ops.outdent, 'Out of its submenu (Alt+Left)'), tool('In \u25B6', ops.indent, 'Into the submenu of the item above (Alt+Right)'),
      h('span', { className: 'sep' }), bUndo, bRedo),
    h('div', { className: 'main' }, tree, props),
    pvBox,
    h('div', { className: 'foot' },
      h('button', { type: 'button', onclick: () => { M.mode = 'lines'; refresh(true); } }, 'Add from lines...'),
      h('button', { type: 'button', onclick: () => { M.mode = 'json'; refresh(true); } }, 'Edit as JSON...'),
      h('span', { className: 'grow dim' }, 'Drag items to reorder; drop on a submenu to put them inside. Ctrl+Enter applies.'),
      h('button', { type: 'button', onclick: () => close(false) }, 'Cancel'),
      h('button', { type: 'button', className: 'primary', onclick: () => close(true) }, 'Apply'))));
  dlg.showModal();
  refresh(true);
  tree.focus();
}

// showIf / enableIf: kind, param or var name (with pickers) and the value to compare with.
const COND_KINDS = [['', '(always)'], ['param:on', 'param on'], ['param:eq', 'param ='], ['param:ne', 'param ≠'], ['param:ge', 'param ≥'], ['param:le', 'param ≤'],
  ['var:eq', 'var ='], ['var:ne', 'var ≠'], ['stack:is', 'stack shows'], ['json:all', 'all / any (JSON)']];
function condCtl(W, key) {
  const kind = h('select', { title: 'param on: true while the param is not at its minimum' }, COND_KINDS.map(([v, l]) => h('option', { value: v }, l)));
  const name = h('input', { type: 'text', spellcheck: false, placeholder: 'name' }), value = h('input', { type: 'text', spellcheck: false, placeholder: 'value', className: 'v' });
  const hint = h('div', { className: 'hint' });
  const write = () => {
    const t = W(), c = L.makeCond({ kind: kind.value, name: name.value.trim(), value: value.value.trim(), vars: L.condForm(t?.[key]).vars });
    if (kind.value.startsWith('json') && c === undefined && value.value.trim()) return msg('The condition is not valid JSON', true);
    if (!t || (c === undefined && kind.value)) return; // wait for a name
    L.setPath(t, key, c);
    done();
  };
  for (const el of [kind, name, value]) el.addEventListener('change', write);
  upd(() => {
    const f = L.condForm(W()?.[key]), it = S.items.get(W());
    if (!keep(kind)) kind.value = f.kind;
    if (!keep(name)) name.value = f.name;
    if (!keep(value)) value.value = f.value;
    const [on, m] = kind.value.split(':');
    name.setAttribute('list', on === 'var' ? 'dl-vars' : on === 'stack' ? 'dl-stacks' : 'dl-params');
    value.setAttribute('list', on === 'stack' ? 'dl-views' : '');
    name.hidden = !kind.value || on === 'json'; value.hidden = !kind.value || m === 'on';
    value.className = on === 'json' ? 'json' : 'v';
    value.placeholder = on === 'json' ? '{ "any": [ ... ] }' : 'value';
    const now = it && (key === 'showIf' ? it.show : it.enabled || W()?.disabled);
    hint.textContent = it && W()?.[key] != null && !now ? (key === 'showIf' ? 'false now: hidden' : 'false now: disabled') : '';
  });
  return [h('div', { className: 'f cond' }, h('label', {}, key), h('div', { className: 'ctl' }, kind, name, value)), hint];
}
const varNames = () => [...new Set([...Object.keys(skin()?.vars || {}), ...views().flatMap(n => (vdoc(n).widgets || []).flatMap(w =>
  [...Object.keys(w.vars || {}), ...Object.keys(w.action?.vars || {}), ...Object.keys(w.action?.set || {})]))])].sort();

const PAD4 = { labels: ['left', 'top', 'right', 'bottom'] };
const MENU_SPEC = [['font', 'font'], ['fill', 'colour'], ['border', 'colour'], ['shadow', 'colour'], ['hoverFill', 'colour'], ['hoverBand', 'colour'], ['hoverFont', 'font'],
  ['disabledFont', 'font'], ['separator', 'colour'], ['separatorHeight', 'int', { def: 3 }], ['rowHeight', 'int', { placeholder: 'font + pad' }], ['pad', 'nums', PAD4],
  ['check', 'image'], ['arrow', 'image'], ['place', 'enum', { options: ['below', 'over'], def: 'below' }], ['releaseGuard', 'int', { def: 0, title: 'ms after opening during which a release does not choose' }],
  ['keys', 'enum', { options: ['all', 'escape'], def: 'all' }]];
const styleNames = () => Object.keys(skin()?.menu?.styles || {});
const TIP_SPEC = [['tooltip.font', 'font'], ['tooltip.fill', 'colour'], ['tooltip.border', 'colour'], ['tooltip.pad', 'nums', PAD4], ['tooltip.delay', 'int', { def: 700 }]];
// A sample menu in the menu style and in each named style (first item checked, second hovered, a
// submenu item and a separator), and a tooltip, at 2x.
function stylePreview() {
  const c = h('canvas', { className: 'preview', width: 1, height: 1 });
  upd(() => {
    const s = skin() || {}, ts = s.tooltip || PLAIN_TIP, tf = fontInfo(ts.font);
    const styles = s.menu ? [L.menuStyleOf(s), ...styleNames().map(n => L.menuStyleOf(s, n))] : [PLAIN_MENU];
    const items = [{ label: 'Sine', value: 0 }, { label: 'Triangle', value: 1 }, { label: 'More', items: [{ label: 'x' }] }, { separator: true }, { label: 'Noise', value: 3 }];
    // a native menu checks only check items and starts columns without a divider
    const native = [{ label: 'Sine', value: 0 }, { label: 'Checked', check: true }, { label: 'More', items: [{ label: 'x' }] }, { label: 'Column 2', columnBreak: true }, { label: 'Noise' }];
    let x = 4;
    const menus = styles.map(ms => { const f = metricsOf(ms), m = L.menuLayout(ms.native ? native : items, ms, f, [x, 4, 70, f.lineH + 4], [1e4, 1e4], 'below'); x = m.rect[0] + m.rect[2] + 12; return [ms, f, m]; });
    const tip = L.tipLayout(s.tooltip ? 'A tooltip' : 'Native tooltip', ts, tf.measure, tf.lineH, [x - 4, -16], [1e4, 1e4]);
    const W = Math.ceil(tip.rect[0] + tip.rect[2] + 4), H = Math.ceil(Math.max(...menus.map(([, , m]) => m.rect[1] + m.rect[3]), tip.rect[1] + tip.rect[3]) + 4);
    const o = h('canvas', { width: W, height: H }), g = o.getContext('2d');
    g.imageSmoothingEnabled = false;
    fillCol(g, [0, 0, W, H], '#3a3d44ff');
    for (const [ms, f, m] of menus) { fillCol(g, [m.rect[0], 4, Math.max(70, 1), f.lineH + 4], '#5a606bff'); drawMenu(g, m, ms, 1, f, item => (ms.native ? !!item.check : item === items[0])); }
    drawTip(g, tip.text, [x - 4, -16], ts, [1e4, 1e4]);
    c.width = W * 2; c.height = H * 2;
    const cg = c.getContext('2d');
    cg.imageSmoothingEnabled = false;
    cg.drawImage(o, 0, 0, W * 2, H * 2);
  });
  return h('div', { className: 'f', style: 'display:block' }, c);
}
// Play mode's panel: what the played skin has changed, and a reset.
function playInspector() {
  const out = [h('h4', {}, 'Play')];
  out.push(note('The canvas works like the runtime: click, drag (Shift: fine), double-click, wheel, and arrows or Enter in menus. Editing is off; P returns to it.'));
  out.push(btns(['Reset', resetPlay, 'Default values, the pages and vars of the files'], ['Stop playing', () => setPlay(false), 'P']));
  const list = h('div', { className: 'refs plain' });
  let sig = '';
  upd(() => {
    const st = S.play, pm = S.pmap;
    const vals = Object.entries(st.values).filter(([id, v]) => pm.has(id) && v !== (pm.get(id).default ?? pm.get(id).min ?? 0));
    const lines = [
      ...Object.entries(st.vars).map(([k, v]) => `var ${k} = ${v}`),
      ...Object.entries(st.embeds).map(([k, e]) => `${k.slice(k.lastIndexOf('/') + 1)} shows ${e.view}${Object.keys(e.vars).length ? ` (${L.formatVars(e.vars)})` : ''}`),
      ...vals.map(([id, v]) => `${id} = ${L.displayText({}, pm.get(id), v)}${pm.get(id).unit ? ' ' + pm.get(id).unit : ''}`),
      ...Object.entries(st.text).map(([k, v]) => `text ${k} = "${v}"`),
      ...Object.entries(st.scroll).filter(([, v]) => v).map(([k, v]) => `${k.slice(k.lastIndexOf('/') + 1)} scrolled ${v} px`),
      ...Object.entries(st.hidden).map(([k, v]) => `${k.slice(k.lastIndexOf('/') + 1)} ${v ? 'hidden' : 'shown'}`),
      ...(st.learn ? [`learn target ${st.learn}`] : []), ...Object.entries(st.midiMap).map(([cc, id]) => `cc ${cc} moves ${id}`),
      ...(st.notesIn.length ? [`notes held ${st.notesIn.join(' ')}`] : []),
    ];
    const next = lines.join('\n');
    if (next === sig) return;
    sig = next;
    list.replaceChildren(...(lines.length ? lines : ['Nothing changed yet']).map(l => h('div', {}, l)));
  });
  out.push(h('h4', {}, 'State'), list);
  const kind = h('select', {}, h('option', { value: 'cc' }, 'CC'), h('option', { value: 'note' }, 'note')), num = h('input', { type: 'number', value: 1 }), val = h('input', { type: 'number', value: 64 });
  const send = () => playEvent(pc => L.playMidiIn(pc, kind.value === 'note' ? { note: +num.value, velocity: +val.value } : { cc: +num.value, value: +val.value }));
  out.push(h('h4', {}, 'MIDI in'), note('Simulated input. A CC goes to the learn target while learn is on, then moves its param and dials with that "midi"; a note with value 0 releases it.'),
    row('message', [kind, num, val, h('button', { onclick: send }, 'Send')]));
  return out;
}
function widgetInspector() {
  const W = () => S.sel[0], w = W(), out = [h('h4', {}, 'Widget')];
  out.push(row('type', enumCtl(() => W()?.type, v => { if (v) W().type = v; }, L.TYPES, null, true)));
  out.push(row('name', textCtl(() => W()?.name, v => renameWidget(W(), v))));
  out.push(row('rect', numsCtl(() => W()?.rect, v => { if (v) W().rect = v; }, ['x', 'y', 'w', 'h'])));
  out.push(row('layer', numCtl(() => W()?.layer, v => { if (v === undefined) delete W().layer; else W().layer = L.clamp(Math.round(v), 0, 7); }, { int: true, def: 3 })));
  if (w.type === 'embed') out.push(note(`Shows ${w.view || 'a view'} here. Double-click a widget inside it on the canvas to open that view with the widget selected.`));
  out.push(field(W, ['hidden', 'bool']), field(W, ['disabled', 'bool']));
  out.push(...[].concat(field(W, ['param', 'param'])));
  out.push(field(W, ['mirror', 'strlist', { list: 'dl-params', title: 'Params that get the same normalized value with every edit (may use {var})' }]));
  out.push(field(W, ['value', 'num']), field(W, ['tip', 'str']));
  out.push(h('h4', {}, 'Conditions'), ...condCtl(W, 'showIf'), ...condCtl(W, 'enableIf'));
  out.push(h('h4', {}, 'Menus'), row('menuStyle', enumCtl(() => W()?.menuStyle, v => { const t = W(); if (t) L.setPath(t, 'menuStyle', v); }, ['native', ...styleNames()]), 'The menu style of its dropdown or context menu'),
    field(W, ['context', 'menu', { title: 'The menu a right-click opens' }]));
  for (const sec of SPECS[w.type] || [FILL]) {
    if (sec === 'action') { out.push(...actionSection(W)); continue; }
    out.push(h('h4', {}, sec[0]));
    for (const spec of sec[1]) if (!spec[2]?.when || spec[2].when(w)) out.push(...[].concat(field(W, spec)));
  }
  const ta = h('textarea', { rows: 12, spellcheck: false });
  upd(() => { if (!keep(ta)) ta.value = L.stringify(W() || {}); });
  out.push(h('h4', {}, 'JSON'), h('div', { className: 'f', style: 'display:block' }, ta), btns(['Apply JSON', () => {
    let o;
    try { o = JSON.parse(ta.value); } catch (e) { return msg(`JSON: ${e.message}`, true); }
    if (!o || typeof o !== 'object' || Array.isArray(o) || !o.type) return msg('A widget is an object with a "type"', true);
    const t = W();
    for (const k of Object.keys(t)) delete t[k];
    Object.assign(t, o);
    done();
  }]));
  out.push(btns(['Duplicate', duplicate, 'Ctrl+D'], ['Layer +', () => layer(1), 'PageUp'], ['Layer -', () => layer(-1), 'PageDown'],
    (w.type === 'embed' || w.action?.goto) && ['Open view', () => openTarget(W()), 'Enter'], ['Delete', del, 'Delete', 'danger']));
  return out;
}
function multiInspector() {
  const out = [h('h4', {}, `${S.sel.length} widgets`)];
  out.push(note('Align to the selection bounds; same width and height copy the first selected.'));
  out.push(btns(['Left', () => alignSel('left')], ['Centre', () => alignSel('hcentre')], ['Right', () => alignSel('right')],
    ['Top', () => alignSel('top')], ['Middle', () => alignSel('vcentre')], ['Bottom', () => alignSel('bottom')]));
  out.push(btns(['Same width', () => alignSel('width')], ['Same height', () => alignSel('height')],
    ['Distribute across', () => distributeSel('x')], ['Distribute down', () => distributeSel('y')]));
  const all = key => v => S.sel.forEach(w => L.setPath(w, key, v));
  out.push(row('layer', numCtl(() => S.sel[0]?.layer, all('layer'), { int: true, def: 3 })));
  out.push(row('hidden', boolCtl(() => S.sel[0]?.hidden, all('hidden'))), row('disabled', boolCtl(() => S.sel[0]?.disabled, all('disabled'))));
  out.push(btns(['Duplicate', duplicate], ['Layer +', () => layer(1)], ['Layer -', () => layer(-1)], ['Delete', del, '', 'danger']));
  out.push(h('div', { className: 'refs' }, S.sel.map(w => h('div', { onclick: () => { S.sel = [w]; selChanged(); } }, `${w.name} (${w.type})`))));
  return out;
}
function viewInspector() {
  const V = () => cur(), out = [h('h4', {}, 'View')];
  out.push(row('name', textCtl(() => S.view, v => renameViewTo(S.view, v))));
  out.push(row('size', numsCtl(() => V()?.size, v => { if (v) V().size = v; }, ['width', 'height'])));
  for (const spec of FILL[1]) out.push(field(V, spec));
  out.push(field(V, ['flow', 'enum', { options: ['column'], title: 'column: widgets stack top to bottom in file order' }]));
  const flowNote = note('');
  upd(() => { flowNote.hidden = !V()?.flow; if (V()?.flow) flowNote.textContent = `Flow: visible widgets stack in file order (Ctrl+[ and Ctrl+] reorder), each keeping its x and height. While H shows hidden widgets they keep their place here; the runtime closes the gaps.`; });
  out.push(field(V, ['gap', 'int', { def: 0 }]), field(V, ['animate', 'int', { title: 'Pixels per 30 Hz frame a widget grows or shrinks by as it shows or hides' }]), flowNote);
  out.push(field(V, ['fit', 'bool', { title: 'As the root view: shrink to the widgets shown. In an embed, see the embed\'s fit.' }]));
  out.push(note(`${V().widgets?.length || 0} widgets. Click one to edit it; the bar above the canvas adds new ones.`));
  out.push(btns(['New view', newView], ['Rename', renameView], ['Delete', deleteView, '', 'danger']));
  out.push(h('h4', {}, 'Skin'));
  out.push(field(skin, ['name', 'str']), field(skin, ['root', 'view', { label: 'root view' }]), field(skin, ['vars', 'vars']));
  out.push(h('h4', {}, 'Menu style'), note('With a menu style the runtime draws dropdown menus itself, in this style; without one it uses native menus.'));
  for (const spec of MENU_SPEC) out.push(...[].concat(field(skin, ['menu.' + spec[0], ...spec.slice(1)])));
  for (const n of styleNames()) {
    const S2 = () => skin()?.menu?.styles?.[n];
    out.push(h('div', { className: 'note' }, `Named style "${n}" (fields left empty come from the menu style)`));
    for (const spec of MENU_SPEC) out.push(...[].concat(field(skin, [`menu.styles.${n}.${spec[0]}`, ...spec.slice(1)])));
    out.push(row('no hoverBand', boolCtl(() => S2()?.hoverBand === null, v => { const t = S2(); if (!t) return; if (v) t.hoverBand = null; else delete t.hoverBand; })));
    out.push(btns([`Remove "${n}"`, () => { delete skin().menu.styles[n]; if (!Object.keys(skin().menu.styles).length) delete skin().menu.styles; commit(); inspector(true); scheduleRender(); }, '', 'danger']));
  }
  out.push(btns(['Add named style', async () => {
    const r = await ask('Add a named menu style', [{ key: 'name', label: 'Name', value: styleNames().includes('plain') ? '' : 'plain' }]);
    if (!r) return;
    if (!L.validName(r.name) || r.name === 'native' || styleNames().includes(r.name)) return msg(`"${r.name}" is not a free snake_case name`, true);
    ((skin().menu ||= {}).styles ||= {})[r.name] = {};
    commit(); inspector(true);
  }, 'Widgets pick one with menuStyle; a context menu uses "plain" when it exists']));
  out.push(h('h4', {}, 'Tooltip style'), note('With a tooltip style the runtime draws tips itself after the delay; without one they are native.'));
  for (const spec of TIP_SPEC) out.push(...[].concat(field(skin, spec)));
  out.push(h('h4', {}, 'Edit all'), note('While this internal param is on, an edit through a param template with {var} writes the param of every listed value.'),
    ...field(skin, ['editAll.param', 'param', { label: 'param' }]), field(skin, ['editAll.var', 'str', { label: 'var', list: 'dl-vars' }]),
    field(skin, ['editAll.values', 'strlist', { label: 'values' }]));
  out.push(h('h4', {}, 'MIDI learn'), note('While this internal param is on, clicking a widget with a host param makes it the learn target.'),
    ...field(skin, ['learn.param', 'param', { label: 'param' }]), field(skin, ['learn.outline', 'colour', { label: 'outline' }]),
    field(skin, ['learn.defaults', 'vars', { label: 'defaults', placeholder: 'cc=param, ... (a fresh instance)' }]));
  out.push(h('h4', {}, 'Preview'), stylePreview(), btns(['Remove menu style', () => { delete skin().menu; done(); }, '', 'danger'], ['Remove tooltip style', () => { delete skin().tooltip; done(); }, '', 'danger']));
  return out;
}
function usedBy(kind, name) {
  const rs = refsTo(kind, name).filter(r => !r.meta);
  return [h('h4', {}, `Used by (${rs.length})`), h('div', { className: 'refs' }, rs.slice(0, 300).map(r => h('div', { onclick: () => goTo(r) }, where(r))))];
}
function imageInspector() {
  const n = S.image, out = [h('h4', {}, 'Image')], M = () => imeta(S.image);
  if (!exists(ip(n))) return [...out, note(`${n} does not exist`)];
  const info = note('');
  upd(() => {
    const e = getImage(ip(S.image)), m = M();
    if (!e) { info.textContent = 'Loading...'; return; }
    const t = L.tileRect(e.w, e.h, m.tiles, m.axis, 0);
    info.textContent = `${e.w} x ${e.h} px, tile ${t[2]} x ${t[3]}${(m.tiles || 1) > 1 ? `, ${m.tiles} tiles` : ''}${(m.tiles || 1) > 1 && (m.axis === 'x' ? e.w : e.h) % m.tiles ? ' (size not a multiple of tiles)' : ''}`;
  });
  out.push(row('name', h('span', {}, n)), info);
  out.push(row('tiles', numCtl(() => M().tiles, v => setMeta('tiles', v, 1), { int: true, def: 1 })));
  out.push(row('axis', enumCtl(() => M().axis, v => setMeta('axis', v, 'y'), ['y', 'x'], 'y')));
  out.push(row('passes', numCtl(() => M().passes, v => setMeta('passes', v, 1), { int: true, def: 1 }), 'How many times the image is composited'));
  out.push(row('slice', numsCtl(() => M().slice, v => setMeta('slice', v), ['top', 'right', 'bottom', 'left']), 'Nine-slice bands [top, right, bottom, left]'));
  out.push(row('preview', numsCtl(() => S.pvRect?.slice(2), v => { S.pv = v || null; }, ['width', 'height']), 'Size of the slice preview'));
  out.push(row('explode', boolCtl(() => S.explode, v => { S.explode = !!v; })));
  out.push(btns(['Use for selected', () => useImage(S.image), 'Set as fill image (or dial/meter image) of the selected widgets'],
    ['Export PNG', () => download(ip(S.image))], ['Rename', () => renameFile('image')], ['Delete', () => deleteFile('image'), '', 'danger']));
  out.push(h('h4', {}, 'Tiles'));
  out.push(btns(['Export tiles', exportTiles, `Write each tile to tiles/${n}/000.png ...`], ['Import tiles', () => importTiles(), `Rebuild the strip from tiles/${n}/*.png`],
    ['Import files...', async () => importTiles(await pickFiles(true)), 'Rebuild the strip from picked PNG files, in name order']));
  out.push(note(`Tiles go to the tiles folder next to the skin, as tiles/${n}/000.png and up. Import reads them back in name order on the image's axis.`));
  out.push(...usedBy('image', n));
  return out;
}
function fontInspector() {
  const n = S.font, out = [h('h4', {}, 'Font')];
  if (!exists(fp(n))) return [...out, note(`${n} does not exist`)];
  const info = note('');
  upd(() => {
    const e = getImage(fp(S.font));
    info.textContent = !e ? 'Loading...' : `${e.w} x ${e.h} px, ${e.font.marker ? 'marker row' : 'fixed width'}, glyphs ${e.font.h} px tall`;
  });
  out.push(row('name', h('span', {}, n)), info);
  out.push(field(skin, [`fonts.${n}.caret`, 'colour', { label: 'caret' }]), field(skin, [`fonts.${n}.selection`, 'colour', { label: 'selection' }]),
    note('Caret and selection colours when an editable number or textbox in this font is edited.'));
  out.push(h('h4', {}, 'Sample text'), h('div', { className: 'f', style: 'display:block' }, areaCtl(() => S.sample, v => { S.sample = v; }, v => v, v => v, 3)));
  out.push(btns(['Export PNG', () => download(fp(S.font))], ['Rename', () => renameFile('font')], ['Delete', () => deleteFile('font'), '', 'danger']));
  out.push(h('h4', {}, 'Glyphs'));
  out.push(btns(['Export glyphs', exportGlyphs, `Write non-empty glyphs to tiles/fonts/${n}/<code>.png`], ['Import glyphs', () => importGlyphs(), 'Rebuild the strip from the exported glyphs'],
    ['Import files...', async () => importGlyphs(await pickFiles(true)), 'Rebuild from picked files named <code>.png']));
  out.push(note(`Glyph files are named by Latin-1 code (65.png is A). Import keeps glyphs that have no file, and writes a fresh marker row.`));
  out.push(...usedBy('font', n));
  return out;
}
function paramInspector() {
  const P = () => params().find(p => p.id === S.param), out = [h('h4', {}, 'Param')];
  if (!P()) return [note('No param selected')];
  const set = (k, def) => v => { const p = P(); if (p) L.setPath(p, k, v, def); };
  out.push(row('id', textCtl(() => P()?.id, renameParam)));
  out.push(row('name', textCtl(() => P()?.name, set('name'))));
  out.push(row('min', numCtl(() => P()?.min, set('min'), { def: 0 })), row('max', numCtl(() => P()?.max, set('max'), { def: 1 })));
  out.push(row('default', numCtl(() => P()?.default, set('default'), { def: 'min' })), row('steps', numCtl(() => P()?.steps, set('steps'), { int: true, def: 0 })));
  out.push(row('taper', enumCtl(() => P()?.taper, set('taper', 'linear'), ['linear', 'log'], 'linear')));
  out.push(row('host', boolCtl(() => P()?.host, set('host', true), true), 'Hosts see and automate it; off keeps it inside the instance (saved, bindable, not automatable)'));
  out.push(row('labels', areaCtl(() => P()?.labels, set('labels'), v => (v || []).join('\n'), t => t.split('\n').map(s => s.trim()).filter(Boolean), 4), 'One label per line, for each discrete value'));
  out.push(row('unit', textCtl(() => P()?.unit, set('unit'))), row('format', textCtl(() => P()?.format, set('format'), { placeholder: '%.2f' })));
  const sample = note('');
  upd(() => { const p = P(); if (p) sample.textContent = `Default shows as "${L.displayText({}, p)}${p.unit ? ' ' + p.unit : ''}"`; });
  out.push(sample, btns(['Delete param', deleteParam, '', 'danger']));
  const uses = paramUses(S.param);
  out.push(h('h4', {}, `Used by (${uses.length})`), h('div', { className: 'refs' }, uses.slice(0, 300).map(r => h('div', { onclick: () => goTo(r) }, `${where(r)}${r.name !== S.param ? `  (${r.name})` : ''}`))));
  return out;
}
function inspector(force) {
  menuEd?.redraw(); // an image the menu preview uses may just have loaded
  const sig = JSON.stringify([S.playing, S.showParam && S.param, S.mode, S.view, S.image, S.font, S.sel.map(w => [w.name, w.type, actionKind(w), w.kind])]);
  if (!force && sig === inspSig) { updaters.forEach(u => u()); return; }
  inspSig = sig;
  updaters = [];
  const out = S.playing ? playInspector() : S.showParam ? paramInspector() : S.mode === 'image' ? imageInspector() : S.mode === 'font' ? fontInspector()
    : !cur() ? [note('No view')] : S.sel.length === 1 ? widgetInspector() : S.sel.length ? multiInspector() : viewInspector();
  $('#inspector').replaceChildren(...out.flat(Infinity));
}

// ---- left panels -----------------------------------------------------------------------------------------------------

function switchTab(t) {
  S.tab = t;
  for (const b of document.querySelectorAll('.tabs button')) b.classList.toggle('on', b.dataset.tab === t);
  for (const s of document.querySelectorAll('.tab')) s.hidden = s.id !== `tab-${t}`;
  panels(true);
}
const tag = (text, cls) => h('span', { className: `tag ${cls || ''}` }, text);
function goTo(r) {
  setPlay(false);
  const m = /^views\/(.+)\.json$/.exec(r.path);
  if (m && vdoc(m[1])) {
    openView(m[1]);
    const w = r.widget != null && cur().widgets?.find(x => x.name === r.widget);
    if (w) {
      S.sel = [w];
      const r = rectOf(w);
      if (validRect(r)) { sc.scrollLeft = MARGIN + (r[0] + r[2] / 2) * S.zoom - sc.clientWidth / 2; sc.scrollTop = MARGIN + (r[1] + r[3] / 2) * S.zoom - sc.clientHeight / 2; }
      selChanged();
    }
  } else if (r.path === 'params.json') switchTab('params');
  else if (r.path === 'skin.json') { S.sel = []; openView(S.view, false); }
}
function buildViews() {
  const q = $('#views-filter').value.toLowerCase(), tags = L.viewTags(S.docs), root = skin()?.root;
  $('#views-list').replaceChildren(...views().filter(n => n.includes(q)).map(n => {
    const v = vdoc(n);
    return h('div', { className: `row${n === S.view && S.mode === 'view' ? ' on' : ''}`, onclick: () => openView(n), title: `${n}: ${(v.widgets || []).length} widgets` },
      h('span', { className: 'name' }, n), n === root ? tag('root', 'root') : null,
      [...(tags[n] || [])].map(s => tag(s === 'pages' ? 'page' : s)), h('span', { className: 'dim' }, Array.isArray(v.size) ? v.size.join('x') : '?'));
  }));
}
const thumbObs = new IntersectionObserver(es => { for (const en of es) if (en.isIntersecting) { thumbObs.unobserve(en.target); drawThumb(en.target); } });
function drawThumb(c) {
  const path = c.dataset.path, font = path.startsWith('fonts/');
  ensureImage(path).then(e => {
    const g = c.getContext('2d');
    g.clearRect(0, 0, 32, 32);
    if (font) { for (const q of L.layoutText(e.font, 'Ab', [0, 0, 32, 32], { align: 'center', valign: 'middle' })) g.drawImage(e.bmp, q.sx, q.sy, q.w, q.h, q.dx, q.dy, q.w, q.h); return; }
    const m = imeta(path.slice(7, -4)), s = L.tileRect(e.w, e.h, m.tiles, m.axis, 0), k = Math.min(32 / s[2], 32 / s[3]);
    const z = k >= 1 ? Math.min(Math.floor(k), 4) : k;
    g.imageSmoothingEnabled = z < 1;
    g.drawImage(e.bmp, ...s, (32 - s[2] * z) / 2, (32 - s[3] * z) / 2, s[2] * z, s[3] * z);
  }).catch(() => {});
}
function fileRow(n, font) {
  const path = font ? fp(n) : ip(n), m = font ? {} : imeta(n), c = h('canvas', { width: 32, height: 32 });
  c.dataset.path = path;
  thumbObs.observe(c);
  const on = font ? S.mode === 'font' && S.font === n : S.mode === 'image' && S.image === n;
  return h('div', { className: `row${on ? ' on' : ''}`, title: path, onclick: () => (font ? openFont(n) : openImage(n)) }, c,
    h('span', { className: 'name' }, font ? n : n.slice(n.lastIndexOf('/') + 1)),
    (m.tiles || 1) > 1 ? h('span', { className: 'dim' }, `${m.tiles}${m.axis === 'x' ? '→' : '↓'}`) : null,
    m.slice ? h('span', { className: 'dim', title: 'nine-slice' }, '9') : null, path in S.bins ? tag('unsaved') : null);
}
function buildImages() {
  const q = $('#images-filter').value.toLowerCase(), groups = new Map();
  for (const n of imageNames()) if (n.includes(q)) { const g = groupOf(n) || '(no group)'; if (!groups.has(g)) groups.set(g, []); groups.get(g).push(n); }
  const fonts = fontNames().filter(n => n.includes(q));
  const open = S.imagesOpen ||= new Set();
  const det = (key, title, rows) => {
    const d = h('details', { open: !q ? open.has(key) || (S.mode === 'image' && groupOf(S.image) === key) || (key === '/fonts' && S.mode === 'font') : true }, h('summary', {}, title));
    d.addEventListener('toggle', () => { if (d.open) { open.add(key); if (!d.children[1]) d.append(...rows()); } else open.delete(key); });
    if (d.open) d.append(...rows());
    return d;
  };
  $('#images-list').replaceChildren(
    ...[...groups].sort((a, b) => a[0].localeCompare(b[0])).map(([g, ns]) => det(g, `${g} (${ns.length})`, () => ns.map(n => fileRow(n)))),
    det('/fonts', `fonts (${fonts.length})`, () => fonts.map(n => fileRow(n, true))));
}
function buildParams() {
  const q = $('#params-filter').value.toLowerCase(), counts = new Map();
  const bound = L.refs(S.docs).filter(r => r.kind === 'param');
  for (const p of params()) counts.set(p.id, 0);
  for (const r of bound) {
    if (counts.has(r.name)) counts.set(r.name, counts.get(r.name) + 1);
    else if (r.name.includes('{')) { const m = L.paramMatcher(r.name); for (const id of counts.keys()) if (m(id)) counts.set(id, counts.get(id) + 1); }
  }
  const host = $('#params-host').value, keep = p => !host || (host === 'internal') === (p.host === false);
  $('#params-list').replaceChildren(...params().filter(p => keep(p) && `${p.id} ${p.name}`.toLowerCase().includes(q)).map(p =>
    h('div', { className: `row${S.showParam && S.param === p.id ? ' on' : ''}`, title: p.name, onclick: () => { S.param = p.id; S.showParam = true; panels(true); inspector(); } },
      h('span', { className: 'name' }, p.id), p.host === false ? tag('internal', 'internal') : null, h('span', { className: 'dim' }, counts.get(p.id) ? `${counts.get(p.id)}` : 'unused'))));
}
function buildCheck() {
  const issues = L.validate(S.docs, { images: new Set(imageNames()), fonts: new Set(fontNames()) });
  $('#check-count').textContent = issues.length ? `${issues.length} issue${issues.length === 1 ? '' : 's'}` : 'No issues';
  $('#check-list').replaceChildren(...issues.slice(0, 2000).map(i => h('div', { className: 'issue', onclick: () => goTo(i) }, i.msg, h('div', { className: 'where' }, where(i)))));
}
let panelSig = '';
function panels(force) {
  const t = S.tab;
  const sig = t + '|' + (t === 'views' ? JSON.stringify([views().map(n => vdoc(n).size), Object.entries(L.viewTags(S.docs)).map(([k, v]) => [k, ...v]), skin()?.root, S.view, S.mode, views()])
    : t === 'images' ? JSON.stringify([imageNames(), fontNames(), skin()?.images, S.binRev, S.image, S.font, S.mode])
      : t === 'params' ? S.hist[S.hi].texts['params.json'] + S.param + S.showParam + $('#params-host').value + views().flatMap(n => (vdoc(n).widgets || []).map(w => w.param)).join()
        : `${S.hi}:${S.binRev}`);
  if (!force && sig === panelSig) return;
  panelSig = sig;
  ({ views: buildViews, images: buildImages, params: buildParams, check: buildCheck })[t]();
}
function fillList(id, items) {
  const el = $(`#${id}`), sig = items.join('\n');
  if (el.dataset.sig === sig) return;
  el.dataset.sig = sig;
  el.replaceChildren(...items.map(v => h('option', { value: v })));
}
function datalists() {
  fillList('dl-images', imageNames()); fillList('dl-fonts', fontNames()); fillList('dl-views', views());
  fillList('dl-params', params().map(p => p.id)); fillList('dl-stacks', embedNames()); fillList('dl-vars', varNames()); fillList('dl-tables', Object.keys(S.data).sort());
  fillList('dl-widgets', (cur()?.widgets || []).map(w => w.name).filter(Boolean));
  fillList('dl-groups', [...new Set([...GROUPS, ...imageNames().map(groupOf).filter(Boolean)])].sort());
}
function changed() {
  const dirty = dirtyPaths().length;
  $('#dirty').textContent = dirty ? `${dirty} unsaved` : '';
  document.title = `${dirty ? '* ' : ''}${S.skins?.current.name || 'Hollow'} - Hollow skin editor`;
  $('#b-undo').disabled = S.hi <= 0;
  $('#b-redo').disabled = S.hi >= S.hist.length - 1;
  $('#b-back').disabled = !S.back.length && S.mode === 'view';
  crumb();
  selInfo();
  datalists();
  panels();
  inspector();
  scheduleRender();
}

// ---- wiring ---------------------------------------------------------------------------------------------------------------

for (const t of L.TYPES) $('#palette').append(h('button', { onclick: () => addWidget(t), title: `Add a ${t} to the view` }, t));
$('#b-skin').onclick = skinDialog;
$('#b-save').onclick = run(save);
$('#b-undo').onclick = undo;
$('#b-redo').onclick = redo;
$('#b-back').onclick = back;
$('#b-fit').onclick = fit;
$('#b-play').onclick = () => setPlay(!S.playing);
$('#params-host').addEventListener('change', () => panels(true));
$('#b-help').onclick = () => $('#help').showModal();
$('#zoom').onchange = e => setZoom(+e.target.value);
$('#show-hidden').onchange = e => { S.showHidden = e.target.checked; scheduleRender(); };
$('#show-boxes').onchange = e => { S.showBoxes = e.target.checked; schedulePaint(); };
for (const b of document.querySelectorAll('.tabs button')) b.onclick = () => switchTab(b.dataset.tab);
for (const id of ['views-filter', 'images-filter', 'params-filter']) $(`#${id}`).addEventListener('input', () => panels(true));
$('#b-view-new').onclick = run(newView);
$('#b-view-rename').onclick = run(renameView);
$('#b-view-delete').onclick = run(deleteView);
$('#b-page-add').onclick = run(addPage);
$('#b-param-add').onclick = run(addParam);
$('#b-image-import').onclick = run(async () => importImages(await pickFiles(true)));
$('#b-check').onclick = () => panels(true);
addEventListener('dragover', e => { if (e.dataTransfer?.types.includes('Files')) { e.preventDefault(); $('#left').classList.add('drop'); } });
addEventListener('dragleave', e => { if (!e.relatedTarget) $('#left').classList.remove('drop'); });
addEventListener('drop', e => {
  if (!e.dataTransfer?.types.includes('Files')) return;
  e.preventDefault();
  $('#left').classList.remove('drop');
  const files = [...(e.dataTransfer?.files || [])];
  if (files.length) run(importImages)(files);
});
// A clicked toolbar button keeps focus, so Space or Enter would press it again; give focus back.
addEventListener('click', e => { if (e.target.tagName === 'BUTTON' && !e.target.closest('dialog')) e.target.blur(); });
addEventListener('beforeunload', e => { if (!S.leaving && dirtyPaths().length) { e.preventDefault(); e.returnValue = ''; } });
function toggleOpt(id) { const el = $(`#${id}`); el.checked = !el.checked; el.dispatchEvent(new Event('change')); }
addEventListener('keydown', e => {
  const k = e.key, lk = k.length === 1 ? k.toLowerCase() : k, ctrl = e.ctrlKey || e.metaKey, t = e.target;
  if (ctrl && lk === 's') { e.preventDefault(); run(save)(); return; }
  if (ctrl && lk === 'f' && !document.querySelector('dialog[open]')) { e.preventDefault(); $('#find').focus(); $('#find').select(); return; }
  const typing = t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || (t.tagName === 'INPUT' && !['checkbox', 'button', 'color', 'file'].includes(t.type));
  if (typing || document.querySelector('dialog[open]')) return;
  if (S.playing && ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Enter', 'Escape', 'Delete'].includes(k) && !e.altKey && playEvent(pc => L.playKey(pc, k))) return e.preventDefault();
  const view = S.mode === 'view' && !!cur() && !S.playing; // editing shortcuts are off in Play mode
  if (ctrl) {
    if (S.playing) return;
    if (lk === 'z') e.shiftKey ? redo() : undo();
    else if (lk === 'y') redo();
    else if (view && lk === 'c') copy();
    else if (view && lk === 'x') { copy(); if (S.clip) S.clip.view = null; del(); }
    else if (view && lk === 'v') paste();
    else if (view && lk === 'd') duplicate();
    else if (view && lk === 'a') { S.sel = [...(cur().widgets || [])]; selChanged(); }
    else if (view && k === ']') reorder(1);
    else if (view && k === '[') reorder(-1);
    else if (view && k.startsWith('Arrow')) nudge(k, e.shiftKey ? 10 : 1, true);
    else return;
  } else if (e.altKey && k === 'ArrowLeft') back();
  else if (view && k.startsWith('Arrow')) nudge(k, e.shiftKey ? 10 : 1, false);
  else if (view && (k === 'Delete' || k === 'Backspace')) del();
  else if (k === 'Escape') { if (S.mode !== 'view') openView(S.view, false); else if (!S.playing) { S.sel = []; selChanged(); } }
  else if (lk === 'p') { if (S.playing || (S.mode === 'view' && cur())) setPlay(!S.playing); }
  else if (view && k === 'PageUp') layer(1);
  else if (view && k === 'PageDown') layer(-1);
  else if (view && k === 'Enter' && S.sel.length === 1) openTarget(S.sel[0]);
  else if (/^[1-6]$/.test(k)) setZoom(+k);
  else if (k === '+' || k === '=') setZoom(S.zoom + 1);
  else if (k === '-') setZoom(S.zoom - 1);
  else if (lk === 'f') fit();
  else if (lk === 'h') toggleOpt('show-hidden');
  else if (lk === 'b') toggleOpt('show-boxes');
  else if (k === '?') $('#help').showModal();
  else return;
  e.preventDefault();
});

run(load)();
