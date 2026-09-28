// Pure logic for the skin editor, shared by the browser (editor.js) and Node (test.mjs).
// No DOM here. Every drawing rule of docs/skin-format.md that the canvas needs is computed by
// these functions, so the tests check the same code the editor runs.

export const TYPES = ['plate', 'button', 'dropdown', 'dial', 'number', 'textbox', 'meter', 'plot', 'veil', 'list', 'tree', 'embed', 'custom'];
export const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
// skin.json "density": pixels per unit of the runtime's own geometry; dp(v, d) is a built-in size at it
// (rounded half away from zero, as the runtime rounds).
export const densityOf = docs => { const d = +docs?.['skin.json']?.density; return d > 0 ? d : 1; };
export const dp = (v, d = 1) => Math.sign(v) * Math.round(Math.abs(v) * d);
const isObj = v => v !== null && typeof v === 'object' && !Array.isArray(v);
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// ---- tiles, button states, nine-slice ----------------------------------------------------

// Tile i of n in a w x h strip: (0, i*h/n, w, h/n) on "y", (i*w/n, 0, w/n, h) on "x"; i clamped.
export function tileRect(w, h, tiles = 1, axis = 'y', i = 0) {
  const n = Math.max(1, Math.floor(tiles) || 1);
  i = clamp(Math.floor(i) || 0, 0, n - 1);
  return axis === 'x' ? [Math.floor(i * w / n), 0, Math.floor(w / n), h] : [0, Math.floor(i * h / n), w, Math.floor(h / n)];
}

// Tile shown by a dial or meter: round(norm * (tiles - 1)).
export const valueTile = (tiles, norm) => clamp(Math.round(clamp(norm, 0, 1) * (Math.max(1, tiles || 1) - 1)), 0, Math.max(1, tiles || 1) - 1);

// Top-left of a slider dial's handle (tile 0, tw x th) in rect [x, y, w, h].
export const sliderPos = ([x, y, w, h], tw, th, norm, vertical = true) => (vertical
  ? [x, y + (h - th) - Math.trunc(clamp(norm, 0, 1) * (h - th - 1))]
  : [x + Math.trunc(clamp(norm, 0, 1) * (w - tw - 1)), y]);

// Button state tile: groups [normal][pressed][hover] of `states` tiles each, then the disabled
// tile. Tile = min(value, states - 1) + states * group; group 1 while pressed (with pressedTiles),
// else 1 + pressedTiles while hovered (with hoverTiles), else 0.
export function buttonTile(w, tiles, value, { hover = false, pressed = false, disabled = false } = {}) {
  const b = x => (x ? 1 : 0), n = Math.max(1, Math.floor(tiles) || 1);
  const dis = b(w.disabledTile), ht = b(w.hoverTiles), pt = b(w.pressedTiles);
  if (disabled && dis) return n - 1;
  const states = Math.max(1, Math.floor((n - dis) / (1 + ht + pt)));
  const group = pressed && pt ? 1 : hover && ht ? 1 + pt : 0;
  return clamp(clamp(Math.round(value) || 0, 0, states - 1) + states * group, 0, n - 1);
}

// One axis of a nine-slice: [srcStart, srcEnd, dstStart, dstEnd] for the lead band, middle, trail band.
function bands(s, d, lead, trail) {
  lead = clamp(lead | 0, 0, s); trail = clamp(trail | 0, 0, s - lead);
  let dl = lead, dt = trail;
  // A rect smaller than both fixed bands shrinks them proportionally (the format does not say).
  if (dl + dt > d) { dl = Math.floor(d * lead / (lead + trail)); dt = d - dl; }
  return [[0, lead, 0, dl], [lead, s - trail, dl, d - dt], [s - trail, s, d - dt, d]];
}

// Nine-slice pieces for tile `src` [x,y,w,h] drawn into `dst` with slice [top,right,bottom,left]:
// a list of [srcRect, dstRect]; corners keep their size, edges and centre scale.
export function nineSlice(src, slice, dst) {
  const [sx, sy, sw, sh] = src, [dx, dy, dw, dh] = dst, [t, r, b, l] = slice;
  const out = [];
  for (const [y0, y1, v0, v1] of bands(sh, dh, t, b))
    for (const [x0, x1, u0, u1] of bands(sw, dw, l, r))
      if (x1 > x0 && y1 > y0 && u1 > u0 && v1 > v0) out.push([[sx + x0, sy + y0, x1 - x0, y1 - y0], [dx + u0, dy + v0, u1 - u0, v1 - v0]]);
  return out;
}

// How an image tile goes into a rect: nine-slice when sliced and the size differs, else 1:1 at
// the top-left clipped to the rect. Returns [srcRect, dstRect] pairs.
export function imagePieces(src, slice, dst) {
  if (slice && (src[2] !== dst[2] || src[3] !== dst[3])) return nineSlice(src, slice, dst);
  const w = Math.min(src[2], dst[2]), h = Math.min(src[3], dst[3]);
  return w > 0 && h > 0 ? [[[src[0], src[1], w, h], [dst[0], dst[1], w, h]]] : [];
}

// ---- surfaces (skin.json "surface"): as the runtime applies them (framework/src/core/draw.cpp) ------

// The two ramps, 0..256 per lightness level and per chroma level (max - min), from lightness [start,
// full] and chroma [start of the fade, gone], smoothstepped; the lightness ramp scaled by strength.
export function surfaceRamps(sf) {
  const ramp = (r, lo, hi, down) => {
    const a = (Array.isArray(r) && Number.isFinite(+r[0]) ? +r[0] : lo) * 255, b = (Array.isArray(r) && Number.isFinite(+r[1]) ? +r[1] : hi) * 255;
    return Int32Array.from({ length: 256 }, (_, i) => {
      let t = b > a ? clamp((i - a) / (b - a), 0, 1) : i >= a ? 1 : 0;
      t = t * t * (3 - 2 * t);
      return Math.round(256 * (down ? 1 - t : t));
    });
  };
  const k = clamp(Number.isFinite(+sf?.strength) ? +sf.strength : 1, 0, 1);
  return { weight: ramp(sf?.lightness, 0.7, 0.86, false).map(w => Math.round(w * k)), chroma: ramp(sf?.chroma, 0.06, 0.14, true) };
}
// The slope of a drawing's shading (lightness times opacity), x and y per pixel (-127..127), as the
// runtime works it out for each image: Sobel, a full slope 127, lighter below reading as tilted up.
export function surfaceSlope(px, w, h) {
  const lum = (x, y) => {
    const i = (Math.min(Math.max(y, 0), h - 1) * w + Math.min(Math.max(x, 0), w - 1)) * 4;
    return Math.floor(((77 * px[i] + 150 * px[i + 1] + 29 * px[i + 2]) >> 8) * px[i + 3] / 255);
  };
  const out = new Int8Array(w * h * 2);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const gx = lum(x + 1, y - 1) + 2 * lum(x + 1, y) + lum(x + 1, y + 1) - lum(x - 1, y - 1) - 2 * lum(x - 1, y) - lum(x - 1, y + 1);
      const gy = lum(x - 1, y + 1) + 2 * lum(x, y + 1) + lum(x + 1, y + 1) - lum(x - 1, y - 1) - 2 * lum(x, y - 1) - lum(x + 1, y - 1);
      out[2 * (y * w + x)] = clamp(Math.trunc(-gx / 8), -127, 127);
      out[2 * (y * w + x) + 1] = clamp(Math.trunc(-gy / 8), -127, 127);
    }
  }
  return out;
}
// Straight RGBA pixels (w x h, as from a canvas) drawn with their top-left at window point (x0, y0),
// through a surface texture tex ({ width, height, data }): a light grey pixel takes the texture's
// colour there times its own lightness, blended by the ramps. A reflecting surface (reflect: { slope,
// relief, normals, dents }) looks the texture up where the pixel's slope and the sheet's point. The
// runtime's integer arithmetic.
export function applySurface(px, w, h, x0, y0, tex, ramps, reflect = null) {
  const { weight, chroma } = ramps, tw = tex.width, th = tex.height, t = tex.data;
  const sl = reflect?.slope, n = reflect?.normals, rel = reflect?.relief | 0, dents = reflect?.dents | 0;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4, r = px[i], g = px[i + 1], b = px[i + 2];
      const l = (77 * r + 150 * g + 29 * b) >> 8, k = (weight[l] * chroma[Math.max(r, g, b) - Math.min(r, g, b)]) >> 8;
      if (!k) continue;
      let ex = x0 + x, ey = y0 + y;
      if (sl) { ex += Math.trunc(sl[2 * (y * w + x)] * rel / 128); ey += Math.trunc(sl[2 * (y * w + x) + 1] * rel / 128); }
      if (n) {
        const q = (((y0 + y) % n.height) * n.width + ((x0 + x) % n.width)) * 4;
        ex += Math.trunc((n.data[q] - 128) * dents / 128); ey += Math.trunc((n.data[q + 1] - 128) * dents / 128);
      }
      const j = ((((ey % th) + th) % th) * tw + (((ex % tw) + tw) % tw)) * 4;
      px[i] = r + Math.trunc(((Math.floor(t[j] * l / 255) - r) * k) / 256);
      px[i + 1] = g + Math.trunc(((Math.floor(t[j + 1] * l / 255) - g) * k) / 256);
      px[i + 2] = b + Math.trunc(((Math.floor(t[j + 2] * l / 255) - b) * k) / 256);
    }
  }
  return px;
}

// ---- RGBA images ({ width, height, data: Uint8Array }) -------------------------------------

export const blank = (width, height) => ({ width, height, data: new Uint8Array(width * height * 4) });

export function crop(img, [x, y, w, h]) {
  const out = blank(w, h);
  for (let r = 0; r < h; r++) out.data.set(img.data.subarray(((y + r) * img.width + x) * 4, ((y + r) * img.width + x + w) * 4), r * w * 4);
  return out;
}

export function blit(dst, src, dx, dy) {
  const w = Math.min(src.width, dst.width - dx), h = Math.min(src.height, dst.height - dy);
  for (let r = 0; r < h; r++) dst.data.set(src.data.subarray(r * src.width * 4, (r * src.width + w) * 4), ((dy + r) * dst.width + dx) * 4);
}

export function isEmpty(img) {
  for (let i = 3; i < img.data.length; i += 4) if (img.data[i]) return false;
  return true;
}

export const splitTiles = (img, n, axis = 'y') => Array.from({ length: Math.max(1, n | 0) }, (_, i) => crop(img, tileRect(img.width, img.height, n, axis, i)));

// Reassemble tiles into a strip; all tiles must have the same size.
export function joinTiles(tiles, axis = 'y') {
  if (!tiles.length) throw new Error('no tiles');
  const { width: w, height: h } = tiles[0];
  tiles.forEach((t, i) => { if (t.width !== w || t.height !== h) throw new Error(`tile ${i} is ${t.width}x${t.height}, expected ${w}x${h}`); });
  const x = axis === 'x', out = blank(x ? w * tiles.length : w, x ? h : h * tiles.length);
  tiles.forEach((t, i) => blit(out, t, x ? i * w : 0, x ? 0 : i * h));
  return out;
}

// ---- fonts ------------------------------------------------------------------------------------

// Glyph table of a font strip: { marker, y, h, glyphs: [[x, w] x 256] }. Row 0 with exactly 256
// non-transparent pixels is a marker row; otherwise glyphs are w/256 wide and h tall.
export function parseFont(img) {
  const { width: w, height: h, data } = img, marks = [];
  for (let x = 0; x < w; x++) if (data[x * 4 + 3]) marks.push(x);
  if (marks.length === 256) return { marker: true, y: 1, h: h - 1, glyphs: marks.map((x, i) => [x, (i < 255 ? marks[i + 1] : w) - x]) };
  const gw = Math.floor(w / 256);
  return { marker: false, y: 0, h, glyphs: Array.from({ length: 256 }, (_, i) => [i * gw, gw]) };
}

// Latin-1 codes of a line; code points above 255 become '?'.
export const textCodes = s => [...s].map(c => { const k = c.codePointAt(0); return k > 255 ? 63 : k; });

// Place text in rect shrunk by pad [l,t,r,b]. The two characters \n break lines. Returns glyph
// blits { sx, sy, w, h, dx, dy }. Centring truncates toward zero like C++ integer division.
export function layoutText(font, str, rect, style = {}, pad) {
  const [pl, pt, pr, pb] = pad || [0, 0, 0, 0];
  const x0 = rect[0] + pl, y0 = rect[1] + pt, w = rect[2] - pl - pr, h = rect[3] - pt - pb;
  const lines = String(str ?? '').split('\\n').map(textCodes), lh = font.h, total = lines.length * lh;
  let y = style.valign === 'middle' ? y0 + Math.trunc((h - total) / 2) : style.valign === 'bottom' ? y0 + h - total : y0;
  const out = [];
  for (const codes of lines) {
    const lw = codes.reduce((s, c) => s + font.glyphs[c][1], 0);
    let x = style.align === 'center' ? x0 + Math.trunc((w - lw) / 2) : style.align === 'right' ? x0 + w - lw : x0;
    for (const c of codes) {
      const [gx, gw] = font.glyphs[c];
      if (gw > 0 && lh > 0) out.push({ code: c, sx: gx, sy: font.y, w: gw, h: lh, dx: x, dy: y });
      x += gw;
    }
    y += lh;
  }
  return out;
}

export const fontGlyphs = (img, font = parseFont(img)) => font.glyphs.map(([x, w]) => crop(img, [x, font.y, w, font.h]));

// A glyph resized to at least 1 px wide and exactly h tall (cropped or padded at the bottom).
export function fitGlyph(img, h) {
  if (img.height === h && img.width > 0) return img;
  const out = blank(Math.max(1, img.width), h);
  blit(out, crop(img, [0, 0, img.width, Math.min(h, img.height)]), 0, 0);
  return out;
}

// Build a font strip from 256 glyph images of equal height, with a fresh marker row.
export function buildFont(glyphs) {
  if (glyphs.length !== 256) throw new Error('a font needs 256 glyphs');
  const h = glyphs[0].height;
  glyphs.forEach((g, i) => {
    if (g.height !== h) throw new Error(`glyph ${i} is ${g.height} px tall, expected ${h}`);
    if (g.width < 1) throw new Error(`glyph ${i} has no width`);
  });
  const out = blank(glyphs.reduce((s, g) => s + g.width, 0), h + 1);
  let x = 0;
  for (const g of glyphs) { out.data.set([255, 0, 255, 255], x * 4); blit(out, g, x, 1); x += g.width; }
  return out;
}

// ---- PNG (8-bit RGBA out; reads grey, RGB, palette, alpha, 1..16 bit, not interlaced) --------

const CRC = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
function crc32(b, start, end) { let c = 0xffffffff; for (let i = start; i < end; i++) c = CRC[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
async function zlib(bytes, inflate) {
  const s = new Blob([bytes]).stream().pipeThrough(inflate ? new DecompressionStream('deflate') : new CompressionStream('deflate'));
  return new Uint8Array(await new Response(s).arrayBuffer());
}
const paeth = (a, b, c) => { const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c); return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; };

export async function pngDecode(bytes) {
  const u = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes), dv = new DataView(u.buffer, u.byteOffset, u.byteLength);
  if (u[0] !== 137 || u[1] !== 80 || u[2] !== 78 || u[3] !== 71) throw new Error('not a PNG');
  let pos = 8, w, h, depth, ctype, lace, plte, trns;
  const idat = [];
  while (pos + 8 <= u.length) {
    const len = dv.getUint32(pos), type = String.fromCharCode(...u.subarray(pos + 4, pos + 8)), d = u.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') { w = dv.getUint32(pos + 8); h = dv.getUint32(pos + 12); depth = d[8]; ctype = d[9]; lace = d[12]; }
    else if (type === 'PLTE') plte = d;
    else if (type === 'tRNS') trns = d;
    else if (type === 'IDAT') idat.push(d);
    else if (type === 'IEND') break;
    pos += 12 + len;
  }
  if (lace) throw new Error('interlaced PNG'); // the editor falls back to the browser decoder
  const ch = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[ctype];
  if (!ch || !w || !h) throw new Error('unsupported PNG');
  const bpp = Math.max(1, (ch * depth) >> 3), stride = (w * ch * depth + 7) >> 3;
  const all = new Uint8Array(idat.reduce((s, d) => s + d.length, 0));
  idat.reduce((o, d) => (all.set(d, o), o + d.length), 0);
  const raw = await zlib(all, true), px = new Uint8Array(stride * h);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], s = y * (stride + 1) + 1, o = y * stride, p = o - stride;
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? px[o + i - bpp] : 0, b = y ? px[p + i] : 0, c = y && i >= bpp ? px[p + i - bpp] : 0;
      px[o + i] = raw[s + i] + (f === 1 ? a : f === 2 ? b : f === 3 ? (a + b) >> 1 : f === 4 ? paeth(a, b, c) : 0);
    }
  }
  const smp = (row, i) => depth === 8 ? px[row + i] : depth === 16 ? (px[row + 2 * i] << 8) | px[row + 2 * i + 1]
    : (px[row + ((i * depth) >> 3)] >> (8 - depth - ((i * depth) & 7))) & ((1 << depth) - 1);
  const to8 = v => depth === 16 ? v >> 8 : depth < 8 ? Math.round(v * 255 / ((1 << depth) - 1)) : v;
  const key = trns && (ctype === 0 || ctype === 2) ? Array.from({ length: ctype ? 3 : 1 }, (_, i) => (trns[2 * i] << 8) | trns[2 * i + 1]) : null;
  const out = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    const row = y * stride;
    for (let x = 0; x < w; x++) {
      const k = (y * w + x) * 4;
      let r, g, b, a = 255;
      if (ctype === 3) { const i = smp(row, x); r = plte[i * 3]; g = plte[i * 3 + 1]; b = plte[i * 3 + 2]; if (trns && i < trns.length) a = trns[i]; }
      else if (ctype === 0 || ctype === 4) {
        const v = smp(row, x * (ch)); r = g = b = to8(v);
        if (ctype === 4) a = to8(smp(row, x * 2 + 1)); else if (key && v === key[0]) a = 0;
      } else {
        const s = [0, 1, 2].map(i => smp(row, x * ch + i));
        [r, g, b] = s.map(to8);
        if (ctype === 6) a = to8(smp(row, x * 4 + 3)); else if (key && s.every((v, i) => v === key[i])) a = 0;
      }
      out[k] = r; out[k + 1] = g; out[k + 2] = b; out[k + 3] = a;
    }
  }
  return { width: w, height: h, data: out };
}

function chunk(type, data) {
  const out = new Uint8Array(12 + data.length), dv = new DataView(out.buffer);
  dv.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  dv.setUint32(8 + data.length, crc32(out, 4, 8 + data.length));
  return out;
}

// Encode 8-bit RGBA, choosing each row's filter by the smallest sum of absolute differences.
export async function pngEncode(img) {
  const { width: w, height: h, data } = img, stride = w * 4, raw = new Uint8Array((stride + 1) * h);
  const tmp = new Uint8Array(stride), best = new Uint8Array(stride);
  for (let y = 0; y < h; y++) {
    const o = y * stride;
    let bestSum = Infinity, bestF = 0;
    for (let f = 0; f < 5; f++) {
      let sum = 0;
      for (let i = 0; i < stride; i++) {
        const a = i >= 4 ? data[o + i - 4] : 0, b = y ? data[o - stride + i] : 0, c = y && i >= 4 ? data[o - stride + i - 4] : 0;
        const v = (data[o + i] - (f === 1 ? a : f === 2 ? b : f === 3 ? (a + b) >> 1 : f === 4 ? paeth(a, b, c) : 0)) & 255;
        tmp[i] = v; sum += v < 128 ? v : 256 - v;
      }
      if (sum < bestSum) { bestSum = sum; bestF = f; best.set(tmp); }
    }
    raw[y * (stride + 1)] = bestF;
    raw.set(best, y * (stride + 1) + 1);
  }
  const ihdr = new Uint8Array(13), dv = new DataView(ihdr.buffer);
  dv.setUint32(0, w); dv.setUint32(4, h); ihdr[8] = 8; ihdr[9] = 6;
  const parts = [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', await zlib(raw, false)), chunk('IEND', new Uint8Array(0))];
  const out = new Uint8Array(parts.reduce((s, p) => s + p.length, 0));
  parts.reduce((o, p) => (out.set(p, o), o + p.length), 0);
  return out;
}

// ---- values and display ---------------------------------------------------------------------

export const cssColour = c => {
  const m = /^#([0-9a-f]{6})([0-9a-f]{2})?$/i.exec(c || '');
  if (!m) return null;
  const n = parseInt(m[1], 16);
  return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${m[2] ? parseInt(m[2], 16) / 255 : 1})`;
};

// {name} is replaced by the var, {name:upper} by the var in upper case; unknown names stay as written.
export const resolveVars = (s, vars) => String(s ?? '').replace(/\{([^}:]+)(:upper)?\}/g, (a, k, up) => vars?.[k] == null ? a : up ? String(vars[k]).toUpperCase() : String(vars[k]));

// Keys of a custom "piano" as the runtime lays them out: `count` keys from MIDI note `first`, white
// keys first (evenly spread), then the black keys over them, 0.7 of a white key wide, 0.59 tall
// and shifted per note to match the usual key art. Hit testing goes from the end (black first).
const BLACK_SHIFT = [0, -0.057, 0, 0.128, 0, 0, -0.093, 0, 0.038, 0, 0.116, 0];
const pc12 = n => ((n % 12) + 12) % 12;
export const isBlackKey = n => [1, 3, 6, 8, 10].includes(pc12(n));
// With key images (blackTile = the black key image's tile size) the keys sit where the key art puts
// them, as in the runtime: a 130 px octave, white keys at 0, 19, 38, 56, 75, 94, 112 and black keys
// (the black image's size) at 11, 33, 67, 87, 107, all times the skin's density d.
const WHITE_AT = [0, -1, 19, -1, 38, 56, -1, 75, -1, 94, -1, 112], BLACK_AT = [-1, 11, -1, 33, -1, -1, 67, -1, 87, -1, 107, -1];
export function pianoKeys(first, count, [x, y, w, h], blackTile, d = 1) {
  first |= 0; count = clamp(count | 0, 0, 128);
  if (blackTile) {
    const base = first - pc12(first), shift = Math.max(0, dp(WHITE_AT[pc12(first)], d)), whites = [], blacks = [];
    for (let k = 0; k < count; k++) {
      const note = first + k, pc = pc12(note), ox = x + Math.trunc((note - pc - base) / 12) * dp(130, d) - shift;
      if (isBlackKey(note)) blacks.push({ note, black: true, rect: [ox + dp(BLACK_AT[pc], d), y, blackTile[0], blackTile[1]] });
      else {
        const at = dp(WHITE_AT[pc], d), next = dp(pc === 11 ? 130 : WHITE_AT[pc + 1 + (WHITE_AT[pc + 1] < 0 ? 1 : 0)], d);
        whites.push({ note, black: false, rect: [ox + at, y, next - at, h] });
      }
    }
    return [...whites, ...blacks];
  }
  let whites = 0;
  for (let k = 0; k < count; k++) whites += !isBlackKey(first + k);
  const kw = whites ? w / whites : 0, keys = [], blacks = [];
  for (let k = 0, i = 0; k < count && whites; k++) {
    const note = first + k, edge = x + Math.round(i * kw);
    if (isBlackKey(note)) {
      const bw = Math.max(1, Math.round(kw * 0.7)), cx = x + Math.round((i + BLACK_SHIFT[pc12(note)]) * kw);
      blacks.push({ note, black: true, rect: [cx - (bw >> 1), y, bw, Math.round(h * 0.59)] });
    } else { i++; keys.push({ note, black: false, rect: [edge, y, x + Math.round(i * kw) - edge, h] }); }
  }
  return [...keys, ...blacks];
}
// Velocity of a click on a key: higher on the key is softer (1 at the top, 127 at the bottom).
export const pianoVelocity = ([, ky, , kh], y) => clamp(Math.round(1 + 126 * (y - ky) / Math.max(1, kh - 1)), 1, 127);
// Size of a pad handle (the runtime's rule: 14 px, a quarter of a small pad).
export const padHandle = ([, , w, h], d = 1) => Math.max(dp(4, d), Math.min(dp(14, d), Math.floor(Math.min(w, h) / 4)));

// Plain value to normalized 0..1, honouring a "log" taper (min, max > 0).
export function toNorm(v, min, max, taper) {
  if (max === min) return 0;
  if (taper === 'log' && min > 0 && max > 0 && v > 0) return clamp(Math.log(v / min) / Math.log(max / min), 0, 1);
  return clamp((v - min) / (max - min), 0, 1);
}
export const fromNorm = (n, min, max, taper) => taper === 'log' && min > 0 && max > 0 ? min * (max / min) ** n : min + n * (max - min);

// The value a widget shows in the editor: its param's default, or its local `value`.
export function widgetValue(w, p) {
  const min = p ? p.min ?? 0 : w.range?.[0] ?? 0, max = p ? p.max ?? 1 : w.range?.[1] ?? 1;
  const steps = (p ? p.steps : w.steps) ?? 0, v = p ? p.default ?? min : w.value ?? 0;
  const norm = toNorm(v, min, max, p?.taper);
  return { v, min, max, steps, norm, index: p ? Math.round(norm * (steps || 1)) : Math.round(v) };
}

// printf with one conversion for a double (%d %i %u %f %e %g %s, flags -+ 0#, width, precision).
export function formatValue(fmt, v) {
  let used = false;
  return String(fmt).replace(/%([-+ 0#]*)(\d*)(?:\.(\d+))?([diufFeEgGs%])/g, (all, flags, width, prec, conv) => {
    if (conv === '%') return '%';
    if (used) return all;
    used = true;
    if (conv === 's') return String(v).padStart(+width || 0);
    const p = prec === undefined ? 6 : +prec, a = Math.abs(v);
    const e = x => x.replace(/e([+-])(\d)$/, 'e$10$2');
    let s;
    if ('diu'.includes(conv)) s = String(Math.round(a));
    else if ('fF'.includes(conv)) {
      // C rounds an exact binary tie (0.25 at %.1f) to even; toFixed rounds it up. 30 more digits
      // of the exact expansion tell a true tie from a near one (0.45 is really 0.4500000000000000111).
      const ext = a.toFixed(Math.min(100, p + 30)), cut = ext.slice(0, -30).replace(/\.$/, '');
      s = /^50*$/.test(ext.slice(-30)) && +cut.slice(-1) % 2 === 0 ? cut : a.toFixed(p);
    }
    else if ('eE'.includes(conv)) s = e(a.toExponential(p));
    else {
      const P = p || 1, X = a === 0 ? 0 : +a.toExponential(P - 1).split('e')[1];
      s = X < -4 || X >= P ? e(a.toExponential(P - 1)) : a.toFixed(Math.max(0, P - 1 - X));
      if (!flags.includes('#')) s = s.replace(/\.?0+(?=e|$)/, m => (s.includes('.') ? '' : m));
    }
    if ('EG'.includes(conv)) s = s.toUpperCase();
    const sign = v < 0 ? '-' : flags.includes('+') ? '+' : flags.includes(' ') ? ' ' : '';
    const n = +width || 0;
    if (flags.includes('-')) return (sign + s).padEnd(n);
    if (flags.includes('0')) return sign + s.padStart(n - sign.length, '0');
    return (sign + s).padStart(n);
  });
}

// A number widget's text for plain value v (default: the value it starts with). As in the runtime,
// labels apply only when the widget has no format of its own.
export function displayText(w, p, v) {
  const d = widgetValue(w, p);
  if (v === undefined) v = d.v;
  const norm = toNorm(v, d.min, d.max, p?.taper);
  if (!w.format && p?.labels && d.steps) { const l = p.labels[Math.round(norm * d.steps)]; if (l != null) return String(l); }
  return formatValue(w.format || p?.format || (d.steps ? '%.0f' : '%.2f'), v);
}
// "Name: value unit" for status displays.
export const statusText = (p, v) => `${p.name ?? p.id}: ${displayText({}, p, v)}${p.unit ? ' ' + p.unit : ''}`;

// Text typed into an editable number, parsed as the runtime does: a label (any case), else the
// number at the start (units and other text after it are ignored). Plain value, or null.
export function parseValueText(s, p) {
  s = String(s ?? '').trim();
  const steps = p?.steps | 0;
  if (p?.labels && steps) {
    const k = p.labels.findIndex(l => String(l).toLowerCase() === s.toLowerCase());
    if (k >= 0) return fromNorm(k / steps, p.min ?? 0, p.max ?? 1, p.taper);
  }
  const m = /^[-+]?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?/i.exec(s);
  return m ? +m[0] : null;
}

// ---- small parsers for inspector fields -------------------------------------------------------

export function parseVars(text) {
  const out = {};
  for (const part of String(text).split(',')) {
    const k = part.indexOf('=');
    if (k > 0) out[part.slice(0, k).trim()] = part.slice(k + 1).trim();
  }
  return Object.keys(out).length ? out : undefined;
}
export const formatVars = o => Object.entries(o || {}).map(([k, v]) => `${k}=${v}`).join(', ');

// Dropdown items, one per line: "label = value", or "-" for a separator. Extra fields of the
// item at the same position survive (positional, so inserting a line shifts them).
export function parseItems(text, old = [], start = 0) {
  let next = start;
  const items = String(text).split('\n').map(s => s.trim()).filter(Boolean).map((s, i) => {
    if (/^-+$/.test(s)) return { separator: true };
    const k = s.lastIndexOf('='), tail = k < 0 ? '' : s.slice(k + 1).trim();
    const value = tail !== '' && Number.isFinite(+tail) ? +tail : next;
    next = value + 1;
    return { ...(old[i] && !old[i].separator ? old[i] : {}), label: k < 0 || tail === '' || !Number.isFinite(+tail) ? s : s.slice(0, k).trim(), value };
  });
  return items.length ? items : undefined;
}
export const formatItems = items => (items || []).map(it => it.separator ? '-' : `${it.label ?? ''} = ${it.value ?? 0}`).join('\n');

// List columns, one per line: "width [left|center|right] [int]"; other fields of the column at the
// same position survive. List rows, one per line, cells separated by "|".
export function parseColumns(text, old = []) {
  const cols = String(text).split('\n').map(s => s.trim()).filter(Boolean).map((line, i) => {
    const c = { ...(isObj(old[i]) ? old[i] : {}) };
    delete c.width; delete c.align; delete c.edit;
    for (const t of line.split(/\s+/)) {
      if (/^\d+$/.test(t)) c.width = +t;
      else if (['left', 'center', 'right'].includes(t)) c.align = t;
      else if (t === 'int') c.edit = 'int';
    }
    return c;
  });
  return cols.length ? cols : undefined;
}
export const formatColumns = cols => (Array.isArray(cols) ? cols : []).map(c => [c.width, c.align, c.edit].filter(x => x != null).join(' ')).join('\n');
export function parseRows(text) {
  const rows = String(text).split('\n').filter(s => s.trim()).map(s => s.split('|').map(c => c.trim()));
  return rows.length ? rows : undefined;
}
export const formatRows = rows => (Array.isArray(rows) ? rows : []).map(r => (Array.isArray(r) ? r : [r]).join(' | ')).join('\n');

// Dotted-path access. Setting undefined, '', [] or the default removes the key and empty parents.
export const getPath = (o, path) => path.split('.').reduce((a, k) => a?.[k], o);
export function setPath(o, path, v, def) {
  const ks = path.split('.'), last = ks.pop(), chain = [o];
  const gone = v === undefined || v === '' || (Array.isArray(v) && !v.length) || (def !== undefined && same(v, def));
  for (const k of ks) {
    const t = chain[chain.length - 1];
    if (!isObj(t[k])) { if (gone) return; t[k] = {}; }
    chain.push(t[k]);
  }
  if (!gone) { chain[chain.length - 1][last] = v; return; }
  delete chain[chain.length - 1][last];
  for (let i = ks.length - 1; i >= 0 && !Object.keys(chain[i + 1]).length; i--) delete chain[i][ks[i]];
}

export function uniqueName(base, taken) {
  if (!taken.has(base)) return base;
  const stem = base.replace(/_\d+$/, '');
  for (let i = 2; ; i++) if (!taken.has(`${stem}_${i}`)) return `${stem}_${i}`;
}
export const snake = s => String(s).toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'untitled';
export const validName = s => /^[a-z0-9_]+$/.test(s);
export const validPath = s => /^[a-z0-9_]+(\/[a-z0-9_]+)*$/.test(s);

// ---- layout helpers -----------------------------------------------------------------------------

// Widgets in draw order: ascending layer (default 3), file order within a layer. `get` picks the
// widget out of a wrapper (play-tree items).
export const drawOrder = (ws, get = x => x) => ws.map((w, i) => [w, i]).sort((a, b) => (get(a[0]).layer ?? 3) - (get(b[0]).layer ?? 3) || a[1] - b[1]).map(e => e[0]);

// Align rects to their common bounds, or match the first one's width or height.
export function align(rects, how) {
  const L = Math.min(...rects.map(r => r[0])), T = Math.min(...rects.map(r => r[1]));
  const R = Math.max(...rects.map(r => r[0] + r[2])), B = Math.max(...rects.map(r => r[1] + r[3]));
  return rects.map(([x, y, w, h]) => {
    if (how === 'left') x = L; else if (how === 'right') x = R - w; else if (how === 'hcentre') x = Math.floor((L + R - w) / 2);
    else if (how === 'top') y = T; else if (how === 'bottom') y = B - h; else if (how === 'vcentre') y = Math.floor((T + B - h) / 2);
    else if (how === 'width') w = rects[0][2]; else if (how === 'height') h = rects[0][3];
    return [x, y, w, h];
  });
}

// Equal gaps along an axis ('x' or 'y'); the first and last rect stay in place.
export function distribute(rects, axis) {
  const i = axis === 'x' ? 0 : 1, order = rects.map((_, k) => k).sort((a, b) => rects[a][i] - rects[b][i]);
  const first = rects[order[0]], last = rects[order[order.length - 1]];
  const gap = (last[i] + last[i + 2] - first[i] - rects.reduce((s, r) => s + r[i + 2], 0)) / Math.max(1, rects.length - 1);
  const out = rects.map(r => r.slice());
  let pos = first[i];
  for (const k of order) { out[k][i] = Math.round(pos); pos += rects[k][i + 2] + gap; }
  return out;
}

// Closest snap: the smallest target - candidate within thr, or null.
export function snapDelta(cands, targets, thr) {
  let best = null;
  for (const c of cands) for (const t of targets) { const d = t - c; if (Math.abs(d) <= thr && (!best || Math.abs(d) < Math.abs(best.d))) best = { d, at: t }; }
  return best;
}

// ---- skin model: references, renaming, validation -------------------------------------------------

export const viewNames = docs => Object.keys(docs).filter(p => /^views\/[^/]+\.json$/.test(p)).map(p => p.slice(6, -5)).sort();
export const viewPath = name => `views/${name}.json`;

// Every by-name reference in a skin: { kind: image|font|view|param|stack|widget, name, path, widget, set(newName) }.
// Image metadata entries in skin.json carry meta: true (they describe the image, they do not use it).
export function refs(docs) {
  const out = [];
  const add = (kind, path, widget, obj, key) => {
    const v = obj?.[key];
    if (typeof v === 'string' && v) out.push({ kind, name: v, path, widget, set: n => { obj[key] = n; } });
  };
  const skin = docs['skin.json'];
  if (skin) {
    add('view', 'skin.json', null, skin, 'root');
    for (const k of Object.keys(skin.images || {}))
      out.push({ kind: 'image', name: k, path: 'skin.json', widget: null, meta: true, set: n => {
        const e = Object.entries(skin.images); skin.images = Object.fromEntries(e.map(([a, v]) => [a === k ? n : a, v]));
      } });
    for (const m of [skin.menu, ...Object.values(isObj(skin.menu?.styles) ? skin.menu.styles : {})]) {
      for (const k of ['font', 'hoverFont', 'disabledFont']) add('font', 'skin.json', null, m, k);
      for (const k of ['check', 'arrow']) add('image', 'skin.json', null, m, k);
    }
    add('font', 'skin.json', null, skin.tooltip, 'font');
    add('param', 'skin.json', null, skin.learn, 'param');
    for (const k of Object.keys(isObj(skin.fonts) ? skin.fonts : {}))
      out.push({ kind: 'font', name: k, path: 'skin.json', widget: null, meta: true, set: n => {
        skin.fonts = Object.fromEntries(Object.entries(skin.fonts).map(([a, v]) => [a === k ? n : a, v]));
      } });
  }
  for (const name of viewNames(docs)) {
    const path = viewPath(name), v = docs[path];
    add('image', path, null, v.fill, 'image');
    for (const w of v.widgets || []) {
      const wn = w.name ?? null;
      add('image', path, wn, w.fill, 'image');
      add('image', path, wn, w, 'image');
      for (const k of ['text', 'header', 'row']) add('font', path, wn, w[k], 'font');
      if (w.type === 'embed') add('view', path, wn, w, 'view');
      add('param', path, wn, w, 'param');
      if (Array.isArray(w.params)) w.params.forEach((_, i) => add('param', path, wn, w.params, i));
      const cond = (holder, k) => { // conditions, all/any nested
        const c = holder?.[k];
        if (typeof c === 'string') return add('param', path, wn, holder, k);
        if (!isObj(c)) return;
        for (const j of ['all', 'any']) if (Array.isArray(c[j])) c[j].forEach((_, i) => cond(c[j], i));
        add('param', path, wn, c, 'param');
        if (c.stack != null) { add('stack', path, wn, c, 'stack'); add('view', path, wn, c, 'view'); }
      };
      cond(w, 'showIf'); cond(w, 'enableIf');
      if (Array.isArray(w.mirror)) w.mirror.forEach((_, i) => add('param', path, wn, w.mirror, i));
      if (isObj(w.keyImages)) for (const k of Object.keys(w.keyImages)) add('image', path, wn, w.keyImages, k);
      add('image', path, wn, w.scroll, 'track');
      add('image', path, wn, w.scroll, 'thumb');
      const a = w.action;
      if (isObj(a)) {
        add('view', path, wn, a, 'goto');
        if (a.goto) add('stack', path, wn, a, 'stack');
        add('widget', path, wn, a, 'toggle');
      }
    }
  }
  return out;
}

export function renameRefs(docs, kind, from, to) {
  const hits = refs(docs).filter(r => r.kind === kind && r.name === from);
  hits.forEach(r => r.set(to));
  return hits.length;
}

// Matcher for a param template such as "op.{op}.ratio".
export function paramMatcher(id) {
  if (!id.includes('{')) return x => x === id;
  const re = new RegExp('^' + id.replace(/[.*+?^$()|[\]\\]/g, '\\$&').replace(/\{[^}]*\}/g, '[^.]+') + '$');
  return x => re.test(x);
}

// Problems in a skin: broken references, duplicate names, widgets outside their view.
// have = { images: Set, fonts: Set } of names that exist as files.
export function validate(docs, have = {}) {
  const issues = [], add = (path, widget, msg) => issues.push({ path, widget, msg });
  const views = viewNames(docs), vset = new Set(views), ids = new Set(), stacks = new Set(), wnames = new Set();
  if (!docs['skin.json']) add('skin.json', null, 'no skin.json');
  for (const p of docs['params.json'] || []) {
    if (!p.id) add('params.json', null, `param without id (${p.name ?? '?'})`);
    else if (ids.has(p.id)) add('params.json', null, `duplicate param id "${p.id}"`);
    else ids.add(p.id);
    if (p.taper === 'log' && !((p.min ?? 0) > 0 && (p.max ?? 1) > 0)) add('params.json', null, `log taper needs min and max > 0 ("${p.id}")`);
  }
  for (const n of views) for (const w of docs[viewPath(n)].widgets || []) { if (w.name) wnames.add(w.name); if (w.type === 'embed' && w.name) stacks.add(w.name); }
  const idList = [...ids];
  const exists = {
    image: n => have.images?.has(n), font: n => have.fonts?.has(n), view: n => vset.has(n),
    param: n => ids.has(n) || (n.includes('{') && idList.some(paramMatcher(n))), stack: n => stacks.has(n), widget: n => wnames.has(n),
  };
  for (const r of refs(docs)) if (!exists[r.kind](r.name)) add(r.path, r.widget, `missing ${r.kind} "${r.name}"`);
  for (const n of views) {
    const path = viewPath(n), v = docs[path], size = Array.isArray(v.size) ? v.size : null, seen = new Set();
    if (!size) add(path, null, 'view has no size');
    if (v.flow != null && v.flow !== 'column') add(path, null, `unknown flow "${v.flow}"`);
    for (const w of v.widgets || []) {
      if (!TYPES.includes(w.type)) add(path, w.name ?? null, `unknown type "${w.type}"`);
      if (!w.name) add(path, null, `${w.type || 'widget'} without a name`);
      else if (seen.has(w.name)) add(path, w.name, `duplicate name "${w.name}"`);
      else seen.add(w.name);
      const r = w.rect;
      if (!Array.isArray(r) || r.length !== 4) add(path, w.name ?? null, 'rect is not [x, y, w, h]');
      else if (size && (r[0] < 0 || r[1] < 0 || r[0] + r[2] > size[0] || r[1] + r[3] > size[1])) add(path, w.name ?? null, 'outside its view');
    }
  }
  return issues;
}

// First embed with this name, searched from the root view down (then anywhere): { view, widget }.
export function findEmbed(docs, root, name) {
  const seen = new Set(), queue = [root];
  while (queue.length) {
    const vn = queue.shift();
    if (seen.has(vn)) continue;
    seen.add(vn);
    for (const w of docs[viewPath(vn)]?.widgets || []) if (w.type === 'embed') {
      if (w.name === name) return { view: vn, widget: w };
      if (w.view) queue.push(w.view);
    }
  }
  for (const vn of viewNames(docs)) if (!seen.has(vn)) for (const w of docs[viewPath(vn)].widgets || []) if (w.type === 'embed' && w.name === name) return { view: vn, widget: w };
  return null;
}

// Where each view shows as the runtime reaches it from the root, breadth first: view -> { view, via }
// (the view holding the embed that shows it, and that embed's name). An embed shows its own view, and
// a goto shows its target in the embed named by its stack.
export function viewParents(docs) {
  const root = docs['skin.json']?.root, parents = {}, seen = new Set([root]), queue = [root], stacks = new Map();
  const stackOf = name => { if (!stacks.has(name)) stacks.set(name, findEmbed(docs, root, name)); return stacks.get(name); };
  while (queue.length) {
    const v = queue.shift();
    for (const w of docs[viewPath(v)]?.widgets || []) {
      const kids = [];
      if (w?.type === 'embed' && typeof w.view === 'string') kids.push([w.view, v, w.name]);
      if (typeof w?.action?.goto === 'string') { const e = stackOf(w.action.stack); kids.push([w.action.goto, e ? e.view : v, e ? e.widget.name : w.action.stack]); }
      for (const [c, pv, via] of kids) if (!seen.has(c) && isObj(docs[viewPath(c)])) { seen.add(c); parents[c] = { view: pv, via }; queue.push(c); }
    }
  }
  return parents;
}
// The views from the root down to v (just [v] for a view nothing shows).
export function viewChain(parents, v) {
  const out = [v];
  for (let p = parents[v]; p && !out.includes(p.view); p = parents[p.view]) out.unshift(p.view);
  return out;
}

// Stack names each view is shown on: goto targets, plus the initial view of an embed used as a stack.
export function viewTags(docs) {
  const tags = {}, stacks = new Set(), all = viewNames(docs).flatMap(n => docs[viewPath(n)].widgets || []);
  const tag = (v, t) => (tags[v] ||= new Set()).add(t);
  for (const w of all) if (w.action?.goto) { tag(w.action.goto, w.action.stack || '?'); stacks.add(w.action.stack); }
  for (const w of all) if (w.type === 'embed' && stacks.has(w.name) && w.view) tag(w.view, w.name);
  return tags;
}

// ---- parallel work ----------------------------------------------------------------------------------

// Runs fn(item, i) over items with at most `limit` calls in flight. Resolves to the results in
// order, or rejects with the first error once the running calls have finished. progress(done,
// total) runs after each call.
export async function pool(items, limit, fn, progress) {
  const out = new Array(items.length);
  let next = 0, done = 0, err = null;
  const worker = async () => {
    while (next < items.length && !err) {
      const i = next++;
      try { out[i] = await fn(items[i], i); } catch (e) { err ||= e; }
      progress?.(++done, items.length);
    }
  };
  await Promise.all(Array.from({ length: Math.max(1, Math.min(limit, items.length)) }, worker));
  if (err) throw err;
  return out;
}

// ---- conditions, flow, scrolling, lists, menus and tooltips --------------------------------------------

export const validRect = r => Array.isArray(r) && r.length === 4 && r.every(Number.isFinite);
export const contains = (r, x, y) => !!r && x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
export function meet(a, b) {
  const x = Math.max(a[0], b[0]), y = Math.max(a[1], b[1]);
  return [x, y, Math.max(0, Math.min(a[0] + a[2], b[0] + b[2]) - x), Math.max(0, Math.min(a[1] + a[3], b[1] + b[3]) - y)];
}
// Width of the widest line of a text in a font (lines break at the two characters \n).
export const textWidth = (font, s) => Math.max(0, ...String(s ?? '').split('\\n').map(l => textCodes(l).reduce((n, c) => n + font.glyphs[c][1], 0)));

const labelAt = (p, v) => p.labels?.[Math.round(toNorm(v, p.min ?? 0, p.max ?? 1, p.taper) * (p.steps | 0))];
const sameVars = (want, have) => Object.entries(isObj(want) ? want : {}).every(([k, v]) => String(have?.[k]) === String(v));
// A showIf / enableIf condition. env.param(id) gives { v, p } for a param that exists (else null),
// env.variable(name) a var's value, env.stack(name) what that embed shows ({ view, vars } or null),
// env.sub(s) resolves {var} placeholders. A condition on a param that does not exist counts as
// true, so a typo never hides a widget (the Check tab reports it).
export function evalCondition(c, env) {
  if (typeof c === 'string') c = { param: c };
  if (!isObj(c)) return true;
  if (Array.isArray(c.all)) return c.all.every(x => evalCondition(x, env));
  if (Array.isArray(c.any)) return c.any.some(x => evalCondition(x, env));
  if ('stack' in c) {
    if (!env.stack) return true;
    const s = env.stack(String(c.stack));
    return !!s && s.view === c.view && sameVars(c.vars, s.vars);
  }
  if ('var' in c) {
    const v = env.variable(String(c.var));
    return 'equals' in c ? String(v) === String(c.equals) : 'notEquals' in c ? String(v) !== String(c.notEquals) : v != null && v !== '';
  }
  const q = c.param ? env.param(env.sub(String(c.param))) : null;
  if (!q) return true;
  const { v, p } = q, is = x => (typeof x === 'string' ? labelAt(p, v) === x : Math.abs(v - x) < 1e-9);
  if ('atLeast' in c) return v >= +c.atLeast - 1e-9;
  if ('atMost' in c) return v <= +c.atMost + 1e-9;
  return 'equals' in c ? is(c.equals) : 'notEquals' in c ? !is(c.notEquals) : Math.abs(v - (p.min ?? 0)) > 1e-9;
}
const COND_OPS = { eq: 'equals', ne: 'notEquals', ge: 'atLeast', le: 'atMost' };
// The inspector's form of a condition: kind "param:on" | "param:eq" | "param:ne" | "param:ge" |
// "param:le" | "var:eq" | "var:ne" | "stack:is" | "json:all" (compound all/any conditions, edited as
// JSON in value) or "" for none, the param, var or stack name, the value (for a stack: its view),
// and a stack condition's vars (kept as they are).
export function condForm(c) {
  if (typeof c === 'string') return { kind: 'param:on', name: c, value: '' };
  if (!isObj(c)) return { kind: '', name: '', value: '' };
  if (Array.isArray(c.all) || Array.isArray(c.any)) return { kind: 'json:all', name: '', value: JSON.stringify(c) };
  if ('stack' in c) return { kind: 'stack:is', name: String(c.stack), value: String(c.view ?? ''), vars: c.vars };
  const on = 'var' in c ? 'var' : 'param', m = Object.keys(COND_OPS).find(k => COND_OPS[k] in c) || 'on';
  return { kind: `${on}:${m}`, name: String(c[on] ?? ''), value: m === 'on' ? '' : String(c[COND_OPS[m]]) };
}
export function makeCond({ kind, name, value, vars }) {
  const [on, m] = String(kind).split(':');
  if (on === 'json') { try { const c = JSON.parse(value); return isObj(c) || typeof c === 'string' ? c : undefined; } catch { return undefined; } }
  if (!m || !name) return undefined;
  if (on === 'stack') return { stack: name, view: String(value ?? ''), ...(isObj(vars) ? { vars } : {}) };
  if (m === 'on') return on === 'var' ? { var: name } : name;
  const v = on === 'param' && String(value).trim() !== '' && Number.isFinite(+value) ? +value : String(value ?? '');
  return { [on]: name, [COND_OPS[m]]: v };
}

// flow "column": the visible widgets stacked in file order, the first at the y of the first widget
// in the file, each keeping its x and height, `gap` apart. The others take no space; they sit where
// they would go, for the editor to show. heights[i], when given, is the height a widget takes in
// the column (animated flows). rects: [x, y, w, h] or null. Returns { rects, height }.
export function flowLayout(rects, visible, gap = 0, heights) {
  let y = rects.find(r => r)?.[1] ?? 0, any = false;
  const out = rects.map((r, i) => {
    if (!r) return null;
    if (!visible[i]) return [r[0], y + (any ? gap : 0), r[2], r[3]];
    if (any) y += gap;
    any = true;
    const o = [r[0], y, r[2], r[3]];
    y += heights?.[i] ?? r[3];
    return o;
  });
  return { rects: out, height: y };
}

// Scrollbar of a rect showing content `content` px tall, scrolled down by pos: null when the
// content fits (unless `always`, which gives a thumb as long as the track), else the clamped pos,
// its max, the track (`width` px at the rect's right edge) and the thumb, sized in proportion. The
// thumb is at least 8 px tall (the format sets no minimum).
export function scrollBar(rect, content, pos, width = 12, always = false) {
  const [x, y, w, h] = rect, max = content - h, bw = clamp(width | 0, 1, Math.max(1, w)), track = [x + w - bw, y, bw, h];
  if (max <= 0 || h <= 0) return always ? { pos: 0, max: 0, track, thumb: track.slice() } : null;
  pos = clamp(Math.round(pos) || 0, 0, max);
  const th = clamp(Math.round(h * h / content), Math.min(8, h), h);
  return { pos, max, track, thumb: [track[0], y + Math.round(pos * (h - th) / max), bw, th] };
}
// The scroll position that puts the thumb's top at y.
export const scrollFromThumb = (bar, y) => clamp(Math.round((y - bar.track[1]) * bar.max / Math.max(1, bar.track[3] - bar.thumb[3])), 0, bar.max);

// A list's geometry: a header row when it has a `header`, then rows rowHeight tall (16 when unset:
// the format gives no default) and rowGap apart, drawn down to the bottom even past the last row,
// scrolled by pos, with the scrollbar of `scroll` when needed (or `always`). Cells follow
// `columns` (width, align, edit), colGap apart; without columns one cell spans the row. Returns
// { body, head, pitch, rows: [{ i, rect, cells: [{ rect, align, edit }] }], bar }.
export function listLayout(w, rect, count, pos = 0, d = 1) {
  const rh = Math.max(1, w.rowHeight | 0 || 16), gap = Math.max(0, w.rowGap | 0), pitch = rh + gap, head = w.header ? rh : 0;
  const body = [rect[0], rect[1] + head, rect[2], Math.max(0, rect[3] - head)], s = isObj(w.scroll) ? w.scroll : null;
  const bar = s ? scrollBar(body, count ? count * pitch - gap : 0, pos, s.width ?? dp(12, d), !!s.always) : null;
  pos = bar ? bar.pos : 0;
  const inner = body[2] - (bar ? bar.track[2] : 0), cols = Array.isArray(w.columns) && w.columns.length ? w.columns.filter(isObj) : [{}];
  const rows = [], first = Math.floor(pos / pitch), last = Math.ceil((body[3] + pos) / pitch);
  for (let i = first; i < last; i++) {
    const y = body[1] + i * pitch - pos;
    let x = body[0];
    const cells = cols.map(c => { const cw = Number.isFinite(c.width) ? c.width : inner - (x - body[0]); const cell = { rect: [x, y, cw, rh], align: c.align, edit: c.edit }; x += cw + (w.colGap | 0); return cell; });
    rows.push({ i, rect: [body[0], y, inner, rh], cells });
  }
  return { body, head, pitch, rows, bar };
}

const DEFAULT_PAD = [4, 2, 4, 2];
// A menu's style: skin.json "menu" with a named style's fields over it; null for a native menu (no
// "menu" in the skin, or the name "native").
export function menuStyleOf(skin, name) {
  const m = skin?.menu;
  if (!isObj(m) || name === 'native') return null;
  const { styles, ...base } = m, over = name && isObj(styles) && isObj(styles[name]) ? styles[name] : null;
  const d = densityOf({ 'skin.json': skin }), defaults = d === 1 ? {} : { pad: DEFAULT_PAD.map(v => dp(v, d)), separatorHeight: dp(3, d) };
  return { ...defaults, ...base, ...(over || {}) };
}
// A menu in a style: rows rowHeight tall (default the line height plus the top and bottom pad),
// separators separatorHeight (default 3), as wide as the anchor or the longest label plus pad, the
// check column and (when an item has a submenu) the arrow column. place "below" puts it under the
// anchor (above it when it does not fit below), "over" on the anchor's top-left, "side" right of
// the anchor (a submenu's row; left of it when there is no room); then it slides to stay in the
// window. A native style (native: true) starts a column at each columnBreak item. mt gives
// measure(label), lineH, checkW and arrowW. Returns { rect, rows: [{ i, item, rect }], pad } in
// window coordinates; pad insets a row's text.
export function menuLayout(items, style, mt, anchor, win, place = style?.place || 'below') {
  const [pl, pt, pr, pb] = style?.pad || DEFAULT_PAD, b = style?.border ? 1 : 0, { measure, lineH, checkW = 0, arrowW = 0 } = mt;
  const rowH = style?.rowHeight || lineH + pt + pb, sepH = style?.separatorHeight ?? 3, arrow = items.some(it => Array.isArray(it?.items)) ? arrowW : 0;
  const cols = [[]];
  items.forEach((it, i) => { if (style?.native && it?.columnBreak && cols.at(-1).length) cols.push([]); cols.at(-1).push(i); });
  const rows = [];
  let x = b, h = 0;
  for (const col of cols) {
    const cw = pl + checkW + Math.max(0, ...col.filter(i => !items[i]?.separator).map(i => measure(String(items[i]?.label ?? '')))) + arrow + pr;
    let y = b;
    for (const i of col) { const rh = items[i]?.separator ? sepH : rowH; rows[i] = { i, item: items[i], rect: [x, y, cw, rh] }; y += rh; }
    x += cw; h = Math.max(h, y);
  }
  const W = Math.max(cols.length === 1 ? anchor[2] : 0, x + b), H = h + b;
  if (cols.length === 1) for (const r of rows) r.rect[2] = W - 2 * b;
  let X = anchor[0], Y = anchor[1] + anchor[3];
  if (place === 'over') Y = anchor[1];
  else if (place === 'side') { X = anchor[0] + anchor[2]; Y = anchor[1] - b; if (X + W > win[0]) X = anchor[0] - W; }
  else if (Y + H > win[1] && anchor[1] - H >= 0) Y = anchor[1] - H;
  X = clamp(X, 0, Math.max(0, win[0] - W)); Y = clamp(Y, 0, Math.max(0, win[1] - H));
  for (const r of rows) { r.rect[0] += X; r.rect[1] += Y; }
  return { rect: [X, Y, W, H], rows, pad: [pl + checkW, pt, pr + arrow, pb] };
}
export const menuRowAt = (m, x, y) => m?.rows.find(r => contains(r.rect, x, y)) || null;

// ---- menu item trees (the menu editor): an item is addressed by its path of indices, [2, 0] being
// the first item of the third item's submenu. A position is a path whose last index may equal the
// length of its list (the end). All of these change `items` in place.
const siblings = (items, path) => path.slice(0, -1).reduce((list, i) => list?.[i]?.items, items);
export const menuItemAt = (items, path) => (path.length ? siblings(items, path)?.[path.at(-1)] : undefined);
export const menuKind = it => (!isObj(it) ? 'item' : it.separator ? 'separator' : Array.isArray(it.items) ? 'submenu' : 'item');
export function menuInsert(items, pos, ...add) {
  const list = siblings(items, pos);
  if (!Array.isArray(list)) return null;
  list.splice(clamp(pos.at(-1), 0, list.length), 0, ...add);
  return [...pos.slice(0, -1), clamp(pos.at(-1), 0, list.length - add.length)];
}
export function menuRemove(items, path) {
  const list = siblings(items, path);
  return Array.isArray(list) && path.at(-1) < list.length ? list.splice(path.at(-1), 1)[0] : undefined;
}
// Moves the item at `from` to position `to` (as the positions were before the move); returns its new
// path, or null when `to` lies inside the item itself.
export function menuMove(items, from, to) {
  if (!from.length || (to.length >= from.length && from.every((x, k) => to[k] === x))) return null;
  const d = from.length - 1, t = to.slice();
  if (t.length > d && from.slice(0, d).every((x, k) => x === t[k]) && t[d] > from[d]) t[d]--; // the removal shifts it
  const it = menuRemove(items, from);
  return it === undefined ? null : menuInsert(items, t, it);
}
// Indent makes the item the last one of the previous item's submenu (turning that item into a
// submenu); outdent puts it right after the submenu that holds it. Both return the new path or null.
export function menuIndent(items, path) {
  const i = path.at(-1), prev = i > 0 ? siblings(items, path)?.[i - 1] : null;
  if (!isObj(prev) || prev.separator) return null;
  if (!Array.isArray(prev.items)) prev.items = [];
  return menuMove(items, path, [...path.slice(0, -1), i - 1, prev.items.length]);
}
export const menuOutdent = (items, path) => (path.length < 2 ? null : menuMove(items, path, [...path.slice(0, -2), path.at(-2) + 1]));
// Every item path in display order (depth first), for the tree and for arrow-key selection.
export function menuPaths(items, pre = []) {
  return (Array.isArray(items) ? items : []).flatMap((it, i) => [[...pre, i], ...(Array.isArray(it?.items) ? menuPaths(it.items, [...pre, i]) : [])]);
}
// The value after the largest one in the menu (0 for none), for a new item.
export const menuNextValue = items => 1 + Math.max(-1, ...menuPaths(items).map(p => menuItemAt(items, p)).filter(x => isObj(x) && Number.isFinite(x.value)).map(x => x.value));
// Next item that is not a separator, from i in direction dir (wrapping); i < 0 starts at an end.
export function menuStep(items, i, dir) {
  const n = items.length, from = i < 0 ? (dir > 0 ? -1 : n) : i;
  for (let k = 1; k <= n; k++) { const j = (((from + dir * k) % n) + n) % n; if (!items[j]?.separator) return j; }
  return -1;
}
// A tooltip box for `text` in a skin style (skin.json "tooltip"): its lines (broken at \n) plus pad
// and border, below and right of the pointer, kept inside the window.
export function tipLayout(text, style, measure, lineH, [px, py], win, d = 1) {
  const [pl, pt, pr, pb] = style?.pad || DEFAULT_PAD, b = style?.border ? 1 : 0, lines = String(text).split(/\\n|\n/);
  const w = Math.max(0, ...lines.map(measure)) + pl + pr + 2 * b, h = lines.length * lineH + pt + pb + 2 * b;
  let y = py + dp(20, d);
  if (y + h > win[1]) y = Math.max(0, py - h - 4);
  return { rect: [Math.max(0, Math.min(px + 4, win[0] - w)), y, w, h], text: lines.join('\\n'), pad: [pl + b, pt + b, pr + b, pb + b] };
}

// ---- play mode: a model of the runtime GUI (framework/src/core/editor.cpp) --------------------------

export const paramMap = docs => new Map((Array.isArray(docs['params.json']) ? docs['params.json'] : []).filter(p => p?.id).map(p => [p.id, p]));
const strVars = o => Object.fromEntries(Object.entries(isObj(o) ? o : {}).map(([k, v]) => [k, String(v)]));
// The MIDI learn assignments a fresh instance starts with (skin.json learn.defaults: CC -> param id).
const learnDefaults = docs => strVars(docs['skin.json']?.learn?.defaults);

// Everything a played skin changes: param values, widget-local values, skin-wide vars, what each
// embed shows, visibility toggled by actions, scroll positions, text data, list selections and
// edited cells, animated flow heights, MIDI learn (target and assignments), incoming notes, plus
// the pointer state (all keyed so they survive rebuilding the tree). `events` collects what the
// page must do: { url }, { edit, text, rect, multiline, cell } (an inline editor) and { midi }.
export const playState = docs => ({
  values: {}, locals: {}, vars: strVars(docs['skin.json']?.vars), embeds: {}, hidden: {}, scroll: {}, text: {}, sel: {}, cells: {}, anim: {},
  midiMap: learnDefaults(docs), learn: null, notesIn: [], midiInAt: null, hover: null, press: null, rdown: null, focus: null, menu: null, tip: null, status: null,
  wheelAt: null, midi: {}, events: [],
});

// A played view: the state and the tree built from it. metrics(style) gives a menu style's
// { measure(label), lineH, checkW, arrowW } (default from measure, lineH, checkW); nativeMenu is the
// style drawn for native menus; showAll lets hidden widgets keep their place in flow and fit (the
// editor, while it shows them). Handlers rebuild the tree after each change.
export function playCtx(docs, st, view, opt = {}) {
  const base = { measure: opt.measure || (s => String(s).length * 6), lineH: opt.lineH || 10, checkW: opt.checkW ?? 8, arrowW: opt.arrowW ?? 6 };
  const ctx = {
    docs, st, view, pmap: opt.pmap || paramMap(docs), now: opt.now ?? 0, metrics: opt.metrics || (() => base), nativeMenu: opt.nativeMenu || { native: true },
    showAll: !!opt.showAll, data: opt.data || {}, random: opt.random || Math.random, tileSize: opt.tileSize || (() => null), tipDelay: docs['skin.json']?.tooltip?.delay ?? 700, root: null,
  };
  ctx.rebuild = () => { ctx.root = playTree(ctx); return ctx; };
  return ctx.rebuild();
}

// The tree of shown views as the runtime builds it. A node is { view, doc, parent, owner (the embed
// item), x, y, w, h (after flow and fit), clip, enabled, vars, all (vars in scope), key, items }; an
// item is { w, i, node, key, rect (window coordinates after flow and scrolling, or null), box (the
// part shown, less while an animated flow grows it), hidden, show, vis, enabled, pid, p, pid2, p2,
// text, dataKey, source, child, bar, list, rows, anim }. Keys name nodes and widgets by the views
// and embeds above them, so state survives a rebuild. Stack conditions need the tree itself, so a
// tree that has one is built twice.
function playTree(ctx) {
  const ea = ctx.docs['skin.json']?.editAll, eaP = isObj(ea) && ctx.pmap.get(ea.param);
  ctx.editAll = eaP && typeof ea.var === 'string' && Array.isArray(ea.values) && Math.abs(valueOf(ctx.st, ea.param, eaP) - (eaP.min ?? 0)) > 1e-9 ? { var: ea.var, values: ea.values.map(String) } : null;
  ctx.stackOf = name => { const w = findEmbed(ctx.docs, ctx.docs['skin.json']?.root, name)?.widget; return w ? { view: w.view, vars: strVars(w.vars) } : null; };
  ctx.usesStack = false;
  let root = buildNode(ctx, ctx.view, null, 0, 0, null, {}, 0);
  if (ctx.usesStack) {
    const first = root, fallback = ctx.stackOf;
    ctx.stackOf = name => { const e = embedNamed(first, name); return e ? (e.child ? { view: e.child.view, vars: e.child.vars } : null) : fallback(name); };
    root = buildNode(ctx, ctx.view, null, 0, 0, null, {}, 0);
  }
  if (root.doc?.fit) { // a fitting root view shrinks to the widgets it shows
    const on = [...root.items].filter(it => it.box && (ctx.showAll || it.vis));
    root.w = Math.max(1, ...on.map(it => it.rect[0] + it.rect[2])); root.h = Math.max(1, ...on.map(it => it.rect[1] + it.box[3]));
    root.clip = [0, 0, root.w, root.h];
  }
  const s = ctx.st.status;
  if (s && ctx.now < s.until) {
    const it = findItem(root, s.key);
    if (it?.p) { const t = statusText(it.p, getPlain(ctx.st, it)); for (const x of treeItems(root)) if (x.w.status && (x.w.type === 'textbox' || x.w.type === 'plate')) x.text = t; }
  }
  return root;
}
// Runtime values a textbox, plate or button can show (`source`). The editor has no audio thread:
// cpu reads 0, voices counts held notes.
function sourceValue(ctx, w) {
  const st = ctx.st;
  if (w.source === 'cpu') return 0;
  if (w.source === 'voices') return (st.press?.note != null ? 1 : 0) + st.notesIn.length;
  if (w.source === 'midi_in') return st.midiInAt != null && ctx.now - st.midiInAt < 200 ? 1 : 0;
  if (w.source === 'modified') return +(Object.entries(st.values).some(([id, v]) => { const p = ctx.pmap.get(id); return p && v !== (p.default ?? p.min ?? 0); })
    || Object.keys(st.text).length > 0 || Object.keys(st.midiMap).length > 0);
  return null;
}
function buildNode(ctx, view, owner, x, y, clip, vars, depth) {
  const { st } = ctx, parent = owner?.node || null, doc = ctx.docs[viewPath(view)];
  const n = { view, doc, parent, owner, x, y, w: 0, h: 0, enabled: owner ? owner.enabled : true, vars: strVars(vars), key: owner ? `${owner.key}>${view}` : view, items: [] };
  n.all = { ...(parent ? parent.all : st.vars), ...n.vars };
  if (!isObj(doc)) { n.clip = clip || [x, y, 0, 0]; return n; }
  if (Array.isArray(doc.size)) { n.w = doc.size[0] | 0; n.h = doc.size[1] | 0; }
  const sub = s => resolveVars(s, n.all);
  const env = {
    sub, variable: k => n.all[k], stack: name => { ctx.usesStack = true; return ctx.stackOf(name); },
    param: id => { const p = ctx.pmap.get(id); return p ? { v: valueOf(st, id, p), p } : null; },
  };
  n.items = (Array.isArray(doc.widgets) ? doc.widgets : []).filter(isObj).map((w, i) => {
    const key = `${n.key}/${w.name ?? '#' + i}`, first = Array.isArray(w.params) && w.params.length ? w.params[0] : w.param;
    const it = { w, i, node: n, key, hidden: key in st.hidden ? st.hidden[key] : !!w.hidden, show: evalCondition(w.showIf, env), text: sub(w.text?.text) };
    it.enabled = n.enabled && !w.disabled && evalCondition(w.enableIf, env);
    it.vis = !it.hidden && it.show;
    it.pid = typeof first === 'string' && first ? sub(first) : null;
    it.p = (it.pid && ctx.pmap.get(it.pid)) || null;
    if (isObj(w.valueText)) for (const k of ['row', 'col']) if (typeof w.valueText[k] === 'string') it['vt_' + k] = sub(w.valueText[k]);
    if (Array.isArray(w.params) && w.params.length > 1) { it.pid2 = sub(w.params[1]); it.p2 = ctx.pmap.get(it.pid2) || null; }
    if (Array.isArray(w.mirror) && it.p) it.mirror = w.mirror.map(m => ({ id: sub(m), p: ctx.pmap.get(sub(m)) })).filter(m => m.p);
    const ea = ctx.editAll; // skin.json editAll: while its param is on, a {var} template writes every value's param
    if (ea && it.p && typeof first === 'string' && first.includes(`{${ea.var}}`))
      it.mirror = [...(it.mirror || []), ...ea.values.map(v => resolveVars(first, { ...n.all, [ea.var]: v })).filter(id => id !== it.pid).map(id => ({ id, p: ctx.pmap.get(id) })).filter(m => m.p)];
    if (w.type === 'textbox' && typeof w.key === 'string' && w.key) { it.dataKey = sub(w.key); if (it.dataKey in st.text) it.text = st.text[it.dataKey]; }
    if (typeof w.source === 'string' && w.type !== 'list') {
      it.source = sourceValue(ctx, w);
      if (w.type !== 'button' && (w.source === 'cpu' || w.source === 'voices')) it.text = formatValue(w.format || '%.0f', it.source);
    }
    return it;
  });
  let rel = n.items.map(it => (validRect(it.w.rect) ? it.w.rect.slice() : null));
  // an embed with "fit" takes the size of its view: measure it first
  n.items.forEach((it, k) => {
    if (it.w.type !== 'embed' || !it.w.fit || !rel[k] || depth >= 16) return;
    const v = viewOf(ctx, it);
    if (v) { const m = buildNode(ctx, v.view, { ...it, rect: rel[k], box: rel[k] }, 0, 0, [0, 0, 0, 0], v.vars, depth + 1); rel[k][2] = m.w; rel[k][3] = m.h; }
  });
  let heights = null;
  if (doc.flow === 'column') {
    const on = n.items.map(it => ctx.showAll || it.vis), step = +doc.animate > 0 && !ctx.showAll ? +doc.animate : 0;
    if (step) heights = n.items.map((it, k) => { // animated: each widget takes its current height, moving toward its target
      if (!rel[k]) return 0;
      const tgt = on[k] ? rel[k][3] : 0;
      if (!(it.key in st.anim)) st.anim[it.key] = tgt; // first seen: no animation
      it.anim = { tgt, step };
      return st.anim[it.key];
    });
    const f = flowLayout(rel, heights ? heights.map(h => h > 0) : on, doc.gap | 0, heights);
    rel = f.rects; n.h = f.height;
  }
  n.clip = clip || [x, y, n.w, n.h];
  n.items.forEach((it, k) => {
    const r = rel[k];
    it.rect = r ? [x + r[0], y + r[1], r[2], r[3]] : null;
    it.box = it.rect;
    if (heights && r) { it.vis = heights[k] > 0; it.box = [it.rect[0], it.rect[1], r[2], Math.min(r[3], heights[k])]; }
  });
  for (const it of n.items) {
    if (it.w.type === 'embed' && it.box && depth < 16) buildChild(ctx, it, depth);
    if (it.w.type === 'list' && it.rect) { it.rows = listRows(ctx, it); it.list = listLayout(it.w, it.rect, it.rows.length, st.scroll[it.key] || 0, densityOf(ctx.docs)); it.bar = it.list.bar; }
  }
  return n;
}
// The view an embed shows (a goto's or its own) with its vars, unless it would show a view above it.
function viewOf(ctx, it) {
  const o = ctx.st.embeds[it.key], view = o ? o.view : it.w.view;
  if (typeof view !== 'string' || !view) return null;
  for (let p = it.node; p; p = p.parent) if (p.view === view) return null;
  return { view, vars: o ? o.vars : it.w.vars };
}
function buildChild(ctx, it, depth) {
  const v = viewOf(ctx, it);
  if (!v) return;
  const st = ctx.st, clip = meet(it.node.clip, it.box), sw = isObj(it.w.scroll) && !it.w.fit ? it.w.scroll.width ?? dp(12, densityOf(ctx.docs)) : 0;
  const make = pos => buildNode(ctx, v.view, it, it.rect[0], it.rect[1] - pos, clip, v.vars, depth + 1);
  let pos = sw ? st.scroll[it.key] || 0 : 0;
  it.child = make(pos);
  if (!sw) return;
  it.bar = scrollBar(it.rect, it.child.h, pos, sw, !!it.w.scroll.always);
  const want = it.bar ? it.bar.pos : 0;
  if (want !== pos) { pos = want; it.child = make(pos); it.bar = scrollBar(it.rect, it.child.h, pos, sw, !!it.w.scroll.always); }
}
// A list's rows: for source "midi_map" the learn assignments ({ cells: [cc, param name], cc }), then
// its `fixed` rows (a param not assigned yet: { fixedParam, cells: ["-1", name] }; a label whose CC
// cell is an internal param: { ccParam, cells: [value, label] }), then the static `rows`
// ({ cells, s: index }) with any cells edited in play.
function listRows(ctx, it) {
  const w = it.w, st = ctx.st, edits = st.cells[it.key] || {};
  const own = (Array.isArray(w.rows) ? w.rows : []).map((r, s) => ({ s, cells: (Array.isArray(r) ? r : [r]).map((c, j) => edits[`${s},${j}`] ?? String(c ?? '')) }));
  if (w.source !== 'midi_map') return own;
  const map = Object.entries(st.midiMap).map(([cc, id]) => ({ cc: +cc, cells: [cc, ctx.pmap.get(id)?.name ?? id] })).sort((a, b) => a.cc - b.cc);
  const fixed = (Array.isArray(w.fixed) ? w.fixed : []).filter(isObj).flatMap(f => {
    if (f.param != null) { const id = String(f.param); return Object.values(st.midiMap).includes(id) ? [] : [{ fixedParam: id, cells: ['-1', ctx.pmap.get(id)?.name ?? id] }]; }
    if (f.ccParam == null) return [];
    const id = String(f.ccParam), p = ctx.pmap.get(id);
    return [{ ccParam: id, cells: [p ? formatValue('%.0f', valueOf(st, id, p)) : '-1', String(f.label ?? id)] }];
  });
  return [...map, ...fixed, ...own];
}
// The inspector's form of `fixed` rows, one per line: a param id, or "label | internal CC param".
export function parseFixed(text) {
  const rows = String(text).split('\n').map(l => l.trim()).filter(Boolean).map(l => { const k = l.lastIndexOf('|'); return k < 0 ? { param: l } : { label: l.slice(0, k).trim(), ccParam: l.slice(k + 1).trim() }; });
  return rows.length ? rows : undefined;
}
export const formatFixed = rows => (Array.isArray(rows) ? rows : []).map(r => (r.param != null ? String(r.param) : `${r.label ?? ''} | ${r.ccParam ?? ''}`)).join('\n');
// Points of a key scaling curve in text data: "<note> <level> <slope>|..." up to the first point at
// note 127 or above (clamped to 127); anything after ";" is a kept stale tail.
export function keyscalePoints(text) {
  const out = [];
  for (const part of String(text ?? '').split(';')[0].split('|')) {
    const [n, l, sl] = part.trim().split(/\s+/).map(Number);
    if (![n, l].every(Number.isFinite)) continue;
    out.push([Math.min(127, n), l, Number.isFinite(sl) ? sl : 0.5]);
    if (n >= 127) break;
  }
  return out;
}
export function* treeItems(n) { for (const it of n.items) { yield it; if (it.child) yield* treeItems(it.child); } }
export function findItem(root, key) { for (const it of treeItems(root)) if (it.key === key) return it; return null; }
// A widget by name as the runtime finds it: in node n first, then (deep) depth first below it.
function findIn(n, pred, deep) {
  const own = n.items.find(pred);
  if (own || !deep) return own || null;
  for (const it of n.items) if (it.child) { const r = findIn(it.child, pred, true); if (r) return r; }
  return null;
}
const embedNamed = (n, name) => findIn(n, it => it.w.type === 'embed' && it.w.name === name, true);

// ---- play mode: values --------------------------------------------------------------------------------

const valueOf = (st, id, p) => (id in st.values ? st.values[id] : p.default ?? p.min ?? 0);
// Range of a widget's value: its param's, else its own `range`/`steps`/`default` (pads keep 0..1).
export function itemRange(it, which = 0) {
  const p = which ? it.p2 : it.p, w = it.w;
  if (p) return { p, min: p.min ?? 0, max: p.max ?? 1, steps: p.steps | 0, taper: p.taper, def: p.default ?? p.min ?? 0 };
  if (w.type === 'custom') return { min: 0, max: 1, steps: 0, def: +w.value || 0 };
  const min = w.range?.[0] ?? 0;
  return { min, max: w.range?.[1] ?? 1, steps: Math.max(0, w.steps | 0), def: w.default ?? min };
}
const localKey = (it, which) => (which ? it.key + '#2' : it.key);
export function getPlain(st, it, which = 0) {
  const p = which ? it.p2 : it.p;
  if (p) return valueOf(st, which ? it.pid2 : it.pid, p);
  const k = localKey(it, which);
  return k in st.locals ? st.locals[k] : +it.w.value || 0;
}
const snap = (v, { min, max, steps, taper }) => {
  v = clamp(v, Math.min(min, max), Math.max(min, max));
  return steps > 0 && max !== min ? fromNorm(Math.round(toNorm(v, min, max, taper) * steps) / steps, min, max, taper) : v;
};
// Params (and unbound dials and numbers) clamp to their range and snap to their steps. A widget's
// "mirror" params get the same normalized value with every edit.
export function setPlain(st, it, v, which = 0) {
  const r = itemRange(it, which), t = it.w.type;
  if (!Number.isFinite(v)) v = r.def;
  if (r.p || t === 'dial' || t === 'number') v = snap(v, r);
  if (!r.p) { st.locals[localKey(it, which)] = v; return; }
  st.values[which ? it.pid2 : it.pid] = v;
  if (!which) for (const m of it.mirror || []) {
    const q = { min: m.p.min ?? 0, max: m.p.max ?? 1, steps: m.p.steps | 0, taper: m.p.taper };
    st.values[m.id] = snap(fromNorm(toNorm(v, r.min, r.max, r.taper), q.min, q.max, q.taper), q);
  }
}
export const getNorm = (st, it, which = 0) => { const r = itemRange(it, which); return toNorm(getPlain(st, it, which), r.min, r.max, r.taper); };
export const setNorm = (st, it, t, which = 0) => { const r = itemRange(it, which); setPlain(st, it, fromNorm(clamp(t, 0, 1), r.min, r.max, r.taper), which); };
// A button's value as a step index: stepped params by step, continuous ones on or off at 0.5.
export function itemIndex(st, it) {
  if (!it.p) return Math.round(getPlain(st, it));
  const r = itemRange(it), t = getNorm(st, it);
  return r.steps > 0 ? Math.round(t * r.steps) : t >= 0.5 ? 1 : 0;
}
// A number's text: zeroText while the value is exactly 0; with "valueText", a data table's cell [row][col]
// (each a fixed index, a param's value, or else the value less first) or value * scale; else the
// formatted value. As in the runtime, an empty or missing cell shows the plain number.
export function numberText(w, p, v, ctx, it) {
  if (v === 0 && typeof w.zeroText === 'string') return w.zeroText;
  const vt = isObj(w.valueText) ? w.valueText : null;
  if (vt && !vt.table && vt.scale) return formatValue(vt.format || '%g', v * vt.scale);
  if (vt?.table && ctx) {
    const own = v - (+vt.first || 0);
    const index = k => {
      if (typeof vt[k] === 'number') return vt[k];
      const id = it?.['vt_' + k], q = id && ctx.pmap.get(id);
      return q ? valueOf(ctx.st, id, q) : own;
    };
    const cell = ctx.data?.[vt.table]?.[Math.round(index('row'))]?.[Math.round(index('col'))];
    if (typeof cell === 'string' && cell) return cell;
  }
  return displayText(w, p, v);
}
// What a double-click resets a dial or number to (`reset`: "default", "zero" when the range spans 0, or false), or null.
export function resetValue(it) {
  const r = itemRange(it), m = it.w.reset ?? 'default';
  if (m === 'zero') return Math.min(r.min, r.max) <= 0 && Math.max(r.min, r.max) >= 0 ? 0 : null;
  return m === 'default' ? r.def : null;
}

// ---- play mode: buttons and actions -------------------------------------------------------------------

const ACTION_KINDS = ['goto', 'set', 'toggle', 'url', 'step', 'value', 'sequence', 'presets', 'data', 'midi_map', '_preset'];
const kindOf = a => (isObj(a) ? ACTION_KINDS.find(k => k in a) : undefined);
export const actionKind = w => kindOf(w?.action);
// Whether an action is "on": a goto whose stack shows its view with its vars, a set whose vars all
// hold, a toggle whose widget is shown. A goto whose stack is not in the shown tree is compared
// with the files' initial state (the editor shows views on their own).
export function actionOn(ctx, it, a) {
  const kind = kindOf(a);
  if (kind === 'set') { const e = Object.entries(isObj(a.set) ? a.set : {}); return e.length > 0 && e.every(([k, v]) => ctx.st.vars[k] === String(v)); }
  if (kind === 'toggle') { const t = findIn(it.node, x => x.w.name === a.toggle, false) || findIn(ctx.root, x => x.w.name === a.toggle, true); return !!t && !t.hidden; }
  if (kind !== 'goto') return false;
  const e = embedNamed(ctx.root, a.stack), s = e ? (e.child ? { view: e.child.view, vars: e.child.vars } : null) : ctx.stackOf(a.stack);
  return !!s && s.view === a.goto && sameVars(a.vars, s.vars);
}
export const isActive = (ctx, it) => actionOn(ctx, it, it.w.action);
export function buttonValue(ctx, it) {
  if (it.source != null) return it.source;
  const k = actionKind(it.w);
  if (k === 'value') return +(!!it.p && getPlain(ctx.st, it) === +it.w.action.value);   // a radio button: on while its param holds the value
  return k === 'goto' || k === 'set' ? +isActive(ctx, it) : k === 'step' ? 0 : itemIndex(ctx.st, it);
}
function flip(ctx, it) {
  const st = ctx.st;
  if (!it.p) return setPlain(st, it, getPlain(st, it) ? 0 : 1);
  const r = itemRange(it), k = itemIndex(st, it);
  setPlain(st, it, r.steps >= 2 ? fromNorm(((k + 1) % (r.steps + 1)) / r.steps, r.min, r.max, r.taper) : k ? r.min : r.max);
}
function act(ctx, it) { if (it.w.toggle) flip(ctx, it); runAction(ctx, it); }
// Runs an action (the widget's own, or a menu item's) and rebuilds the tree, so `it` is stale
// afterwards. A goto shows a fresh page: state kept below that embed is dropped, as the runtime
// rebuilds it. midi_map "remove" drops the assignment in the selected row of the list `it`,
// "reset" drops them all.
export function runAction(ctx, it, a = it.w.action) {
  const st = ctx.st, kind = kindOf(a);
  if (kind === 'url') st.events.push({ url: String(a.url) });
  else if (kind === 'step') { if (it.p) setPlain(st, it, getPlain(st, it) + (+a.step || 0)); }
  else if (kind === 'value') { if (it.p) setPlain(st, it, +a.value); }
  else if (kind === 'set') Object.assign(st.vars, strVars(a.set));
  else if (kind === 'toggle') {
    const t = findIn(it.node, x => x.w.name === a.toggle, false) || findIn(ctx.root, x => x.w.name === a.toggle, true);
    if (t) st.hidden[t.key] = !t.hidden;
  } else if (kind === 'goto') {
    const e = embedNamed(ctx.root, a.stack);
    if (e && isObj(ctx.docs[viewPath(a.goto)])) {
      for (const m of [st.embeds, st.hidden, st.scroll, st.locals, st.anim]) for (const k of Object.keys(m)) if (k.startsWith(e.key + '>')) delete m[k];
      st.embeds[e.key] = { view: a.goto, vars: strVars(a.vars) };
      delete st.scroll[e.key];
    }
  } else if (kind === 'sequence') sequence(ctx, it, a);
  else if (kind === 'presets') openPresets(ctx, it, a);
  else if (kind === 'data') { for (const [k, v] of Object.entries(isObj(a.data) ? a.data : {})) st.text[resolveVars(k, it.node.all)] = String(v); } // text data, keys may use {var}
  else if (kind === '_preset') applyPreset(ctx, it, a);
  else if (kind === 'midi_map') {
    if (a.midi_map === 'reset') st.midiMap = learnDefaults(ctx.docs); // back to a fresh instance's assignments
    else if (a.midi_map === 'remove') { const row = it.rows?.[st.sel[it.key]]; if (row?.cc != null) { delete st.midiMap[row.cc]; st.sel[it.key] = null; } }
  }
  ctx.rebuild();
}

// The presets action: a native menu of the slots of data/<table>.json ({ "presets": [{ "name", ... }] },
// kept once changed in text data presets.<table>), `columns` items a column. A slot copies its
// content (the slot without its name, as JSON) into text data `key` and its name into `name`; with
// `save` a Save submenu stores the current content and name into a slot.
function presetSlots(ctx, table) {
  try { const s = JSON.parse(ctx.st.text[`presets.${table}`]); if (Array.isArray(s)) return s; } catch { /* not saved yet */ }
  const t = ctx.data[table]?.presets;
  return Array.isArray(t) ? t : [];
}
function openPresets(ctx, it, a) {
  const table = String(a.presets), slots = presetSlots(ctx, table), per = a.columns | 0;
  if (!slots.length) return;
  const item = (op, s, k) => ({ label: String(s?.name ?? `Slot ${k + 1}`), columnBreak: per > 0 && k > 0 && k % per === 0, action: { _preset: op, table, slot: k, key: a.key, name: a.name } });
  const items = slots.map((s, k) => item('load', s, k));
  if (a.save) items.push({ separator: true }, { label: 'Save', items: slots.map((s, k) => item('save', s, k)) });
  ctx.st.menu = { key: it.key, levels: [{ items, hover: -1 }], style: null, anchor: [it.rect[0], it.rect[1], 0, it.rect[3]], place: 'below', at: ctx.now, context: true };
  placeMenuAt(ctx, it);
}
// "menuAt": [x, y] on a dropdown or presets button: the menu's top-left at that point of the view.
function placeMenuAt(ctx, it) {
  const m = ctx.st.menu, p = it.w.menuAt;
  if (m && Array.isArray(p) && p.length === 2 && p.every(Number.isFinite)) { m.anchor = [it.node.x + p[0], it.node.y + p[1], 0, 0]; m.place = 'over'; }
}
function applyPreset(ctx, it, a) {
  const st = ctx.st, slots = presetSlots(ctx, a.table), slot = slots[a.slot], sub = v => (v ? resolveVars(String(v), it.node.all) : null), key = sub(a.key), nameKey = sub(a.name);
  if (!isObj(slot)) return;
  if (a._preset === 'load') {
    const { name, ...content } = slot;
    if (key) st.text[key] = JSON.stringify(content);
    if (nameKey) st.text[nameKey] = String(name ?? '');
    return;
  }
  let cur = null;
  try { const c = JSON.parse(st.text[key]); if (isObj(c)) cur = c; } catch { /* nothing stored: keep the slot's content */ }
  const name = nameKey && st.text[nameKey] != null ? st.text[nameKey] : slot.name;
  st.text[`presets.${a.table}`] = JSON.stringify(slots.map((s, k) => (k === a.slot ? (cur ? { name, ...cur } : { ...slot, name }) : s)));
}
// A step sequence: params <prefix><n>.<leaf> for n = 1..count. "insert" moves steps index..count-1
// one place up (the last is dropped) and resets step index; "delete" moves steps index+1..count one
// place down and resets step count. index may use {var}. "reset" and "random" set one `leaf` of
// every step to its default or to a random valid value (uniform over the param's values; the
// runtime's row rules in editor.cpp are not reproduced).
function sequence(ctx, it, a) {
  const st = ctx.st, prefix = String(a.prefix ?? ''), count = a.count | 0, index = parseInt(resolveVars(String(a.index ?? ''), it.node.all), 10);
  if (a.sequence === 'reset' || a.sequence === 'random') {
    for (let n = 1; n <= count; n++) {
      const id = `${prefix}${n}.${a.leaf}`, p = ctx.pmap.get(id);
      if (!p) continue;
      if (a.sequence === 'reset') { delete st.values[id]; continue; }
      const steps = p.steps | 0, t = steps > 0 ? Math.floor(ctx.random() * (steps + 1)) / steps : ctx.random();
      st.values[id] = fromNorm(Math.min(1, t), p.min ?? 0, p.max ?? 1, p.taper);
    }
    return;
  }
  if (!(index >= 1 && index <= count)) return;
  const leaves = new Set();
  for (const id of ctx.pmap.keys()) { const m = id.startsWith(prefix) && /^(\d+)\.(.+)$/.exec(id.slice(prefix.length)); if (m && +m[1] >= 1 && +m[1] <= count) leaves.add(m[2]); }
  for (const leaf of leaves) {
    const id = n => `${prefix}${n}.${leaf}`, has = n => ctx.pmap.has(id(n)), get = n => valueOf(st, id(n), ctx.pmap.get(id(n)));
    const move = (to, from) => { if (has(to)) { if (has(from)) st.values[id(to)] = get(from); else delete st.values[id(to)]; } };
    if (a.sequence === 'insert') { for (let n = count; n > index; n--) move(n, n - 1); delete st.values[id(index)]; }
    else if (a.sequence === 'delete') { for (let n = index; n < count; n++) move(n, n + 1); delete st.values[id(count)]; }
  }
}

// ---- play mode: pointer, wheel, keys, MIDI in --------------------------------------------------------

const INPUT = ['button', 'dropdown', 'dial', 'number', 'veil', 'list'];
const takesInput = it => INPUT.includes(it.w.type) || (it.w.type === 'custom' && ['pad', 'morph_pad', 'piano'].includes(it.w.kind))
  || (it.w.type === 'textbox' && !!it.w.editable && !!it.dataKey);
const shown = it => !!it.box && it.vis;
const topFirst = n => drawOrder(n.items, it => it.w).reverse();

// The widget a click at (x, y) goes to, as in the runtime: topmost first and into embeds (a
// scrollbar before the content under it), skipping hidden and disabled widgets and those that
// take no input. A veil swallows the click.
export function playHit(n, x, y) {
  if (!contains(n.clip, x, y)) return null;
  for (const it of topFirst(n)) {
    if (!shown(it) || !contains(it.box, x, y)) continue;
    if (it.bar && it.child && contains(it.bar.track, x, y)) return it;
    if (it.child) { const h = playHit(it.child, x, y); if (h) return h; continue; }
    if (it.enabled && takesInput(it)) return it;
  }
  return null;
}
// The topmost shown widget under (x, y), into embeds, for which pick(item) holds (tips, status,
// context menus).
export function playTop(n, x, y, pick) {
  if (!contains(n.clip, x, y)) return null;
  for (const it of topFirst(n)) {
    if (!shown(it) || !contains(it.box, x, y)) continue;
    if (it.child) { const h = playTop(it.child, x, y, pick); if (h) return h; } else if (pick(it)) return it;
  }
  return null;
}
// The innermost embed or list under (x, y) that scrolls.
function scrollAt(n, x, y) {
  if (!contains(n.clip, x, y)) return null;
  for (const it of topFirst(n)) {
    if (!shown(it) || !contains(it.box, x, y)) continue;
    if (it.child) return scrollAt(it.child, x, y) || (it.bar ? it : null);
    if (it.w.type === 'list' && it.bar) return it;
  }
  return null;
}
const pressMode = w => w.press || 'down';
const isStep = w => actionKind(w) === 'step';
const learn = ctx => { const l = ctx.docs['skin.json']?.learn, p = isObj(l) && ctx.pmap.get(l.param); return p ? Math.abs(valueOf(ctx.st, l.param, p) - (p.min ?? 0)) > 1e-9 : false; };

// A dial with "midi" reports what it would send: pitch bend 0..16383, or controller n 0..127.
function midi(ctx, it) {
  const m = String(it.w.midi ?? ''), cc = /^cc(\d+)$/.exec(m), t = getNorm(ctx.st, it);
  const out = m === 'bend' ? `pitch bend ${Math.round(t * 16383)}` : cc ? `cc ${cc[1]} ${Math.round(t * 127)}` : null;
  if (out && ctx.st.midi[it.key] !== out) { ctx.st.midi[it.key] = out; ctx.st.events.push({ midi: out }); }
}
// A piano's keys, laid out by its key images when it has them (their tile sizes from ctx.tileSize).
export const pianoLayout = (ctx, it) => pianoKeys(it.w.first ?? 36, it.w.count ?? 61, it.rect, isObj(it.w.keyImages) && it.w.keyImages.black ? ctx.tileSize(it.w.keyImages.black) : null, densityOf(ctx.docs));
// The piano: the key under the pointer sounds (a fixed `velocity`, else from the height on the key);
// sliding to another key retriggers unless "glide" is false.
function piano(ctx, it, x, y) {
  const pr = ctx.st.press, w = it.w, keys = pianoLayout(ctx, it);
  let k = null;
  for (let i = keys.length - 1; i >= 0 && !k; i--) if (contains(keys[i].rect, x, y)) k = keys[i];
  const note = k ? k.note : null;
  if (note === pr.note || (pr.note != null && w.glide === false)) return;
  if (pr.note != null) ctx.st.events.push({ midi: `note off ${pr.note}` });
  pr.note = note;
  if (k) ctx.st.events.push({ midi: `note on ${note} velocity ${Number.isFinite(w.velocity) ? clamp(Math.round(w.velocity), 1, 127) : pianoVelocity(k.rect, y)}` });
}
// Dragging a dial, number or pad, as the runtime does. Relative drags re-anchor while in range, so
// overshoot past an end is kept.
function drag(ctx, it, x, y, shift, start) {
  const w = it.w, r = it.rect, st = ctx.st, pr = st.press;
  const d = densityOf(ctx.docs); // rates are per design pixel, as in the runtime
  if (w.type === 'custom') { // pads: the handle's centre follows the pointer, y up
    const hs = padHandle(r, d);
    setNorm(st, it, (x - r[0] - (hs >> 1)) / Math.max(r[2] - hs, 1), 0);
    setNorm(st, it, (r[1] + r[3] - (hs - (hs >> 1)) - y) / Math.max(r[3] - hs, 1), 1);
    return;
  }
  const vertical = w.type === 'number' || (w.vertical ?? true);
  if (w.type === 'dial' && w.drag === 'absolute') {
    const m = w.margin | 0, len = Math.max((vertical ? r[3] : r[2]) - 2 * m - 1, 1);
    setNorm(st, it, vertical ? 1 - (y - r[1] - m) / len : (x - r[0] - m) / len);
  } else if (w.type === 'dial' && w.drag === 'rotary') { // a 270 degree sweep around the centre, as the runtime assumes
    setNorm(st, it, (Math.atan2(x - (r[0] + r[2] / 2), r[1] + r[3] / 2 - y) * 180 / Math.PI + 135) / 270);
  } else {
    const pos = vertical ? y : x, zones = w.type === 'number' && Array.isArray(w.sensitivityZones) && w.sensitivityZones.length ? w.sensitivityZones : null;
    // sensitivityZones: equal columns left to right, the one the drag starts in sets the rate
    if (start) { pr.anchor = getNorm(st, it); pr.pos = pos; if (zones) pr.zone = clamp(Math.floor((x - r[0]) * zones.length / Math.max(1, r[2])), 0, zones.length - 1); return; }
    let per = (w.sensitivity ?? 1) / (200 * d); // dials: a full range per 200 px at 1.0
    if (w.type === 'number') {
      const q = itemRange(it), span = q.max - q.min, s = zones ? +zones[pr.zone ?? 0] || 0 : w.sensitivity ?? 0;
      per = s <= 0 ? (zones ? 0 : 1 / (200 * d)) : span ? s / Math.abs(span) / d : 0;
    }
    const t = pr.anchor + (vertical ? pr.pos - pos : pos - pr.pos) * per * (shift ? w.fine ?? 0.1 : 1);
    if (t >= 0 && t <= 1) { pr.anchor = t; pr.pos = pos; }
    setNorm(st, it, t);
  }
  midi(ctx, it);
}
function scrollTo(ctx, it, top) { if (it.bar) ctx.st.scroll[it.key] = scrollFromThumb(it.bar, top); }
const editEvent = (it, text, extra) => ({ edit: it.key, text, rect: it.rect, ...extra });
// A click in a list selects the row under it (not the empty rows below the last) and focuses the list.
function listPick(ctx, it, x, y) {
  const row = it.list.rows.find(r => r.i < it.rows.length && contains(r.rect, x, y)) || null;
  ctx.st.focus = it.key;
  ctx.st.sel[it.key] = row ? row.i : null;
  return { row, c: row ? row.cells.findIndex(cl => contains(cl.rect, x, y)) : -1 };
}

// ---- play mode: menus -----------------------------------------------------------------------------------

// Opens a menu of `items` for widget `it` at anchor (a dropdown's rect, or the pointer for a context
// menu) in the widget's menuStyle (a context menu: the named style "plain" when there is one). The
// item for the current value (or the submenu holding it) is highlighted. st.menu keeps the open
// levels (submenus).
function openMenu(ctx, it, items, anchor, context) {
  const skin = ctx.docs['skin.json'], name = it.w.menuStyle ?? (context && isObj(skin?.menu?.styles?.plain) ? 'plain' : undefined), style = menuStyleOf(skin, name);
  const v = getPlain(ctx.st, it), hover = context ? -1 : items.findIndex(x => (valueItem(x) && Math.abs((+x.value || 0) - v) < 1e-9) || !!itemForValue(x?.items, v));
  ctx.st.menu = { key: it.key, levels: [{ items, hover }], style, anchor, place: context ? 'below' : style?.place || 'below', at: ctx.now, context: !!context };
}
// The style an open menu is drawn in (the native stand-in for native menus).
export const menuStyleNow = ctx => (ctx.st.menu ? ctx.st.menu.style || ctx.nativeMenu : null);
// Layouts of the open menu and its open submenus, outermost first.
export function menuLayouts(ctx) {
  const m = ctx.st.menu;
  if (!m) return [];
  const style = menuStyleNow(ctx), mt = ctx.metrics(style), win = [ctx.root.w, ctx.root.h], out = [];
  let anchor = m.anchor, place = m.place;
  for (const lv of m.levels) {
    const lay = menuLayout(lv.items, style, mt, anchor, win, place);
    out.push(lay);
    anchor = lay.rows[lv.hover]?.rect;
    place = 'side';
    if (!anchor) break;
  }
  return out;
}
// Whether a menu item shows a check mark: a check item whose action is on, else an item whose
// value is the widget's value. Native menus check only check items.
export function menuChecked(ctx, item) {
  const m = ctx.st.menu, it = m && findItem(ctx.root, m.key);
  if (!it || !isObj(item) || item.separator || Array.isArray(item.items)) return false;
  if (!m.style && !item.check) return false;
  if (isObj(item.action)) return !!item.check && actionOn(ctx, it, item.action);
  return (!m.context || 'value' in item) && Math.abs((+item.value || 0) - getPlain(ctx.st, it)) < 1e-9;
}
// An item that stands for a value (not a separator, a submenu or an action).
const valueItem = x => isObj(x) && !x.separator && !isObj(x.action) && !Array.isArray(x.items);
// The item (searching submenus) whose value is v; captionFromItem shows its `short` or its label.
export function itemForValue(items, v) {
  for (const x of Array.isArray(items) ? items : []) {
    const r = Array.isArray(x?.items) ? itemForValue(x.items, v) : valueItem(x) && Math.abs((+x.value || 0) - v) < 1e-9 ? x : null;
    if (r) return r;
  }
  return null;
}
function menuAt(ctx, x, y) {
  const lays = menuLayouts(ctx);
  for (let k = lays.length - 1; k >= 0; k--) if (contains(lays[k].rect, x, y)) return { level: k, row: menuRowAt(lays[k], x, y) };
  return null;
}
// Highlights item i of a level and opens its submenu (closing deeper ones).
function hoverItem(m, level, i, keys) {
  m.levels.length = level + 1;
  m.levels[level].hover = i;
  const sub = m.levels[level].items[i]?.items;
  if (Array.isArray(sub)) { const items = sub.filter(isObj); m.levels.push({ items, hover: keys ? menuStep(items, -1, 1) : -1 }); }
}
// Chooses item i of a level: a submenu opens; an item with an action runs it; otherwise the value
// is set (and a dropdown then runs its own action).
function choose(ctx, level, i) {
  const st = ctx.st, m = st.menu, item = m?.levels[level]?.items[i], it = m && findItem(ctx.root, m.key);
  if (!isObj(item) || item.separator) return;
  if (Array.isArray(item.items)) { hoverItem(m, level, i, true); return ctx.rebuild(); }
  st.menu = null;
  if (!it) return ctx.rebuild();
  if (isObj(item.action)) return runAction(ctx, it, item.action);
  setPlain(st, it, +item.value || 0);
  if (m.context) ctx.rebuild(); else runAction(ctx, it);
}

// ---- play mode: events ------------------------------------------------------------------------------------

export const tipVisible = ctx => !!ctx.st.tip && ctx.now - ctx.st.tip.at >= ctx.tipDelay;

export function playDown(ctx, x, y, { dbl = false, shift = false, right = false } = {}) {
  const st = ctx.st;
  st.tip = null;
  if (st.menu) { // a press inside a menu waits for the release; outside, it closes the menu and does nothing else
    if (right || !menuAt(ctx, x, y)) { st.menu = null; ctx.rebuild(); }
    return;
  }
  if (right) { // a context menu opens on release; a list row under the pointer is selected first
    const li = playHit(ctx.root, x, y);
    if (li?.w.type === 'list') listPick(ctx, li, x, y);
    st.rdown = playTop(ctx.root, x, y, i => i.enabled && Array.isArray(i.w.context) && i.w.context.length > 0)?.key ?? null;
    return ctx.rebuild();
  }
  if (st.press) return;
  const it = playHit(ctx.root, x, y), w = it?.w;
  if (!it) return;
  if (it.bar && contains(it.bar.track, x, y)) {
    const t = it.bar.thumb, grab = contains(t, x, y) ? y - t[1] : t[3] >> 1;
    st.press = { key: it.key, scroll: grab };
    scrollTo(ctx, it, y - grab);
    return ctx.rebuild();
  }
  if (learn(ctx) && it.p && it.p.host !== false) { st.learn = it.pid; return ctx.rebuild(); } // MIDI learn picks the target
  if (w.type === 'button') {
    st.press = { key: it.key, next: ctx.now + 400 };
    const m = pressMode(w);
    if (m === 'down') act(ctx, it);
    else if (m !== 'up') { if (w.toggle) flip(ctx, it); else if (!isStep(w)) setPlain(st, it, 1); runAction(ctx, it); }
  } else if (w.type === 'dropdown') { const items = Array.isArray(w.items) ? w.items.filter(isObj) : []; if (items.length) { openMenu(ctx, it, items, it.rect, false); placeMenuAt(ctx, it); } }
  else if (w.type === 'textbox') { if (dbl || w.editOn === 'click') st.events.push(editEvent(it, it.text, { multiline: !!w.multiline })); }
  else if (w.type === 'list') {
    const { row, c } = listPick(ctx, it, x, y);
    if (dbl && row && row.cells[c]?.edit === 'int') st.events.push({ edit: it.key, cell: [row.i, c], text: it.rows[row.i].cells[c] ?? '', rect: row.cells[c].rect });
  } else if (w.type === 'custom' && w.kind === 'piano') { st.press = { key: it.key, note: null }; piano(ctx, it, x, y); }
  else if (w.type === 'dial' || w.type === 'number' || w.type === 'custom') {
    if (dbl && w.type === 'number' && w.editable) st.events.push(editEvent(it, displayText(w, it.p, getPlain(st, it))));
    else if (dbl) {
      const v = w.type === 'custom' ? itemRange(it).def : resetValue(it);
      if (v != null) setPlain(st, it, v);
      if (w.type === 'custom') setPlain(st, it, itemRange(it, 1).def, 1);
      midi(ctx, it);
    } else if (!(w.type === 'number' && w.drag === false)) { st.press = { key: it.key }; drag(ctx, it, x, y, shift, true); } // "drag": false: no dragging
  }
  if (st.press && st.press.scroll == null) st.status = { key: it.key, until: Infinity };
  ctx.rebuild();
}
export function playMove(ctx, x, y, { shift = false } = {}) {
  const st = ctx.st, pr = st.press;
  if (st.menu) {
    const h = menuAt(ctx, x, y);
    if (h?.row && !h.row.item.separator && st.menu.levels[h.level].hover !== h.row.i) hoverItem(st.menu, h.level, h.row.i, false);
    return ctx.rebuild();
  }
  if (pr) {
    const it = findItem(ctx.root, pr.key), t = it?.w.type;
    if (!it) st.press = null;
    else if (pr.scroll != null) scrollTo(ctx, it, y - pr.scroll);
    else if (t === 'custom' && it.w.kind === 'piano') piano(ctx, it, x, y);
    else if (t === 'dial' || t === 'number' || t === 'custom') drag(ctx, it, x, y, shift, false);
    return ctx.rebuild();
  }
  const h = playHit(ctx.root, x, y)?.key ?? null;
  if (h !== st.hover) st.wheelAt = null;
  st.hover = h;
  const t = playTop(ctx.root, x, y, i => !!i.w.tip);
  if (!t) st.tip = null;
  else if (st.tip?.key !== t.key) st.tip = { key: t.key, text: String(t.w.tip), x, y, at: ctx.now };
  else if (!tipVisible(ctx)) Object.assign(st.tip, { x, y, at: ctx.now }); // it shows once the pointer rests
  const q = playTop(ctx.root, x, y, i => !!i.p);
  if (q) st.status = { key: q.key, until: Infinity };
  else if (st.status?.until === Infinity) st.status.until = ctx.now + 1500;
  ctx.rebuild();
}
export function playUp(ctx, x, y, { shift = false, right = false } = {}) {
  const st = ctx.st, pr = st.press;
  if (right) { // a context menu opens on release, at the pointer
    const t = playTop(ctx.root, x, y, i => i.enabled && Array.isArray(i.w.context) && i.w.context.length > 0);
    if (t && t.key === st.rdown && !st.menu) openMenu(ctx, t, t.w.context.filter(isObj), [x, y, 0, 0], true);
    st.rdown = null;
    return ctx.rebuild();
  }
  if (st.menu && !pr) { // a release over an item chooses it, unless it comes within releaseGuard ms of opening
    const h = menuAt(ctx, x, y), m = st.menu;
    if (h?.row && !h.row.item.separator && !Array.isArray(h.row.item.items) && ctx.now - m.at >= (+m.style?.releaseGuard || 0)) choose(ctx, h.level, h.row.i);
    return;
  }
  const it = pr && findItem(ctx.root, pr.key);
  st.press = null;
  if (it && pr.scroll == null) {
    const w = it.w, m = pressMode(w);
    if (w.type === 'button' && (m === 'momentary' || m === 'repeat')) {
      if (w.toggle) flip(ctx, it); else if (!isStep(w)) setPlain(st, it, 0);
      if (!isStep(w)) runAction(ctx, it); // a nudge acts only while held
    } else if (w.type === 'button' && m === 'up') { if (contains(it.box, x, y)) act(ctx, it); }
    else if (w.type === 'custom' && w.kind === 'piano') { if (pr.note != null) st.events.push({ midi: `note off ${pr.note}` }); }
    else if (w.type === 'dial' && w.spring != null && Number.isFinite(+w.spring)) { setNorm(st, it, +w.spring); midi(ctx, it); }
  }
  if (st.status?.until === Infinity) st.status.until = ctx.now + 1500;
  ctx.rebuild();
  playMove(ctx, x, y, { shift });
}
export function playLeave(ctx) {
  const st = ctx.st;
  if (st.press || st.menu) return;
  st.hover = null; st.tip = null;
  if (st.status?.until === Infinity) st.status.until = ctx.now + 1500;
  ctx.rebuild();
}
// Button repeat, called every few ms while pressed: a "repeat" button acts every 100 ms after a 400 ms delay. True when it acted.
export function playTick(ctx) {
  const pr = ctx.st.press, it = pr && findItem(ctx.root, pr.key);
  if (!it || it.w.type !== 'button' || it.w.press !== 'repeat' || ctx.now < pr.next) return false;
  pr.next = ctx.now + 100;
  runAction(ctx, it);
  return true;
}
// The wheel (notches > 0 = away from the user) moves the dial or number under the pointer by its
// `wheel` fraction of the range per notch (default 0.05 for dials, 0 = off for numbers; a stepped
// value accumulates it and snaps to the nearest step; Shift multiplies by `fine`), else scrolls the innermost scrolling
// embed (20 px per notch; the format does not fix it) or list (a row per notch). True when used.
export function playWheel(ctx, x, y, notches, { shift = false } = {}) {
  const st = ctx.st;
  if (st.press || st.menu) return false;
  const it = playHit(ctx.root, x, y), frac = it && (it.w.type === 'dial' || it.w.type === 'number') ? +(it.w.wheel ?? (it.w.type === 'dial' ? 0.05 : 0)) || 0 : 0;
  if (frac > 0) { // the unsnapped position accumulates; a stepped value snaps to the nearest step
    const w0 = st.wheelAt, t = clamp((w0?.key === it.key && Math.abs(w0.v - getPlain(st, it)) < 1e-9 ? w0.t : getNorm(st, it)) + notches * frac * (shift ? it.w.fine ?? 0.1 : 1), 0, 1);
    setNorm(st, it, t);
    st.wheelAt = { key: it.key, t, v: getPlain(st, it) };
    midi(ctx, it);
    ctx.rebuild();
    return true;
  }
  const s = scrollAt(ctx.root, x, y);
  if (!s) return false;
  st.scroll[s.key] = clamp(s.bar.pos - Math.round(notches * (s.list ? s.list.pitch : 20)), 0, s.bar.max);
  ctx.rebuild();
  return true;
}
// Keys: in an open menu arrows move (Right opens a submenu, Left closes it), Enter chooses and
// Escape closes (with the style's keys "escape", only Escape); otherwise Delete removes the
// selected row of the last clicked midi_map list. True when used.
export function playKey(ctx, key) {
  const st = ctx.st, m = st.menu;
  if (!m) {
    const it = key === 'Delete' && st.focus && findItem(ctx.root, st.focus), row = it?.w.source === 'midi_map' && it.rows[st.sel[it.key]];
    if (row?.cc == null) return false;
    delete st.midiMap[row.cc];
    st.sel[it.key] = null;
    ctx.rebuild();
    return true;
  }
  if (key === 'Escape') { st.menu = null; ctx.rebuild(); return true; }
  if (m.style?.keys === 'escape') return false;
  const k = m.levels.length - 1, lv = m.levels[k];
  if (key === 'ArrowDown' || key === 'ArrowUp') lv.hover = menuStep(lv.items, lv.hover, key === 'ArrowDown' ? 1 : -1);
  else if (key === 'ArrowRight') { if (lv.hover >= 0) hoverItem(m, k, lv.hover, true); }
  else if (key === 'ArrowLeft') { if (k > 0) m.levels.pop(); }
  else if (key === 'Enter') { if (lv.hover >= 0) return choose(ctx, k, lv.hover), true; }
  else return false;
  ctx.rebuild();
  return true;
}
// Ends an inline edit: text data for a textbox, the parsed value for a number, a cell for a list
// (an int cell must parse; a midi_map CC cell moves the assignment). False when refused.
export function playCommit(ctx, key, text, cell) {
  const st = ctx.st, it = findItem(ctx.root, key);
  if (!it) return false;
  if (it.w.type === 'textbox') { if (!it.dataKey) return false; st.text[it.dataKey] = String(text); }
  else if (it.w.type === 'list') {
    const [r, c] = cell || [], row = it.rows?.[r];
    if (!row) return false;
    if (it.w.columns?.[c]?.edit === 'int') { if (!/^\s*[-+]?\d+/.test(text)) return false; text = String(parseInt(text, 10)); }
    if (row.cc != null && c === 0) { const cc = clamp(+text, 0, 127), id = st.midiMap[row.cc]; delete st.midiMap[row.cc]; st.midiMap[cc] = id; }
    else if (row.fixedParam && c === 0) { if (+text >= 0) st.midiMap[clamp(+text, 0, 127)] = row.fixedParam; }
    else if (row.ccParam && c === 0) { const p = ctx.pmap.get(row.ccParam); if (p) setPlain(st, { w: {}, p, pid: row.ccParam }, +text); }
    else if (row.s != null) (st.cells[it.key] ||= {})[`${row.s},${c}`] = String(text);
  } else { const v = parseValueText(text, it.p); if (v == null) return false; setPlain(st, it, v); }
  ctx.rebuild();
  return true;
}
// Incoming MIDI (the editor simulates it): { cc, value } or { note, velocity } (velocity 0 = off).
// A CC while MIDI learn is on goes to the learn target; an assigned CC moves its param, and dials
// with that "midi" follow it; held notes show on the piano.
export function playMidiIn(ctx, msg) {
  const st = ctx.st;
  st.midiInAt = ctx.now;
  if (msg.note != null) {
    const n = msg.note | 0;
    st.notesIn = st.notesIn.filter(x => x !== n);
    if (+msg.velocity > 0) st.notesIn.push(n);
  } else {
    const cc = clamp(msg.cc | 0, 0, 127), t = clamp(+msg.value || 0, 0, 127) / 127;
    if (learn(ctx) && st.learn) { for (const k of Object.keys(st.midiMap)) if (st.midiMap[k] === st.learn) delete st.midiMap[k]; st.midiMap[cc] = st.learn; st.learn = null; }
    const id = st.midiMap[cc], p = id && ctx.pmap.get(id);
    if (p) setNorm(st, { w: {}, p, pid: id }, t);
    for (const it of treeItems(ctx.root)) if (it.w.type === 'dial' && it.w.midi === `cc${cc}`) setNorm(st, it, t);
  }
  ctx.rebuild();
}
// Animated flows: advances every growing or shrinking widget by its step (one 30 Hz frame) and
// keeps a growing one in view in an embed whose scroll has "follow". True while anything moves.
export function playFrame(ctx) {
  const st = ctx.st, growing = new Set();
  let moved = false;
  for (const it of treeItems(ctx.root)) if (it.anim) {
    const cur = st.anim[it.key], { tgt, step } = it.anim;
    if (cur === tgt) continue;
    moved = true;
    st.anim[it.key] = cur < tgt ? Math.min(tgt, cur + step) : Math.max(tgt, cur - step);
    if (cur < tgt) growing.add(it.key);
  }
  if (!moved) return false;
  ctx.rebuild();
  for (const e of treeItems(ctx.root)) if (e.bar && e.child && e.w.scroll?.follow) for (const it of e.child.items) if (growing.has(it.key)) {
    const bottom = it.rect[1] - e.rect[1] + e.bar.pos + st.anim[it.key];
    if (bottom > e.bar.pos + e.rect[3]) st.scroll[e.key] = Math.min(e.bar.max, bottom - e.rect[3]);
  }
  ctx.rebuild();
  return true;
}
export const playAnimating = ctx => [...treeItems(ctx.root)].some(it => it.anim && ctx.st.anim[it.key] !== it.anim.tgt);

// ---- JSON output ------------------------------------------------------------------------------------

const FIRST = ['type', 'name', 'rect', 'layer'];
export function orderedKeys(o) {
  const ks = Object.keys(o).filter(k => o[k] !== undefined);
  return 'type' in o ? [...FIRST.filter(k => ks.includes(k)), ...ks.filter(k => !FIRST.includes(k))] : ks;
}
function flat(v) {
  if (Array.isArray(v)) return '[' + v.map(flat).join(', ') + ']';
  if (isObj(v)) { const ks = orderedKeys(v); return ks.length ? '{ ' + ks.map(k => JSON.stringify(k) + ': ' + flat(v[k])).join(', ') + ' }' : '{}'; }
  return JSON.stringify(v) ?? 'null';
}

// Pretty JSON: 2-space indent, widget keys type, name, rect, layer first and the rest as they were,
// values that fit in `width` columns on one line, widgets and widget lists always expanded.
export function stringify(v, indent = '', lead = 0, width = 100) {
  const expand = !indent || (isObj(v) && 'type' in v) || (Array.isArray(v) && v.some(x => isObj(x) && 'type' in x));
  if (!expand) { const f = flat(v); if (indent.length + lead + f.length <= width) return f; }
  const inner = indent + '  ';
  if (Array.isArray(v)) return v.length ? '[\n' + v.map(x => inner + stringify(x, inner, 0, width)).join(',\n') + '\n' + indent + ']' : '[]';
  if (isObj(v)) {
    const ks = orderedKeys(v);
    return ks.length ? '{\n' + ks.map(k => { const kk = JSON.stringify(k) + ': '; return inner + kk + stringify(v[k], inner, kk.length, width); }).join(',\n') + '\n' + indent + '}' : '{}';
  }
  return JSON.stringify(v) ?? 'null';
}
