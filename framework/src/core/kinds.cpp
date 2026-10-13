// The custom widget kinds: drawing and input for each "kind" of a custom widget, behind KindOps
// (core.h). A new kind is a set of hooks here plus a line in findKind; editor.cpp calls the hooks.
// Also the scope drawing shared with plot widgets, and the "presets" action (template menus).
#include "core.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

namespace hollow {

// The widget's own state, created on first use.
template <class T> static T& kindState(Gui& g, const Hit& h) {
    std::unique_ptr<KindState>& ks = g.inst(h).ks;
    if (!ks || !dynamic_cast<T*>(ks.get())) ks.reset(new T);
    return *static_cast<T*>(ks.get());
}

static void pixel(Canvas& c, int x, int y, uint32_t col) { fillRect(c, {x, y, 1, 1}, col); }
static void hline(Canvas& c, int x, int y, int n, uint32_t col) { if (n > 0) fillRect(c, {x, y, n, 1}, col); }
static void vline(Canvas& c, int x, int y, int n, uint32_t col) { if (n > 0) fillRect(c, {x, y, 1, n}, col); }
static void frame(Canvas& c, int x, int y, int w, int h, uint32_t col, int k = 1) {   // a k px outline of the box
    fillRect(c, {x, y, w, k}, col);
    fillRect(c, {x, y + h - k, w, k}, col);
    fillRect(c, {x, y + k, k, h - 2 * k}, col);
    fillRect(c, {x + w - k, y + k, k, h - 2 * k}, col);
}
// A square dot k px wide centred on (x, y): a pixel at density 1.
static void dot(Canvas& c, int x, int y, uint32_t col, int k) { fillRect(c, {x - (k - 1) / 2, y - (k - 1) / 2, k, k}, col); }
static uint32_t colourField(const Widget& w, const char* key, uint32_t def) { return parseColour(w.json[key].str(), def); }

// Bresenham from (x0, y0) to (x1, y1), the end point not drawn, k px thick.
static void line(Canvas& c, int x0, int y0, int x1, int y1, uint32_t col, int k = 1) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    while (x0 != x1 || y0 != y1) {
        dot(c, x0, y0, col, k);
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

// A polyline through points in canvas pixels (centres at .5), `width` px wide, antialiased: each pixel takes
// the line's colour by how much of it the nearest segment covers, so joints never blend twice.
void drawPolyline(Canvas& c, const std::vector<std::pair<double, double>>& pts, uint32_t col, double width) {
    if (pts.size() < 2) return;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, half = width / 2;
    for (auto& p : pts) x0 = std::min(x0, p.first), y0 = std::min(y0, p.second), x1 = std::max(x1, p.first), y1 = std::max(y1, p.second);
    Rect box = Rect{(int)std::floor(x0 - half - 1), (int)std::floor(y0 - half - 1), 0, 0};
    box.w = (int)std::ceil(x1 + half + 1) - box.x + 1;
    box.h = (int)std::ceil(y1 + half + 1) - box.y + 1;
    box = box & c.clip;
    if (box.empty()) return;
    std::vector<float> cover((size_t)box.w * box.h, 0.0f);
    for (size_t s = 0; s + 1 < pts.size(); ++s) {
        double ax = pts[s].first, ay = pts[s].second, bx = pts[s + 1].first, by = pts[s + 1].second;
        double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        int sx0 = std::max(box.x, (int)std::floor(std::min(ax, bx) - half - 1)), sx1 = std::min(box.x + box.w - 1, (int)std::ceil(std::max(ax, bx) + half + 1));
        int sy0 = std::max(box.y, (int)std::floor(std::min(ay, by) - half - 1)), sy1 = std::min(box.y + box.h - 1, (int)std::ceil(std::max(ay, by) + half + 1));
        for (int y = sy0; y <= sy1; ++y)
            for (int x = sx0; x <= sx1; ++x) {
                double px = x + 0.5 - ax, py = y + 0.5 - ay;
                double t = len2 > 0 ? std::clamp((px * dx + py * dy) / len2, 0.0, 1.0) : 0;
                double d = std::hypot(px - t * dx, py - t * dy);
                float k = (float)std::clamp(half + 0.5 - d, 0.0, 1.0);
                float& m = cover[(size_t)(y - box.y) * box.w + (x - box.x)];
                m = std::max(m, k);
            }
    }
    for (int y = 0; y < box.h; ++y)
        for (int x = 0; x < box.w; ++x)
            if (float k = cover[(size_t)y * box.w + x]; k > 0)
                fillRect(c, {box.x + x, box.y + y, 1, 1}, (uint32_t)std::lround((col >> 24) * k) << 24 | (col & 0xffffff));
}

// A disc of radius rad centred at (cx, cy) in canvas pixels, antialiased.
static void disc(Canvas& c, double cx, double cy, double rad, uint32_t col) {
    if (!(col >> 24) || rad <= 0) return;
    for (int y = (int)std::floor(cy - rad - 1); y <= (int)std::ceil(cy + rad + 1); ++y)
        for (int x = (int)std::floor(cx - rad - 1); x <= (int)std::ceil(cx + rad + 1); ++x)
            if (double k = std::clamp(rad + 0.5 - std::hypot(x + 0.5 - cx, y + 0.5 - cy), 0.0, 1.0); k > 0)
                fillRect(c, {x, y, 1, 1}, (uint32_t)std::lround((col >> 24) * k) << 24 | (col & 0xffffff));
}

// A triangle filled in col, antialiased by 4 x 4 samples a pixel.
static void fillTriangle(Canvas& c, double ax, double ay, double bx, double by, double cx, double cy, uint32_t col) {
    auto side = [](double px, double py, double x0, double y0, double x1, double y1) { return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0); };
    for (int y = (int)std::floor(std::min({ay, by, cy})); y <= (int)std::ceil(std::max({ay, by, cy})); ++y)
        for (int x = (int)std::floor(std::min({ax, bx, cx})); x <= (int)std::ceil(std::max({ax, bx, cx})); ++x) {
            int in = 0;
            for (int s = 0; s < 16; ++s) {
                double px = x + (s % 4 + 0.5) / 4, py = y + (s / 4 + 0.5) / 4;
                double d0 = side(px, py, ax, ay, bx, by), d1 = side(px, py, bx, by, cx, cy), d2 = side(px, py, cx, cy, ax, ay);
                in += (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
            }
            if (in) fillRect(c, {x, y, 1, 1}, (uint32_t)((col >> 24) * in / 16) << 24 | (col & 0xffffff));
        }
}

// A curve's point handle: a dot in `fill` ringed in `ring` (the screen's colour, so the curve parts around
// it), and while it is dragged a halo of the fill.
static void handleDot(Gui& g, Canvas& c, double x, double y, uint32_t fill, uint32_t ring, bool held) {
    const double d = g.skin().density;
    if (held) disc(c, x, y, 5 * d, (fill & 0xffffff) | (fill >> 24) / 4 << 24);
    disc(c, x, y, 3 * d, ring);
    disc(c, x, y, 2 * d, fill);
}

static uint32_t mixColour(uint32_t a, uint32_t b, double t) {   // per channel, ARGB
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= (uint32_t)std::lround((a >> s & 255) + ((int)(b >> s & 255) - (int)(a >> s & 255)) * t) << s;
    return out;
}

// The area under a curve in column x from row top down to row base, its colour running from `area` at the
// plot's top row y0 to `area2` at its bottom row y1 (a fade; the same colour fills flat).
static void areaColumn(Canvas& c, int x, int top, int base, int y0, int y1, uint32_t area, uint32_t area2) {
    if (top > base) std::swap(top, base);
    if (area == area2) return vline(c, x, top, base - top, area);
    for (int y = top; y < base; ++y) fillRect(c, {x, y, 1, 1}, mixColour(area, area2, std::clamp(double(y - y0) / std::max(1, y1 - y0), 0.0, 1.0)));
}

// ---- scopes: plot widgets and the display kinds -------------------------------------------------

void drawScope(Canvas& c, Rect r, const std::vector<int>& v, int mode, uint32_t c1, uint32_t c2, int thick) {
    int W = std::min(r.w, (int)v.size()), H = r.h, bottom = r.y + r.h - 1;
    auto bars = [&](uint32_t col, bool centre) {
        for (int i = 0; i < W; ++i) {
            int h = std::min(v[i], H - 1);
            if (h < 0 || (h == 0 && !centre)) continue;
            int top = bottom - h, mid = bottom - H / 2;
            if (centre) vline(c, r.x + i, std::min(top, mid), std::abs(top - mid) + 1, col);
            else vline(c, r.x + i, top, h + 1, col);
        }
    };
    auto trace = [&](uint32_t col) {   // column to column, the end points left to the next segment
        auto y = [&](int i) { return std::clamp(bottom - v[i], r.y, bottom); };
        for (int i = 0; i + 1 < W; ++i) {
            if (v[i] < 0) continue;
            if (v[i + 1] < 0) dot(c, r.x + i, y(i), col, thick);   // a lone pixel before a gap
            else line(c, r.x + i, y(i), r.x + i + 1, y(i + 1), col, thick);
        }
    };
    if (mode == 1 || mode == 2) bars(c1, false);
    if (mode == 3) bars(c1, true);
    if (mode == 0) trace(c1);
    if (mode >= 2) trace(c2);
}

// ---- pad: a handle dragged in 2D over params [x, y] ----------------------------------------------

// Pad handles are 14 px. Shortcut: smaller pads get a quarter of their size.
static int handleSize(const Gui& g, Rect r) { return std::max(g.skin().dp(4), std::min(g.skin().dp(14), std::min(r.w, r.h) / 4)); }

static void padDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {   // an outlined red square kept inside the rect
    int hs = handleSize(g, r);
    int hx = r.x + (int)std::lround(g.norm(*h.node, h.i, 0) * (r.w - hs));
    int hy = r.y + r.h - hs - (int)std::lround(g.norm(*h.node, h.i, 1) * (r.h - hs));
    frame(c, hx, hy, hs, hs, 0xffb33733, g.skin().lw());
}

static void padDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool) {   // the handle's centre follows the pointer, y up
    int hs = handleSize(g, r);
    g.setNorm(h, (x - r.x - hs / 2) / double(std::max(r.w - hs, 1)), 0);
    g.setNorm(h, (r.y + r.h - (hs - hs / 2) - y) / double(std::max(r.h - hs, 1)), 1);
}

static bool padDown(Gui& g, const Hit& h, Rect r, int x, int y, bool shift) {
    g.begin(h);
    padDrag(g, h, r, x, y, shift);
    return true;
}

static void padUp(Gui& g, const Hit&, Rect, int, int) { g.end(); }

static void padReset(Gui& g, const Hit& h, Rect, int, int) {   // double-click: both params to their defaults
    g.begin(h);
    g.setPlain(h, g.defaultOf(h, 0), 0);
    g.setPlain(h, g.defaultOf(h, 1), 1);
    g.end();
}

// ---- morph_pad: params [x, y, jitter_x, jitter_y, seed], fields handle, colour, morphColour -------

struct MorphState : KindState {
    std::vector<std::array<int, 4>> corners;   // every param kept per morph corner (bl, br, tr, tl)
    bool built = false, morphable = false;
    int offX = 0, offY = 0;                    // handle centre minus pointer, at the press
};

// Whether any param kept per corner differs between the four corners.
static bool morphable(Gui& g, MorphState& st) {
    State& s = g.state();
    if (!st.built) {
        st.built = true;
        for (size_t i = 0; i < s.size(); ++i) {
            const std::string& id = s.def(i).id;
            if (id.size() < 4 || id.compare(id.size() - 3, 3, ".bl") != 0) continue;
            std::string base = id.substr(0, id.size() - 2);
            std::array<int, 4> q = {(int)i, s.indexOf(base + "br"), s.indexOf(base + "tr"), s.indexOf(base + "tl")};
            if (q[1] >= 0 && q[2] >= 0 && q[3] >= 0) st.corners.push_back(q);
        }
    }
    for (auto& q : st.corners) {
        double v = s.get(q[0]);
        if (s.get(q[1]) != v || s.get(q[2]) != v || s.get(q[3]) != v) return true;
    }
    return false;
}

struct MorphGeo {
    int s, cx0, cx1, cy0, cy1;
    double X, Y;
};

static MorphGeo morphGeo(Gui& g, const Hit& h, Rect r) {
    MorphGeo m;
    m.s = g.wid(h).json["handle"].integer(g.skin().dp(14));
    m.cx0 = r.x + m.s / 2;
    m.cx1 = r.x + r.w - (m.s - m.s / 2);
    m.cy0 = r.y + m.s / 2;
    m.cy1 = r.y + r.h - (m.s - m.s / 2);
    m.X = g.norm(*h.node, h.i, 0);
    m.Y = g.norm(*h.node, h.i, 1);
    return m;
}

// First the scatter: one white pixel per morphed parameter where its randomised position lands
// (seeded like the C runtime's rand), then the handle outline, red while the corners differ.
static void morphDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    MorphState& st = kindState<MorphState>(g, h);
    MorphGeo m = morphGeo(g, h, r);
    int x0 = r.x, x1 = r.x + r.w, y0 = r.y, y1 = r.y + r.h, s = m.s;
    auto param = [&](size_t k) { return k < w.params.size() ? g.paramOf(h, w.params[k]) : -1; };
    int jx = param(2), jy = param(3), sd = param(4);
    if (jx >= 0 && jy >= 0) {
        double ax = std::min({g.state().get(jx) / 100 * 0.5, m.X, 1 - m.X});
        double ay = std::min({g.state().get(jy) / 100 * 0.5, m.Y, 1 - m.Y});
        uint32_t rng = sd >= 0 ? (uint32_t)std::lround(g.state().get(sd)) : 0;
        auto rnd = [&] { rng = rng * 214013u + 2531011u; return (int)(rng >> 16 & 0x7fff); };
        for (int i = 0; i < 135; ++i) {
            double rx = rnd() * (2.0 / 32767) - 1, ry = rnd() * (2.0 / 32767) - 1;
            int x = (int)std::round((x1 - (x0 + s)) * (rx * ax + m.X) + (s / 2 + x0));
            int y = (int)std::round((y1 - s / 2) - ((y1 - s) - y0) * (ry * ay + m.Y));
            dot(c, x, y, 0xffffffff, g.skin().lw());
        }
    }
    st.morphable = morphable(g, st);
    uint32_t col = st.morphable ? colourField(w, "morphColour", 0xffb33733) : colourField(w, "colour", 0xffdbdbdb);
    int left = (int)std::round(m.cx0 + (m.cx1 - m.cx0) * m.X - s * 0.5), top = (int)std::round(m.cy1 - (m.cy1 - m.cy0) * m.Y - s * 0.5);
    frame(c, left, top, s, s, col, g.skin().lw());
}

static void morphTick(Gui& g, const Hit& h, Rect r) {   // the colour follows the corners
    MorphState& st = kindState<MorphState>(g, h);
    if (morphable(g, st) != st.morphable) g.invalidate(r);
}

// The handle moves relative to the press (it does not jump to the pointer).
static bool morphDown(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    MorphState& st = kindState<MorphState>(g, h);
    MorphGeo m = morphGeo(g, h, r);
    st.offX = (int)(m.cx0 + (m.cx1 - m.cx0) * m.X) - x;
    st.offY = (int)(m.cy1 - (m.cy1 - m.cy0) * m.Y) - y;
    g.begin(h);
    return true;
}

static void morphDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    MorphState& st = kindState<MorphState>(g, h);
    MorphGeo m = morphGeo(g, h, r);
    g.setNorm(h, (x + st.offX - m.cx0) / double(std::max(m.cx1 - m.cx0, 1)), 0);
    g.setNorm(h, (m.cy1 - (y + st.offY)) / double(std::max(m.cy1 - m.cy0, 1)), 1);
}

static void morphReset(Gui& g, const Hit& h, Rect, int, int) {   // double-click: the bottom-left corner
    g.begin(h);
    g.setPlain(h, 0, 0);
    g.setPlain(h, 0, 1);
    g.end();
}

// ---- piano: `count` keys from MIDI note `first`; plays notes into State's MIDI queue ---------------

static bool isBlack(int note) {
    int pc = (note % 12 + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// Key rects, white keys first, then the black keys over them. With keyImages the keys sit where the
// key art puts them: a 130 px octave, white keys at 0, 19, 38, 56, 75, 94, 112 and black keys (the
// black image's size) at 11, 33, 67, 87, 107 (times the skin's density). Without, white keys spread evenly and black keys are
// 0.7 of a white key wide, 0.59 tall and shifted per note to match the usual piano key art.
static std::vector<std::pair<int, Rect>> pianoKeys(const Gui& g, const Widget& w, Rect r) {
    std::vector<std::pair<int, Rect>> keys, black;
    if (w.keyImages[7] >= 0) {
        static const int whiteAt[12] = {0, -1, 19, -1, 38, 56, -1, 75, -1, 94, -1, 112};
        static const int blackAt[12] = {-1, 11, -1, 33, -1, -1, 67, -1, 87, -1, 107, -1};
        const Skin& sk = g.skin();
        Rect bt = sk.images[w.keyImages[7]].tile(0);
        int base = w.first - (w.first % 12 + 12) % 12;   // the C at or below the first key
        int shift = whiteAt[(w.first % 12 + 12) % 12] < 0 ? 0 : sk.dp(whiteAt[(w.first % 12 + 12) % 12]);
        for (int k = 0; k < w.count; ++k) {
            int note = w.first + k, pc = (note % 12 + 12) % 12, x = r.x + (note - pc - base) / 12 * sk.dp(130) - shift;
            if (isBlack(note)) {
                black.push_back({note, {x + sk.dp(blackAt[pc]), r.y, bt.w, bt.h}});
            } else {
                int next = sk.dp(pc == 11 ? 130 : whiteAt[pc + 1 + (whiteAt[pc + 1] < 0)]);
                keys.push_back({note, {x + sk.dp(whiteAt[pc]), r.y, next - sk.dp(whiteAt[pc]), r.h}});
            }
        }
    } else {
        static const double shift[12] = {0, -0.057, 0, 0.128, 0, 0, -0.093, 0, 0.038, 0, 0.116, 0};
        int whites = 0;
        for (int k = 0; k < w.count; ++k) whites += !isBlack(w.first + k);
        double kw = whites ? double(r.w) / whites : 0;
        for (int k = 0, i = 0; k < w.count && whites; ++k) {
            int note = w.first + k, edge = r.x + (int)std::lround(i * kw);
            if (isBlack(note)) {
                int bw = std::max(1, (int)std::lround(kw * 0.7));
                int cx = r.x + (int)std::lround((i + shift[(note % 12 + 12) % 12]) * kw);
                black.push_back({note, {cx - bw / 2, r.y, bw, (int)std::lround(r.h * 0.59)}});
            } else {
                ++i;
                keys.push_back({note, {edge, r.y, r.x + (int)std::lround(i * kw) - edge, r.h}});
            }
        }
    }
    keys.insert(keys.end(), black.begin(), black.end());
    return keys;
}

struct PianoState : KindState {
    uint64_t shown[2] = {0, 0};   // the pressed keys at the last paint
};

static bool pianoDown(Gui&, const Hit&, Rect, int, int, bool);

// Pressed keys: the one held by the mouse and those incoming MIDI holds. With keyImages each is
// tile 1 of its image at its place; without, a highlight (with a fill the widget draws its own keys,
// without one only the pressed keys over the art beneath).
static void pianoDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    PianoState& st = kindState<PianoState>(g, h);
    st.shown[0] = st.shown[1] = 0;
    int held = g.pressed(h) ? g.held : -1;
    auto keys = pianoKeys(g, w, r);
    auto lit = [&](int note) { return note == held || g.state().noteHeld(note); };
    for (auto& k : keys)
        if (lit(k.first)) st.shown[k.first >> 6] |= 1ull << (k.first & 63);
    if (w.keyImages[7] >= 0) {   // every key from its picture: tile 1 while pressed
        for (auto& k : keys) {   // white keys come first in the list, the black keys over them
            int pc = (k.first % 12 + 12) % 12;
            static const int whiteImage[12] = {0, 7, 1, 7, 2, 3, 7, 4, 7, 5, 7, 6};
            // the top C has no black key on its right, and "low", the first key, none on its left (an 88's A0)
            int img = w.keyImages[pc == 0 && k.first == w.first + w.count - 1 && w.keyImages[8] >= 0 ? 8
                                  : k.first == w.first && !isBlack(k.first) && w.keyImages[9] >= 0 ? 9 : whiteImage[pc]];
            if (img < 0) continue;
            int tile = lit(k.first) ? 1 : 0;
            Rect t = g.skin().images[img].tile(tile);
            drawImage(c, g.skin().images[img], tile, {k.second.x, k.second.y, t.w, t.h});
        }
        return;
    }
    bool own = w.fill.image >= 0 || w.fill.hasColour;
    for (auto& k : keys) {
        bool on = lit(k.first);
        if (isBlack(k.first)) {
            if (own || on) fillRect(c, k.second, !on ? 0xff1f1f1f : own ? 0xff5078b0 : 0x806088c0);
        } else if (own) {
            fillRect(c, k.second, on ? 0xffa8c8f0 : 0xffffffff);
            fillRect(c, {k.second.x + k.second.w - g.skin().dp(2), k.second.y, g.skin().dp(2), k.second.h}, 0xff3c3c3c);
        } else if (on) {   // keep the black keys beside it untinted
            Rect top = k.second;
            for (auto& b : keys)
                if (isBlack(b.first) && b.second.x < top.x + top.w && b.second.x + b.second.w > top.x) {
                    top.h = b.second.h;
                    if (b.second.x <= top.x) { top.w -= b.second.x + b.second.w - top.x; top.x = b.second.x + b.second.w; }
                    else top.w = b.second.x - top.x;
                }
            fillRect(c, top, 0x806088c0);
            fillRect(c, {k.second.x, k.second.y + top.h, k.second.w, k.second.h - top.h}, 0x806088c0);
        }
    }
}

static void pianoTick(Gui& g, const Hit& h, Rect r) {   // keys held by incoming MIDI
    PianoState& st = kindState<PianoState>(g, h);
    uint64_t now[2] = {0, 0};
    for (int n = 0; n < 128; ++n)
        if (g.state().noteHeld(n) || (g.pressed(h) && n == g.held)) now[n >> 6] |= 1ull << (n & 63);
    if (now[0] != st.shown[0] || now[1] != st.shown[1]) g.invalidate(r);
}

static void noteOff(Gui& g) {
    if (g.held < 0) return;
    uint8_t m[3] = {0x80, (uint8_t)g.held, 0};
    g.state().pushMidi(m, 3);
    g.held = -1;
}

// The key under the pointer sounds, with the fixed "velocity" or one from the height of the press
// on the key (1 at its top edge to 127 at its bottom). With "glide" (the default) dragging plays
// the keys it crosses; without, the first key keeps the pointer.
static void pianoDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    const Widget& w = g.wid(h);
    if (!w.glide && g.held >= 0) return;
    int note = -1;
    Rect key;
    auto keys = pianoKeys(g, w, r);
    for (auto k = keys.rbegin(); k != keys.rend() && note < 0; ++k)
        if (k->second.contains(x, y) && k->first >= 0 && k->first < 128) { note = k->first; key = k->second; }
    if (note == g.held) return;
    noteOff(g);
    if (note >= 0) {
        int vel = w.velocity > 0 ? w.velocity : std::clamp((int)std::lround(127.0 * (y - key.y + 1) / key.h), 1, 127);
        uint8_t m[3] = {0x90, (uint8_t)note, (uint8_t)vel};
        g.state().pushMidi(m, 3);
        g.held = note;
    }
    g.invalidate(r);
}

static bool pianoDown(Gui& g, const Hit& h, Rect r, int x, int y, bool shift) {
    g.held = -1;
    pianoDrag(g, h, r, x, y, shift);
    return true;
}

static void pianoUp(Gui& g, const Hit&, Rect r, int, int) {
    noteOff(g);
    g.invalidate(r);
}

// ---- waveform: params [wave, invert]; the operator wave from data/waveforms.json -----------------

// W columns across one cycle, linearly interpolated from the 128-point table, scaled by 0.952381 and
// drawn as a trace in the widget's line colour.
static void waveDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Json& waves = g.skin().table("waveforms")["waves"];
    const Inst& s = g.inst(h);
    int wave = s.param >= 0 ? (int)std::lround(g.plain(*h.node, h.i, 0)) : 0;
    bool invert = s.param2 >= 0 && g.plain(*h.node, h.i, 1) > 0.5;
    const Json& wv = waves[(size_t)std::max(wave, 0)];
    int n = (int)wv.size(), W = r.w, H = r.h;
    if (n < 2 || W < 2) return;
    std::vector<int> col((size_t)W);
    for (int i = 0; i < W; ++i) {
        double p = (double)i / W * n;
        int k = (int)p;
        double a = wv[(size_t)k].num(), v = a + (p - k) * (wv[(size_t)((k + 1) % n)].num() - a);
        int y = (int)((v * 0.95238101 + 1.0) * ((H - 1) * 0.5));
        col[(size_t)i] = invert ? (H - 1) - y : y;
    }
    drawScope(c, r, col, 0, g.wid(h).line, 0, g.skin().lw());
}

// ---- spectrum, spectrum_wave: the sound at middle C ----------------------------------------------

// A synth would render the patch in its engine (16 periods of 128 samples at middle C, with the
// FM matrix). The kind has no engine, so this is an approximation: the sum of each
// operator's table wave (a..f, at its ratio and offset) times its output amount, without
// modulation between operators and without X and Z.
struct SpectrumState : KindState {
    std::vector<double> sig;   // the inputs at the last render
    std::vector<int> v;        // one height per column
};

static void renderSound(Gui& g, const Hit& h, std::vector<double>& sig, std::vector<double>& out) {
    const Json& waves = g.skin().table("waveforms")["waves"];
    static const char* ops[6] = {"a", "b", "c", "d", "e", "f"};
    out.assign(2048, 0.0);
    sig.clear();
    for (const char* op : ops) {
        auto get = [&](const std::string& id, double def) {
            int p = g.paramOf(h, id);
            return p >= 0 ? g.state().get(p) : def;
        };
        std::string o = op;
        double amount = get("matrix." + o + ".out.{timbre}", 0) / 100.0, on = get("op." + o + ".on.{timbre}", 1);
        double ratio = get("op." + o + ".ratio.{timbre}", 1), offset = get("op." + o + ".offset.{timbre}", 0);
        int wave = (int)std::lround(get("op." + o + ".wave.{timbre}", 0));
        sig.insert(sig.end(), {amount, on, ratio, offset, (double)wave});
        const Json& wv = waves[(size_t)std::max(wave, 0)];
        int n = (int)wv.size();
        if (amount == 0 || on < 0.5 || n < 2) continue;
        double step = (ratio + offset / 261.63) * n / 128.0;   // table points per sample
        for (int i = 0; i < 2048; ++i) {   // measured: the phase advances before the first sample
            double p = std::fmod((i + 1) * step, (double)n);
            int k = (int)p;
            double a = wv[(size_t)k].num();
            out[(size_t)i] += amount * (a + (p - k) * (wv[(size_t)((k + 1) % n)].num() - a));
        }
    }
}

// Bars, one per FFT bin (harmonic n at column 16 n, times the density): the power's natural log times 0.1085725 of the height.
static void spectrumTick(Gui& g, const Hit& h, Rect r) {
    SpectrumState& st = kindState<SpectrumState>(g, h);
    std::vector<double> sig, s;
    renderSound(g, h, sig, s);
    if (sig == st.sig && !st.v.empty()) return;
    st.sig = sig;
    st.v.assign((size_t)std::max(r.w, 0), 0);
    const double d = g.skin().density;
    for (int col = 0, last = -1; col < r.w; ++col) {
        int j = (int)(col / d);   // the bin this column shows: every column at density 1
        if (j == last) { st.v[(size_t)col] = st.v[(size_t)col - 1]; continue; }
        last = j;
        double re = 0, im = 0;
        for (int i = 0; i < 2048; ++i) {
            double a = 2 * 3.14159265358979323846 * (double)((long long)j * i % 2048) / 2048;
            re += s[(size_t)i] * std::cos(a);
            im -= s[(size_t)i] * std::sin(a);
        }
        double pw = (re * 0.25) * (re * 0.25) + (im * 0.25) * (im * 0.25);
        st.v[(size_t)col] = std::clamp((int)(std::log(pw > 0 ? pw : 1e-7) * 0.1085725 * r.h), 0, r.h - 1);
    }
    g.invalidate(r);
}

static void spectrumDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    SpectrumState& st = kindState<SpectrumState>(g, h);
    if (st.v.empty()) spectrumTick(g, h, r);
    drawScope(c, r, st.v, 1, g.wid(h).line, 0);
}

// Two periods across the width, peak-normalised and centred; values outside the rect leave a gap.
static void spectrumWaveTick(Gui& g, const Hit& h, Rect r) {
    SpectrumState& st = kindState<SpectrumState>(g, h);
    std::vector<double> sig, s;
    renderSound(g, h, sig, s);
    if (sig == st.sig && !st.v.empty()) return;
    st.sig = sig;
    int W = r.w, H = r.h;
    double hi = *std::max_element(s.begin(), s.end()), lo = *std::min_element(s.begin(), s.end()), scale = 1e300;
    if (hi > 0.0001) scale = std::min(scale, (H - 1 - H / 2) / hi);
    if (lo < -0.0001) scale = std::min(scale, (-(H / 2)) / lo);
    st.v.assign((size_t)std::max(W, 0), -1);
    if (scale < 1e300)
        for (int i = 0; i < W; ++i) {
            double pos = (double)i * 256 / W;
            int j = (int)std::round(pos - 0.5);
            double fr = pos - j;
            int v = (int)std::round((fr * s[(size_t)(j + 1) % 2048] + (1 - fr) * s[(size_t)j % 2048]) * scale) + H / 2;
            st.v[(size_t)i] = v >= 0 && v < H ? v : -1;
        }
    g.invalidate(r);
}

static void spectrumWaveDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    SpectrumState& st = kindState<SpectrumState>(g, h);
    if (st.v.empty()) spectrumWaveTick(g, h, r);
    drawScope(c, r, st.v, g.wid(h).drawMode, g.wid(h).line, g.wid(h).line2, g.skin().lw());
}

// ---- matrix_wires: the FM matrix's connection lines -----------------------------------------------

static const char* kSources[9] = {"a", "b", "c", "d", "e", "f", "x", "z", "in"};
static const char* kTargets[11] = {"a", "b", "c", "d", "e", "f", "x", "z", "in", "out", "pan"};

struct WiresState : KindState {
    std::vector<double> sig;
};

static std::vector<double> wireInputs(Gui& g, const Hit& h) {   // 9 x 11 amounts, then the 9 on switches
    const Json& j = g.wid(h).json;
    if (j.has("algorithm") || j.has("number")) {   // fixed algorithms: the wires of the one the param (or number) picks, from a table
        std::vector<double> v(9 * 11 + 9, 0.0);
        int first = j["first"].integer(1), p = j.has("number") ? -1 : g.paramOf(h, j["algorithm"].str());
        int a = j.has("number") ? j["number"].integer() - first : p >= 0 ? (int)std::lround(g.state().get(p)) - first : 0;
        const Json& t = g.skin().table(j["table"].str("algorithms"))[(size_t)std::max(a, 0)];
        auto set = [&](int src, int dst) {   // operators 1..8
            if (src >= 1 && src <= 8 && dst >= 1 && dst <= 10) v[(size_t)((src - 1) * 11 + dst - 1)] = 1;
        };
        for (auto& e : t["mod"].items) set(e[0].integer(), e[1].integer());
        if (t["fb"].size() == 2) set(t["fb"][0].integer(), t["fb"][1].integer());
        for (auto& o : t["out"].items) set(o.integer(), 10);
        for (int i = 0; i < 8; ++i) v[(size_t)(99 + i)] = 1;
        return v;
    }
    std::vector<double> v;
    for (int i = 0; i < 9; ++i)
        for (int j = 0; j < 11; ++j) {
            int p = g.paramOf(h, std::string("matrix.") + kSources[i] + "." + kTargets[j] + ".{timbre}");
            v.push_back(p >= 0 ? g.state().get(p) : 0);
        }
    for (int i = 0; i < 9; ++i) {
        int p = g.paramOf(h, i == 8 ? std::string("master.input_on.{timbre}") : std::string("op.") + kSources[i] + ".on.{timbre}");
        v.push_back(p >= 0 ? g.state().get(p) : 0);
    }
    return v;
}

// Text centred on its ink rather than on its cells, which carry blank rows and a spacing column.
static void inkCentred(Canvas& c, const Font& f, const std::string& s, Rect b) {
    int top = f.height, bottom = -1, left = b.w, right = -1, pen = 0;
    for (unsigned char ch : s) {   // single-byte text only (operator numbers)
        int x = ((pen + 32) >> 6) + f.ox[ch];
        for (int y = 0; y < f.height; ++y)
            for (int i = 0; i < f.w[ch]; ++i)
                if (f.img.px[(size_t)(f.top + y) * (size_t)f.img.w + (size_t)(f.x[ch] + i)] >> 24) {
                    top = std::min(top, y), bottom = std::max(bottom, y);
                    left = std::min(left, x + i), right = std::max(right, x + i);
                }
        pen += f.adv[ch];
    }
    if (bottom < 0) return;
    drawText(c, f, s, {b.x + (b.w - (right - left + 1)) / 2 - left, b.y + (b.h - (bottom - top + 1)) / 2 - top, (pen + 32) >> 6, f.height}, 0, 0, false);
}

// In matrix-local design units (scaled by the skin's density as they are drawn), operator box n
// (0-based, the n-th source and destination) is centred on (25 + 28 n, 32.67 + 28 n), where the
// operator art's visible box is, 16.67 x 13.33 ("boxSize" changes it); a source's feedback amount
// sits above it, centred at y 18 + 28 n, and its output amount on the bus, y 284.67. A wire leaves a
// box straight down or up its column to the destination's row, then runs along the row to the box's
// edge with an arrow, unbroken: the amount tags of a skin draw over it. Carriers run down into the
// bus, which starts at the first of them. A source's lines are light while it is on and grey while
// off; the bus is always light. "scale" shrinks the whole drawing (a thumbnail), and "box" draws the
// boxes in that colour, numbered in the text font if there is one, where no operator art sits over.
static void wiresDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    WiresState& st = kindState<WiresState>(g, h);
    st.sig = wireInputs(g, h);
    const Widget& w = g.wid(h);
    const double s = g.skin().density * w.json["scale"].num(1);
    auto D = [&](double v) { return (int)std::lround(v * s); };   // the skin's dp() at scale 1
    const Json& size = w.json["boxSize"];
    const double hw = size[0].num(16.67) / 2, hh = size[1].num(13.33) / 2, busY = 284.67, busEnd = 268;
    const int k = std::max(1, D(1)), head = std::max(D(2), std::clamp(D(2 * hh) / 2, 2, 3));   // no taller than a box
    auto nz = [&](int i, int j) { return st.sig[(size_t)(i * 11 + j)] != 0; };
    auto cx = [&](int n) { return 25.0 + 28 * n; };
    auto cy = [&](int n) { return 32.67 + 28 * n; };
    auto X = [&](double u) { return r.x + D(u) - k / 2; };   // a line centred on unit u
    auto Y = [&](double u) { return r.y + D(u) - k / 2; };
    const uint32_t light = 0xffdcdee4, grey = 0xff808080;
    uint32_t col = light;
    auto hl = [&](int x0, int x1, int y) { fillRect(c, {std::min(x0, x1), y, std::abs(x1 - x0) + k, k}, col); };
    auto vl = [&](int x, int y0, int y1) { fillRect(c, {x, std::min(y0, y1), k, std::abs(y1 - y0) + k}, col); };
    auto arrow = [&](int from, int tip, int y, int dir) {   // along y to the tip pixel, pointing dir (1 right, -1 left)
        hl(from, tip - (dir > 0 ? k - 1 : 0), y);
        for (int d = 0; d < head; ++d) fillRect(c, {tip - dir * d, y - 1 - d, 1, k + 2 + 2 * d}, col);
    };
    for (int i = 0; i < 9; ++i)   // the bus, from the first carrier
        if (nz(i, 9)) {
            arrow(X(cx(i)), r.x + D(busEnd) - 1, Y(busY), 1);
            break;
        }
    for (int i = 0; i < 9; ++i)
        for (int j = 0; j < 11; ++j) {
            if (!nz(i, j) || j == 10) continue;   // pan draws no wire
            col = st.sig[(size_t)(99 + i)] != 0 ? light : grey;
            int x = X(cx(i));
            if (j == 9) {
                vl(x, Y(cy(i)), Y(busY));
            } else if (j != i) {   // down or up to the destination's row, then along it into the box
                vl(x, Y(cy(i)), Y(cy(j)));
                if (j > i) arrow(x, r.x + D(cx(j) - hw) - 1, Y(cy(j)), 1);
                else arrow(x, r.x + D(cx(j) + hw), Y(cy(j)), -1);
            } else {   // feedback: up through the amount, round the right side and back in
                double top = 18 + 28 * i, side = cx(i) + std::max(14.0, hw + 7);
                vl(x, Y(cy(i)), Y(top));
                hl(x, X(side), Y(top));
                vl(X(side), Y(top), Y(cy(i)));
                arrow(X(side), r.x + D(cx(i) + hw), Y(cy(i)), -1);
            }
        }
    if (!w.json.has("box")) return;
    for (int i = 0; i < 8; ++i) {
        Rect b{r.x + D(cx(i) - hw), r.y + D(cy(i) - hh), D(cx(i) + hw) - D(cx(i) - hw), D(cy(i) + hh) - D(cy(i) - hh)};
        fillRect(c, b, colourField(w, "box", 0xff6b7070));
        if (w.text.font >= 0) inkCentred(c, g.skin().fonts[w.text.font], std::to_string(i + 1), b);
    }
}

static void wiresTick(Gui& g, const Hit& h, Rect r) {
    if (wireInputs(g, h) != kindState<WiresState>(g, h).sig) g.invalidate(r);
}

// ---- matrix_op: an operator box; fields image, letter, param (on/off), action (goto), bypass ------

// Tile (selected ? 1 : 0) + (off ? 2 : 0): selected while var op names this box (never "in"). With a text
// font the box's letter is drawn over it, centred on its ink in the face above the art's 2 px foot.
static void boxDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    if (w.image < 0) return;
    std::string letter = w.json["letter"].str();
    bool selected = letter != "in" && !letter.empty() && g.subst(*h.node, "{op}") == letter;
    bool off = g.inst(h).param >= 0 && g.plain(*h.node, h.i) == 0;
    const Image& img = g.skin().images[w.image];
    Rect t = img.tile(0);
    drawImage(c, img, (selected ? 1 : 0) + (off ? 2 : 0), {r.x, r.y, t.w, t.h});
    if (w.text.font >= 0 && !letter.empty()) inkCentred(c, g.skin().fonts[w.text.font], letter, {r.x, r.y, t.w, t.h - g.skin().dp(2)});
}

static bool boxDown(Gui& g, const Hit& h, Rect, int, int, bool) {   // its page (also on a double-click's second press)
    if (g.wid(h).json["letter"].str() != "in") g.runAction(*h.node, h.i, g.wid(h).action);
    return false;
}

static void boxDbl(Gui& g, const Hit& h, Rect r, int x, int y) { boxDown(g, h, r, x, y, false); }

// Right release: on/off at the current corner; with Shift on a box that has "bypass" and is on,
// that bypass switch instead.
static void boxRight(Gui& g, const Hit& h, Rect r, int, int, bool shift, bool up) {
    if (!up) return;
    const Widget& w = g.wid(h);
    int on = g.inst(h).param, bypass = g.paramOf(h, w.json["bypass"].str());
    bool isOn = on >= 0 && g.state().get(on) != 0;
    if (shift && bypass >= 0 && isOn) {
        g.setParam(bypass, g.state().get(bypass) != 0 ? 0 : 1);
    } else if (on >= 0) {   // through the widget, so "editAll" and "mirror" apply
        g.begin(h);
        g.setPlain(h, isOn ? 0 : 1);
        g.end();
    }
    g.invalidate(r);
}

// ---- envelope --------------------------------------------------------------------------------------

// A breakpoint envelope kept as text data under the widget's key: "<loopStart> <loopEnd>|<dt> <level>
// <slope>|..." (dt in ms from the previous point, level 0..1 or -1..1 for a key ending in "pitch",
// slope 0.05..0.999 shaping the segment that ends at the point). It starts at time 0 at the level of
// its last point. Fields: strip, graph ("plot" or "control"), handleOffset, labels ("centre"),
// static, view ("shared": envui.zoom / envui.scroll, or "own"); param: tempo sync; text.font: the
// time labels. Internal params envui.* mirror the selection for the readouts.
struct Env {
    std::vector<double> dt, level, slope;
    int loopStart = 0, loopEnd = 1;
    size_t count() const { return dt.size(); }
    double cum(int i) const {   // absolute time of breakpoint i (-1: 0)
        double t = 0;
        for (int k = 0; k <= i && k < (int)dt.size(); ++k) t += dt[(size_t)k];
        return t;
    }
    double total() const { return cum((int)dt.size() - 1); }
};

static Env defaultEnv(bool pitch) {
    Env e;
    if (pitch) e.dt = {100, 500, 500}, e.level = {0, 0, 0}, e.slope = {0.5, 0.9, 0.9};
    else e.dt = {1, 500, 1}, e.level = {1, 1, 0}, e.slope = {0.5, 0.9, 0.9};
    return e;
}

static bool parseEnv(const std::string& s, Env& e) {
    Env r;
    const char* p = s.c_str();
    const char* end = p + s.size();
    auto num = [&](double& v) {
        while (p < end && *p == ' ') ++p;
        return parseNumber(p, end, v);
    };
    double ls, le;
    if (!num(ls) || !num(le)) return false;
    r.loopStart = (int)ls;
    r.loopEnd = (int)le;
    while (p < end && *p == '|') {
        ++p;
        double dt, level, slope;
        if (!num(dt) || !num(level) || !num(slope)) return false;
        r.dt.push_back(dt);
        r.level.push_back(level);
        r.slope.push_back(slope);
    }
    if (r.count() < 2 || r.count() > 32) return false;
    e = r;
    return true;
}

static std::string formatEnv(const Env& e) {
    std::string s = std::to_string(e.loopStart) + " " + std::to_string(e.loopEnd);
    for (size_t i = 0; i < e.count(); ++i)
        s += "|" + formatNumber("%.7g", e.dt[i]) + " " + formatNumber("%.7g", e.level[i]) + " " + formatNumber("%.7g", e.slope[i]);
    return s;
}

// The loop is kept inside the points: 0 <= loopStart < loopEnd <= count - 2.
static void normaliseLoop(Env& e) {
    int lo = e.loopStart, hi = e.loopEnd, count = (int)e.count();
    if (hi < lo) std::swap(lo, hi);
    if (hi >= count - 1) hi = count - 2;
    if (hi <= lo) {
        lo = hi - 1;
        if (hi < 1) { lo = hi; hi = hi + 1; }
    }
    e.loopStart = std::max(lo, 0);
    e.loopEnd = std::max(hi, 0);
}

static double envSlopeCurve(double slope) {   // the curve value at half time; 0.5 is linear
    double s = std::clamp(slope, 0.05, 0.999);
    return s <= 0.5 ? 2 * s * s : (4 - 2 * s) * s - 1;
}

enum EnvDrag { DragNone, DragPoint, DragLevel, DragMarker, DragSlope, DragView };

struct EnvState : KindState {
    Env env;
    std::string key, seen;        // the key and data text last read or written
    int sel = 0;                  // selected breakpoint, -1 none
    double m = 4, o = 0;          // own view: ms per pixel, scroll in ms
    EnvDrag drag = DragNone;
    int dragPoint = -1, lastX = 0, lastY = 0, lastMarker = -1;
    double anchor = 0;
    double shown[6] = {-1e300, -1e300, -1e300, -1e300, -1e300, -1e300};   // envui values last written
};

struct EnvGeo {
    int Rx0, Ry0, Rx1, Ry1, Px0, Py0, Px1, Py1, Gx0, Gy0, Gx1, Gy1, Sy0, Sy1;
    int hx, hy;
    bool centre, isStatic, pitch, shared;
    double lmin, lmax;
    double d = 1;   // the skin's density: sizes below are design pixels times d
    int k = 1;      // line width
    int D(double v) const { return (int)std::lround(v * d); }
};

static const char* kEnvUi[] = {"envui.point", "envui.count", "envui.abs_time", "envui.rel_time", "envui.level", "envui.slope"};

static EnvGeo envGeo(Gui& g, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    EnvGeo e;
    e.d = g.skin().density;
    e.k = g.skin().lw();
    e.Rx0 = r.x; e.Ry0 = r.y; e.Rx1 = r.x + r.w; e.Ry1 = r.y + r.h;
    bool strip = w.json["strip"].flag(true);
    int in = strip ? e.D(3) : 0;
    e.Px0 = e.Rx0 + in; e.Py0 = e.Ry0 + in; e.Px1 = e.Rx1 - in; e.Py1 = e.Ry1 - in;
    if (w.json["graph"].str() == "control") { e.Gx0 = e.Rx0; e.Gy0 = e.Ry0; e.Gx1 = e.Rx1; e.Gy1 = e.Ry1; }
    else { e.Gx0 = e.Px0; e.Gy0 = e.Py0 - e.D(2); e.Gx1 = e.Px1; e.Gy1 = e.Py1; }
    int mid = e.Py0 + (e.Py1 - e.Py0) / 2;
    e.Sy0 = mid - e.D(9);
    e.Sy1 = mid + e.D(10);
    e.hx = w.json["handleOffset"][0].integer(e.D(1));
    e.hy = w.json["handleOffset"][1].integer(0);
    e.centre = w.json["labels"].str() == "centre";
    e.isStatic = w.json["static"].flag();
    e.shared = w.json["view"].str("shared") == "shared" && g.state().indexOf("envui.zoom") >= 0;
    const std::string& key = g.inst(h).key;
    e.pitch = key.size() >= 5 && key.compare(key.size() - 5, 5, "pitch") == 0;
    e.lmin = e.pitch ? -1 : 0;
    e.lmax = 1;
    return e;
}

static void viewOf(Gui& g, const EnvGeo& e, EnvState& st, double& m, double& o) {
    if (e.shared) {
        m = g.state().get(g.state().indexOf("envui.zoom"));
        o = g.state().get(g.state().indexOf("envui.scroll"));
    } else {
        m = st.m;
        o = st.o;
    }
    m = std::clamp(m, 0.1, 999999.0);
    o = std::max(o, 0.0);
}

static void setView(Gui& g, const EnvGeo& e, EnvState& st, double m, double o) {
    m = std::clamp(m, 0.1, 999999.0);
    o = std::max(o, 0.0);
    if (e.shared) {
        g.setParam(g.state().indexOf("envui.zoom"), m);
        g.setParam(g.state().indexOf("envui.scroll"), o);
    } else {
        st.m = m;
        st.o = o;
    }
}

// The widget's envelope, re-read when its data changed from elsewhere (a load, a preset, var op).
static EnvState& envState(Gui& g, const Hit& h) {
    EnvState& st = kindState<EnvState>(g, h);
    const std::string& key = g.inst(h).key;
    std::string data = g.state().hasData(key) ? g.state().data(key) : std::string("\x01");
    if (key != st.key || data != st.seen) {
        st.key = key;
        st.seen = data;
        bool pitch = key.size() >= 5 && key.compare(key.size() - 5, 5, "pitch") == 0;
        if (!parseEnv(data, st.env)) st.env = defaultEnv(pitch);
        st.sel = std::min(st.sel, (int)st.env.count() - 1);
    }
    return st;
}

static double tempoGrid(double m) {   // the beat grid step in ms: 500 ms a beat, 1/sub of a whole note
    double q = 500 / m;
    if (q <= 16) {
        while (q < 8) q *= 2;
    } else {
        for (int k = 0; k < 5; ++k) {
            q *= 0.5;
            if (q <= 16) break;
        }
    }
    return q * m;
}

static int tempoSub(double m) {
    double q = 500 / m, sub = 4;
    if (q <= 16) {
        while (q < 8) { sub *= 0.5; q *= 2; }
    } else {
        static const int subs[5] = {8, 16, 32, 64, 128};
        for (int k = 0; k < 5; ++k) {
            sub = subs[k];
            q *= 0.5;
            if (q <= 16) break;
        }
    }
    return (int)sub;
}

// Writes the envelope back under its key and the selection into the envui readouts.
static void envCommit(Gui& g, const Hit& h, EnvState& st, bool data) {
    State& s = g.state();
    if (data) {
        st.seen = formatEnv(st.env);
        s.setData(st.key, st.seen);
    }
    if (g.wid(h).json["static"].flag()) return;
    int sel = st.sel;
    double v[6] = {sel >= 0 ? sel + 1.0 : 0.0, (double)st.env.count(), sel >= 0 ? st.env.cum(sel) / 1000 : 0,
                   sel >= 0 ? st.env.dt[(size_t)sel] / 1000 : 0, sel >= 0 ? st.env.level[(size_t)sel] : 0,
                   sel >= 0 ? st.env.slope[(size_t)sel] : 0};
    for (int k = 0; k < 6; ++k) {
        int p = s.indexOf(kEnvUi[k]);
        if (p < 0) continue;
        s.set((size_t)p, v[k]);
        st.shown[k] = s.get((size_t)p);
    }
}

// Point drag and the rel_time readout: SLD moves the later points with it, FIX keeps them in place.
static void envSetTime(Gui& g, EnvState& st, int i, double dtNew) {
    Env& e = st.env;
    int slide = g.state().indexOf("envui.slide");
    bool sld = slide < 0 || g.state().get(slide) != 0;
    if (sld) {
        e.dt[(size_t)i] = std::min(dtNew, 999999.0);
        return;
    }
    double delta = e.dt[(size_t)i] - dtNew;
    if (i + 1 < (int)e.count()) {
        double next = std::clamp(e.dt[(size_t)i + 1] + delta, 1.0, 999999.0);
        delta = next - e.dt[(size_t)i + 1];
        e.dt[(size_t)i + 1] = next;
    }
    e.dt[(size_t)i] = std::clamp(e.dt[(size_t)i] - delta, 1.0, 999999.0);
}

// The fitted zoom: the longest of the nine envelopes (shared view) or this one over the plot width.
static double fitZoom(Gui& g, const Env& e, int plotW, bool shared) {
    double total = e.total();
    if (shared)
        for (const char* op : {"a", "b", "c", "d", "e", "f", "x", "z", "pitch"}) {
            std::string key = std::string("env.") + op;
            Env other;
            if (!g.state().hasData(key) || !parseEnv(g.state().data(key), other)) other = defaultEnv(std::string(op) == "pitch");
            total = std::max(total, other.total());
        }
    return total / std::max(plotW - 1, 1);
}

namespace {
// Pixel mappings of one envelope in one view; m is per device pixel (the view's zoom over the density).
struct EnvMap {
    const EnvGeo& e;
    double m, o;
    int px(double t) const { return (int)(e.Px0 + (t - o) / m); }
    int py(double l) const { return (int)((e.Py1 - 1) - (l - e.lmin) * (e.Py1 - 1 - e.Py0) / (e.lmax - e.lmin)); }
    double timeAt(int x) const { return (x - e.Px0) * m + o; }
    double levelAt(int y) const {
        return std::clamp(e.lmin + (e.lmax - e.lmin) * ((e.Py1 - 1) - y) / double((e.Py1 - 1) - e.Py0), e.lmin, e.lmax);
    }
    bool vis(int x, int y) const { return e.Px0 <= x && x < e.Px1 && e.Py0 <= y && y < e.Py1; }
};

// The visible columns of the segment from p1 to p2 (c0 .. c0 + f.size() - 1 from p1.x) and the
// fraction of the way from p1.y to p2.y at each; false when nothing of it shows.
bool segmentShape(const EnvMap& mp, int x1, int y1, int x2, int y2, double slope, int& c0, std::vector<double>& f) {
    const EnvGeo& e = mp.e;
    if (!mp.vis(x1, y1) && !mp.vis(x2, y2) && !(x1 < e.Px0 && x2 >= e.Px1)) return false;
    int dx = x2 - x1;
    c0 = mp.vis(x1, y1) ? 0 : e.Px0 - x1;
    int c1 = mp.vis(x2, y2) ? dx : e.Px1 - 2 - x1, n = c1 - c0 + 1;
    if (n < 1) return false;
    f.assign((size_t)n, 0.0);
    double B = 0;
    bool linear = slope >= 0.49999 && slope < 0.50001;
    if (!linear) B = std::pow(1 / envSlopeCurve(slope) - 1, 2);
    for (int j = 0; j < n; ++j) {
        if (dx == 0) f[(size_t)j] = 1;
        else if (linear) f[(size_t)j] = double(c0 + j) / dx;
        else f[(size_t)j] = (std::pow(B, double(c0 + j) / dx) - 1) / (B - 1);
    }
    return true;
}
}

// Steps 4 and 5 of drawing the graph: the curve buffer (5 subsamples a column, 0..1 of the
// plot height), from each point to the next with the next point's slope, then 0 after the last point.
static std::vector<double> curveBuffer(const EnvMap& mp, const std::vector<int>& xs, const std::vector<int>& ys, const std::vector<double>& slopes) {
    const EnvGeo& e = mp.e;
    int W = e.Gx1 - e.Gx0, Pw = e.Px1 - e.Px0;
    std::vector<double> buf((size_t)std::max(5 * std::max(W, Pw), 0), 0.0);
    double Hp = e.Py1 - e.Py0;
    std::vector<double> f;
    for (size_t i = 0; i + 1 < xs.size(); ++i) {
        int x1 = xs[i], y1 = ys[i], x2 = xs[i + 1], y2 = ys[i + 1], c0;
        if (!segmentShape(mp, x1, y1, x2, y2, slopes[i], c0, f)) continue;
        double top = (y1 - e.Py0) / Hp;
        int k = x1 + c0 - e.Px0;
        for (size_t j = 0; j < f.size(); ++j) {
            double prev = j > 0 ? f[j - 1] : f[0];
            for (int sub = 0; sub < 5; ++sub) {
                double v = prev + (f[j] - prev) * sub / 5;
                long long idx = 5LL * (k + (long long)j) + sub;
                if (idx >= 0 && idx < (long long)buf.size()) buf[(size_t)idx] = (1 - (v * (y2 - y1) / Hp + top)) * 0.98;
            }
        }
    }
    if (!xs.empty())
        for (long long i = 5LL * std::max(0, xs.back() - e.Px0); i < 5LL * Pw && i < (long long)buf.size(); ++i) buf[(size_t)i] = 0;
    return buf;
}

// The graph's colours, fields of the widget: grid (tempo grid lines), area
// (the fill under the curve), curve, handle (an unselected point's inside) and dots (the loop's dotted
// segment). A skin with a dark display sets them to suit it.
struct EnvColours { uint32_t grid, area, curve, handle, dots; };
static EnvColours envColours(const Widget& w) {
    return {colourField(w, "grid", 0xffc3c3c3), colourField(w, "area", 0xa5fbffff), colourField(w, "curve", 0xff888888),
            colourField(w, "handle", 0xfffdfdfd), colourField(w, "dots", 0xff0c0c0c)};
}

// Step 7: the fill from the curve down, then the anti-aliased curve (5 x 5 supersampling, a 7 x 7
// subpixel brush, wider at a higher density), each column landing one pixel right of its own place,
// over the graph rect G.
static void paintGraph(Canvas& c, const EnvGeo& e, const std::vector<double>& buf, const EnvColours& k) {
    int W = e.Gx1 - e.Gx0, H = e.Gy1 - e.Gy0, R = e.D(3);
    for (int col = 0; col < W && 5 * col < (int)buf.size(); ++col) {
        int hh = (int)(buf[(size_t)(5 * col)] * H + 0.5);
        vline(c, e.Gx0 + col, e.Gy1 - hh, hh, k.area);
    }
    if (W <= 0 || H <= 0) return;
    int SW = 5 * W + 2 * R, SH = 5 * H;
    std::vector<uint8_t> cover((size_t)SW * SH, 0);
    auto at = [&](size_t i) { return i < buf.size() ? buf[i] : 0.0; };
    int prev = (int)(at(0) * H * 5 + 0.5);
    for (int col = 0; col < W; ++col)
        for (int sub = 0; sub < 5; ++sub) {
            int y = (int)(at((size_t)(5 * col + sub)) * H * 5 + 0.5);
            int lo = y == prev ? y : std::min(y, prev), hi = y == prev ? y + 1 : std::max(y, prev);
            for (int yy = lo; yy < hi; ++yy)
                for (int ddx = -R; ddx <= R; ++ddx)
                    for (int ddy = -R; ddy <= R; ++ddy) {
                        int sx = 5 * col + sub + ddx + R, sy = std::clamp(yy + ddy, 0, SH - 1);
                        if (sx >= 0 && sx < SW) cover[(size_t)sy * SW + sx] = 1;
                    }
            prev = y;
        }
    for (int col = -1; col <= W - 2; ++col)
        for (int row = 0; row < H; ++row) {
            int n = 0;
            for (int sy = 5 * row; sy < 5 * row + 5; ++sy)
                for (int sx = 5 * col + R; sx < 5 * col + R + 5; ++sx)
                    if (sx >= 0 && sx < SW) n += cover[(size_t)sy * SW + sx];
            int a = (int)(n / 25.0 * 255 + 0.5);
            if (a) pixel(c, e.Gx0 + col + 1, e.Gy1 - row, (uint32_t)(a * (k.curve >> 24) / 255) << 24 | (k.curve & 0xffffff));
        }
}

// Step 8: a point handle (a filled square when selected, else white with a red outline), drawn
// only when its centre is in the plot; a slope handle at mid-time on the curve (a 5 x 5 ring with
// open corners, filled in the middle when its point is selected).
static void pointHandle(Canvas& c, const EnvGeo& e, int x, int y, bool selected, uint32_t inside) {
    if (!(x >= e.Px0 && x < e.Px1 && y >= e.Py0 && y < e.Py1)) return;
    int s = e.D(7), a = s / 2;
    fillRect(c, {x - a + e.hx, y - a + e.hy, s, s}, selected ? 0xffc80800 : inside);
    if (!selected) frame(c, x - a + e.hx, y - a + e.hy, s, s, 0xffc80800, e.k);
}

static void slopeHandle(Canvas& c, const EnvGeo& e, int xp, int yp, int x, int y, double slope, bool selected) {
    int cx = (xp + x) / 2;
    if (cx < e.Px0 || cx >= e.Px1) return;
    int cy = (int)((y - yp) * envSlopeCurve(slope) + 0.5) + yp;
    if (cy < e.Py0 || cy >= e.Py1) return;
    const uint32_t col = 0xffc80800;
    auto H = [&](int dx, int dy, int n) { fillRect(c, {cx + e.D(dx), cy + e.D(dy), e.D(dx + n) - e.D(dx), e.k}, col); };   // design offsets
    auto V = [&](int dx, int dy, int n) { fillRect(c, {cx + e.D(dx), cy + e.D(dy), e.k, e.D(dy + n) - e.D(dy)}, col); };
    H(-1, -2, 3);
    V(-2, -1, 3);
    H(-1, 2, 3);
    V(2, -1, 3);
    if (selected) fillRect(c, {cx + e.D(-1), cy + e.D(-1), e.D(2) - e.D(-1), e.D(2) - e.D(-1)}, col);
}

static std::string fixed(double v, int decimals) {
    char fmt[16];
    std::snprintf(fmt, sizeof fmt, "%%.%df", std::clamp(decimals, 0, 9));
    return formatNumber(fmt, v);
}

// Time labels in the centre band: seconds, or beats with tempo sync; each "  value  " drawn from
// (x - 1, band top) in the label font, whose baked field hides the grid behind it.
static void envLabels(Gui& g, Canvas& c, const Hit& h, const EnvGeo& e, double m, double o, bool sync) {
    const Widget& w = g.wid(h);
    if (w.text.font < 0) return;
    const Font& f = g.skin().fonts[w.text.font];
    Rect band{e.Px0 + 1, e.Sy0, e.Px1 - e.Px0 - 1, e.Sy1 - e.Sy0};   // measured: the first plot column keeps the grid
    Rect saved = c.clip;
    c.clip = c.clip & band;
    int Pw = e.Px1 - e.Px0, Pwd = (int)(Pw / e.d);   // the plot width in device and in design pixels
    double md = m / e.d;                              // m is per design pixel
    auto label = [&](int x, const std::string& s) { drawText(c, f, s, {x - e.D(1), e.Sy0, e.Px1 + e.D(200) - x, f.height}, 0, 0, false); };
    if (!sync) {
        double start = o / 1000, width = Pw * md / 1000, end = start + width;
        int ex = (int)std::floor(std::log10(width));
        int prec = std::max(0, ex - 1);
        double pe = std::pow(10.0, ex);
        int labelw = std::max(f.width(fixed(std::ceil(start / pe) * pe, prec)), f.width(fixed(std::floor(end / pe) * pe, prec)));
        int n = std::clamp(Pw / (labelw + e.D(8)), 1, 10);
        double v = width / n;
        int k = (int)std::floor(std::log10(v));
        double d = v / std::pow(10.0, k);
        int u = d <= 1 ? 1 : d <= 2 ? 2 : d <= 5 ? 5 : 10;
        double major = u * std::pow(10.0, k);
        int decimals = k < 0 ? -k : 0;
        double first = std::ceil(start / major) * major;
        int majors = (int)(width / major);
        auto pick = [](int p, int u) {
            if (p >= 50) return (u == 1 || u % 5 == 0) ? 5 : (u == 1 || u % 2 == 0) ? 2 : 1;
            if (p >= 20) return (u == 1 || u % 2 == 0) ? 2 : 1;
            return 1;
        };
        int M = majors == 0 ? 1 : pick(Pwd / majors, u);
        double minor = major / M;
        int back = (int)((first - start) / minor);
        double t0 = first - back * minor;
        int phase = back > 0 ? M - back : 0, count = (int)((end - t0) / minor);
        for (int i = 0; i <= count; ++i)
            if ((phase + i) % M == 0) {
                double t = t0 + i * minor;
                label((int)(e.Px0 + (t * 1000 - o) / md), "  " + fixed(t, decimals) + "  ");
            }
    } else {   // beats (not checked against a capture)
        int sub = tempoSub(m);
        double gstep = tempoGrid(m);
        int idx = (int)std::ceil(o / gstep), step2 = 1;
        while ((Pw * md / gstep) / step2 > 7) step2 *= 2;
        char buf[64];
        std::snprintf(buf, sizeof buf, "  %i/%i  ", idx + sub, sub);
        int spacing = (int)(f.width(buf) * 1.6 + e.D(3)), next = e.Px0;
        for (;; ++idx) {
            int x = (int)(e.Px0 + (idx * gstep - o) / md);
            if (x >= e.Px1) break;
            if (x < next) continue;
            bool show = true;
            if (sub < 2) {
                show = idx % step2 == 0;
                std::snprintf(buf, sizeof buf, "  %i  ", idx / std::max(sub, 1));
            } else if (sub % step2 == 0) {
                show = idx % step2 == 0;
                if (sub / step2 < 2) std::snprintf(buf, sizeof buf, "  %i  ", idx / step2);
                else std::snprintf(buf, sizeof buf, "  %i/%i ", idx / step2, sub / step2);
            } else {
                std::snprintf(buf, sizeof buf, "  %i/%i  ", idx, sub);
            }
            if (!show) continue;
            label(x, buf);
            next = x + spacing;
        }
    }
    c.clip = saved;
}

// In this order: grid lines, time labels, then clipped to the plot: the curve buffer
// (5 subsamples a column), loop markers with the dotted loop-back, the fill and the anti-aliased
// curve (5 x 5 supersampling, a 7 x 7 subpixel brush), and the handles.
static void envDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    EnvState& st = envState(g, h);
    EnvGeo e = envGeo(g, h, r);
    double m, o;
    viewOf(g, e, st, m, o);
    EnvMap mp{e, m / e.d, o};
    const EnvColours k = envColours(g.wid(h));
    const Env& env = st.env;
    int count = (int)env.count(), last = count - 1;
    bool sync = g.inst(h).param >= 0 && g.plain(*h.node, h.i) > 0.5;
    Rect saved = c.clip;
    if (!e.isStatic) {
        double gstep = tempoGrid(m);   // the grid is spaced in design pixels
        for (double t = std::ceil(o / gstep);; ++t) {
            int x = (int)(e.Px0 + (t * gstep - o) / mp.m);
            if (x >= e.Px1) break;
            vline(c, x, e.Py0 + 1, (e.Py1 - e.Py0) - 2, k.grid);
        }
        if (e.centre) envLabels(g, c, h, e, m, o, sync);
    }
    c.clip = c.clip & Rect{e.Px0, e.Py0 - (e.isStatic ? e.D(3) : 0), e.Px1 - e.Px0, e.Py1 - e.Py0 + (e.isStatic ? e.D(3) : 0)};
    // points in pixels: the implicit start point, then the breakpoints
    std::vector<int> xs((size_t)count + 1), ys((size_t)count + 1);
    xs[0] = mp.px(0);
    ys[0] = mp.py(env.level[(size_t)last]);
    for (int i = 0; i < count; ++i) {
        xs[(size_t)i + 1] = mp.px(env.cum(i));
        ys[(size_t)i + 1] = mp.py(env.level[(size_t)i]);
    }
    std::vector<double> buf = curveBuffer(mp, xs, ys, env.slope);
    std::vector<double> f;
    if (!e.isStatic && env.loopStart >= 0 && env.loopStart < env.loopEnd && env.loopEnd < count) {   // loop markers
        int lxs = xs[(size_t)env.loopStart + 1], lxe = xs[(size_t)env.loopEnd + 1], ye = ys[(size_t)env.loopEnd + 1];
        const uint32_t red = 0xfff51717;
        if (lxs >= e.Px0 && lxs < e.Px1) fillRect(c, {lxs + e.hx, e.Py0, e.k, e.Py1 - e.Py0}, red);
        if (lxe >= e.Px0 && lxe < e.Px1) fillRect(c, {lxe + e.hx, e.Py0, e.k, e.Py1 - e.Py0}, red);
        if (std::min(lxe, e.Px1) > std::max(lxs, e.Px0)) fillRect(c, {std::max(lxs, e.Px0), ye, std::min(lxe, e.Px1) - std::max(lxs, e.Px0), e.k}, red);
        int c0;
        int x2 = xs[(size_t)env.loopStart + 2], y2 = ys[(size_t)env.loopStart + 2];
        if (env.loopEnd - env.loopStart > 1 && segmentShape(mp, lxs, ye, x2, y2, env.slope[(size_t)env.loopStart + 1], c0, f)) {
            int dy = y2 - ye, y = c0 == 0 ? ye : (int)(dy * f[0] + 0.5) + ye, x = lxs + c0;
            bool on = true;
            for (size_t j = 0; j < f.size(); ++j) {
                int target = (int)(f[j] * dy + 0.5) + ye;
                do {
                    if (on) pixel(c, x, y, k.dots);
                    y += (target > y) - (target < y);
                    on = !on;
                } while (y != target);
                ++x;
            }
        }
    }
    paintGraph(c, e, buf, k);
    if (!e.isStatic) {   // handles: the start point, then each point and its slope handle
        pointHandle(c, e, xs[0], ys[0], st.sel == last, k.handle);
        for (int i = 0; i < count; ++i) {
            pointHandle(c, e, xs[(size_t)i + 1], ys[(size_t)i + 1], st.sel == i, k.handle);
            slopeHandle(c, e, xs[(size_t)i], ys[(size_t)i], xs[(size_t)i + 1], ys[(size_t)i + 1], env.slope[(size_t)i], st.sel == i);
        }
    }
    c.clip = saved;
}

// Readout edits (envui.*) apply to the envelope; outside changes to its data or view redraw it.
static void envTick(Gui& g, const Hit& h, Rect r) {
    EnvState& st = kindState<EnvState>(g, h);
    std::string before = st.seen;
    envState(g, h);
    EnvGeo e = envGeo(g, h, r);
    if (g.loaded && !e.shared) {   // a sound loaded while an own-view row shows: it fits itself
        st.m = std::clamp(fitZoom(g, st.env, e.Px1 - e.Px0, false) * e.d, 0.1, 999999.0);
        st.o = 0;
        g.invalidate(r);
    }
    if (st.seen != before) {
        envCommit(g, h, st, false);
        g.invalidate(r);
    }
    if (e.isStatic) return;
    State& s = g.state();
    double v[6];
    bool edited = false;
    for (int k = 0; k < 6; ++k) {
        int p = s.indexOf(kEnvUi[k]);
        v[k] = p >= 0 ? s.get((size_t)p) : st.shown[k];
        if (p >= 0 && st.shown[k] == -1e300) { edited = true; continue; }   // first tick: publish
        if (p >= 0 && v[k] != st.shown[k] && k != 1 && k != 2) edited = true;
    }
    if (!edited) return;
    Env& env = st.env;
    int count = (int)env.count();
    bool first = st.shown[0] == -1e300;
    if (!first && v[0] != st.shown[0]) st.sel = std::clamp((int)std::lround(std::min(v[0], (double)count)) - 1, -1, count - 1);
    else if (!first && st.sel >= 0) {
        size_t i = (size_t)st.sel;
        if (v[3] != st.shown[3]) envSetTime(g, st, st.sel, std::max(1.0, v[3] * 1000));
        if (v[4] != st.shown[4]) env.level[i] = std::clamp(v[4], e.lmin, e.lmax);
        if (v[5] != st.shown[5]) env.slope[i] = std::clamp(v[5], 0.05, 0.999);
    }
    envCommit(g, h, st, !first);
    g.invalidate(r);
}

struct EnvHitResult {
    enum { None, Point, Start, Marker, Slope } what = None;
    int i = -1;
};

// Strict distances, no handle offset: the start point first, then the points, the loop markers
// (only the loop's two points), then the slope handles.
static EnvHitResult envHit(const EnvMap& mp, const Env& env, int mx, int my) {
    EnvHitResult r;
    const EnvGeo& e = mp.e;
    int count = (int)env.count(), last = count - 1, d3 = e.D(3), d4 = e.D(4), d5 = e.D(5);
    if (std::abs(mx - mp.px(0)) < d4 && std::abs(my - mp.py(env.level[(size_t)last])) < d5) return {EnvHitResult::Start, last};
    for (int i = 0; i < count; ++i)
        if (std::abs(mx - mp.px(env.cum(i))) < d5 && std::abs(my - mp.py(env.level[(size_t)i])) < d5) return {EnvHitResult::Point, i};
    for (int i : {env.loopStart, env.loopEnd})
        if (i >= 0 && i < count && std::abs(mx - mp.px(env.cum(i))) < d3) return {EnvHitResult::Marker, i};
    for (int i = 0; i < count; ++i) {
        int x = mp.px(env.cum(i)), y = mp.py(env.level[(size_t)i]);
        int xp = i == 0 ? mp.px(0) : mp.px(env.cum(i - 1)), yp = i == 0 ? mp.py(env.level[(size_t)last]) : mp.py(env.level[(size_t)i - 1]);
        int cx = (xp + x) / 2, cy = (int)((y - yp) * envSlopeCurve(env.slope[(size_t)i]) + 0.5) + yp;
        if (std::abs(mx - cx) < d5 && std::abs(my - cy) < d5) return {EnvHitResult::Slope, i};
    }
    return r;
}

static bool envDown(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    EnvGeo e = envGeo(g, h, r);
    if (e.isStatic) return false;
    EnvState& st = envState(g, h);
    double m, o;
    viewOf(g, e, st, m, o);
    EnvMap mp{e, m / e.d, o};
    EnvHitResult hit = envHit(mp, st.env, x, y);
    st.lastX = x;
    st.lastY = y;
    st.dragPoint = hit.i;
    switch (hit.what) {
    case EnvHitResult::Point: st.sel = hit.i; st.drag = DragPoint; break;
    case EnvHitResult::Start: st.sel = hit.i; st.drag = DragLevel; break;
    case EnvHitResult::Marker: st.drag = DragMarker; break;
    case EnvHitResult::Slope: st.sel = hit.i; st.drag = DragSlope; break;
    default: st.anchor = mp.timeAt(x); st.drag = DragView; break;
    }
    envCommit(g, h, st, false);
    g.invalidate(r);
    return true;
}

static void envDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    EnvGeo e = envGeo(g, h, r);
    EnvState& st = envState(g, h);
    double m, o;
    viewOf(g, e, st, m, o);
    EnvMap mp{e, m / e.d, o};
    Env& env = st.env;
    int dx = st.lastX - x, dy = st.lastY - y, i = st.dragPoint, count = (int)env.count();
    st.lastX = x;
    st.lastY = y;
    bool sync = g.inst(h).param >= 0 && g.plain(*h.node, h.i) > 0.5;
    switch (st.drag) {
    case DragPoint:
    case DragLevel:
        env.level[(size_t)i] = mp.levelAt(y);
        if (st.drag == DragPoint) {
            double t = mp.timeAt(x);
            if (sync) {
                double gstep = tempoGrid(m);
                t = (int)((t + 0.5 * gstep) / gstep) * gstep;
            }
            envSetTime(g, st, i, std::max(1.0, t - env.cum(i - 1)));
        }
        break;
    case DragSlope: {
        int prev = i == 0 ? count - 1 : i - 1;
        env.slope[(size_t)i] += (env.level[(size_t)prev] < env.level[(size_t)i] ? 0.01 : -0.01) * dy / e.d;
        env.slope[(size_t)i] = std::clamp(env.slope[(size_t)i], 0.05, 0.999);
        break;
    }
    case DragView: {   // zoom compounding per event (up zooms in), scroll keeping the anchor under the pointer
        int xOld = (int)(e.Px0 + (st.anchor - o) / (m / e.d));
        double m2 = std::clamp((100 - dy / e.d) * 0.01 * m, 0.1, 999999.0);
        if (m2 != m) m = m2;
        double md = m / e.d;
        int xNew = (int)(e.Px0 + (st.anchor - o) / md);
        o = std::max(0.0, dx * md + o - (xOld - xNew) * md);
        setView(g, e, st, m, o);
        g.invalidate(r);
        return;
    }
    case DragMarker: {   // not checked live
        int n = 0;
        for (int k = 1; k < count; ++k)
            if (std::abs(mp.px(env.cum(k)) - x) < std::abs(mp.px(env.cum(n)) - x)) n = k;
        int lo = env.loopStart, hi = env.loopEnd, lastN = st.lastMarker;
        if (lo < lastN) {
            if (hi <= lastN && hi != n && lo < n) hi = n;
        } else if (lo != n && n < hi) {
            lo = n;
        }
        env.loopStart = lo;
        env.loopEnd = hi;
        normaliseLoop(env);
        st.lastMarker = n;
        break;
    }
    default:
        return;
    }
    envCommit(g, h, st, true);
    g.invalidate(r);
}

static void envUp(Gui& g, const Hit& h, Rect, int, int) { kindState<EnvState>(g, h).drag = DragNone; }

static void envFit(Gui& g, const Hit& h, Rect r, int, int) {   // double-click: fit the view
    EnvGeo e = envGeo(g, h, r);
    if (e.isStatic) return;
    EnvState& st = envState(g, h);
    setView(g, e, st, fitZoom(g, st.env, e.Px1 - e.Px0, false) * e.d, 0);
    g.invalidate(r);
}

// Right press inside the plot: on a point, delete it (down to 3; the later points move earlier by
// its time); elsewhere, add one there (up to 31).
static void envRight(Gui& g, const Hit& h, Rect r, int x, int y, bool, bool up) {
    EnvGeo e = envGeo(g, h, r);
    if (up || e.isStatic || !(x >= e.Px0 && x < e.Px1 && y >= e.Py0 && y < e.Py1)) return;
    EnvState& st = envState(g, h);
    double m, o;
    viewOf(g, e, st, m, o);
    EnvMap mp{e, m / e.d, o};
    Env& env = st.env;
    int count = (int)env.count();
    EnvHitResult hit = envHit(mp, env, x, y);
    if (hit.what == EnvHitResult::Point || hit.what == EnvHitResult::Start) {
        if (count <= 3) return;
        int i = hit.i;
        env.dt.erase(env.dt.begin() + i);
        env.level.erase(env.level.begin() + i);
        env.slope.erase(env.slope.begin() + i);
        if (env.loopStart > i) --env.loopStart;
        if (env.loopEnd > i) --env.loopEnd;
        normaliseLoop(env);
        st.sel = (int)env.count() - 1;
        for (int k = 0; k < (int)env.count(); ++k)
            if (mp.px(env.cum(k)) > x) { st.sel = k; break; }
    } else {
        if (count >= 31) return;
        double t = mp.timeAt(x), level = mp.levelAt(y);
        if (g.inst(h).param >= 0 && g.plain(*h.node, h.i) > 0.5) {
            double gstep = tempoGrid(m);
            t = (int)((t + 0.5 * gstep) / gstep) * gstep;
        }
        int i = 0;
        while (i < count && env.cum(i) < t) ++i;
        if (i < count) {
            double before = env.cum(i - 1), at = env.cum(i);
            env.dt.insert(env.dt.begin() + i, t - before);
            env.dt[(size_t)i + 1] = at - t;
            env.slope.insert(env.slope.begin() + i, i == 0 ? 0.5 : env.slope[(size_t)i]);
            env.level.insert(env.level.begin() + i, level);
        } else {
            env.dt.push_back(t - env.total());
            env.level.push_back(level);
            env.slope.push_back(0.5);
        }
        if (env.loopStart >= i) ++env.loopStart;
        if (env.loopEnd >= i) ++env.loopEnd;
        normaliseLoop(env);
        st.sel = i;
    }
    envCommit(g, h, st, true);
    g.invalidate(r);
}

// ---- keyscale: a key scaling curve (the envelope editor configured differently) --------------------

// Points (note 0..127, level in dB over "levelRange", slope) kept as text data under the widget's
// key: "<note> <level> <slope>|...", the curve being every point up to the first with note 127;
// points stored after it (a stale tail, kept after a ';') are ignored but kept,
// since the delete quirk re-reads one of them. Fields: levelRange, mode
// ("selected" or "strip": no input, no handles, clip P with y0 - 3), scaleRect (notes per pixel:
// 127 over its width - 11), readouts {note, level, slope}. The plot is R inset (6, 3, 4, 4), the
// graph is R, so the curve and fill land 5 px left of the handles; no start point, no loop, no
// grid or labels; edits run in FIX mode; the first and the last point move only up and down.
struct KsState : KindState {
    std::vector<std::array<double, 3>> stored;
    std::string key, seen;
    double m = 0;                 // notes per pixel after a double-click refit, 0 = from scaleRect
    EnvDrag drag = DragNone;
    int point = -1, lastX = 0, lastY = 0;
};

static int ksCount(const KsState& st) {   // the curve: up to the first point at note 127 or more
    for (size_t i = 0; i < st.stored.size(); ++i)
        if (st.stored[i][0] >= 127) return (int)i + 1;
    return (int)st.stored.size();
}

static bool ksIsZ(const std::string& key) { return !key.empty() && key.back() == 'z'; }

static KsState& ksState(Gui& g, const Hit& h) {
    KsState& st = kindState<KsState>(g, h);
    const std::string& key = g.inst(h).key;
    std::string data = g.state().hasData(key) ? g.state().data(key) : std::string("\x01");
    if (key == st.key && data == st.seen) return st;
    st.key = key;
    st.seen = data;
    st.stored.clear();
    const char* p = data.c_str();
    const char* end = p + data.size();
    while (p < end) {
        double v[3];
        bool ok = true;
        for (double& x : v) {
            while (p < end && (*p == ' ' || *p == '|' || *p == ';')) ++p;
            ok = ok && parseNumber(p, end, x);
        }
        if (!ok || st.stored.size() == 32) break;
        st.stored.push_back({std::min(v[0], 127.0), v[1], v[2]});
    }
    if (st.stored.size() == 32) st.stored[31][0] = 127;   // the last of 32 slots always ends the curve
    if (st.stored.size() < 2) {
        if (ksIsZ(key)) st.stored = {{0, -60, 0.5}, {127, 67, 0.5}};
        else st.stored = {{0, 0, 0.5}, {60, 0, 0.5}, {127, 0, 0.5}};
    }
    return st;
}

static EnvGeo ksGeo(Gui& g, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    EnvGeo e{};
    e.d = g.skin().density;
    e.k = g.skin().lw();
    e.Rx0 = r.x; e.Ry0 = r.y; e.Rx1 = r.x + r.w; e.Ry1 = r.y + r.h;
    e.Px0 = e.Rx0 + e.D(6); e.Py0 = e.Ry0 + e.D(3); e.Px1 = e.Rx1 - e.D(4); e.Py1 = e.Ry1 - e.D(4);
    e.Gx0 = e.Rx0; e.Gy0 = e.Ry0; e.Gx1 = e.Rx1; e.Gy1 = e.Ry1;
    e.isStatic = w.json["mode"].str() == "strip";
    e.lmin = w.json["levelRange"][0].num(-80);
    e.lmax = w.json["levelRange"][1].num(0);
    return e;
}

static double ksZoom(Gui& g, const Hit& h, const KsState& st) {
    if (st.m > 0) return st.m;
    int w = g.wid(h).json["scaleRect"][2].integer(g.skin().dp(475));
    return 127.0 / std::max((w - g.skin().dp(10)) - 1, 1);
}

// The curve in the envelope's terms: a time (note distance) from the previous point per point.
static Env ksEnv(const KsState& st) {
    Env e;
    double prev = 0;
    for (int i = 0; i < ksCount(st); ++i) {
        e.dt.push_back(st.stored[(size_t)i][0] - prev);
        e.level.push_back(st.stored[(size_t)i][1]);
        e.slope.push_back(st.stored[(size_t)i][2]);
        prev = st.stored[(size_t)i][0];
    }
    return e;
}

// Written back with note_i = min(127, round(dt_i) + note_(i-1)), stopping after the
// first note above 126; stored points beyond what is written stay (the stale tail).
static void ksWrite(Gui& g, KsState& st, const Env& e) {
    double note = 0;
    for (size_t i = 0; i < e.count(); ++i) {
        note = std::min(127.0, std::round(e.dt[i]) + (i ? note : 0.0));
        std::array<double, 3> pt = {note, e.level[i], e.slope[i]};
        if (i < st.stored.size()) st.stored[i] = pt;
        else st.stored.push_back(pt);
        if (note > 126) break;
    }
    int n = ksCount(st);
    std::string out;
    for (size_t i = 0; i < st.stored.size(); ++i) {
        if (i) out += (int)i == n ? ";" : "|";
        out += formatNumber("%.7g", st.stored[i][0]) + " " + formatNumber("%.7g", st.stored[i][1]) + " " + formatNumber("%.7g", st.stored[i][2]);
    }
    st.seen = out;
    g.state().setData(st.key, out);
}

// Readouts: the selected point's note, level and slope (0 for the first point, which has none). The
// readout params are shared by every keyscale widget, so what was last written to each is kept in
// kindData ("<id>\x01v": the value asked for, "<id>\x01s": the value the param then held) and an
// edit is a param no longer holding what was written.
static const char* const ksNames[3] = {"note", "level", "slope"};

static void ksWanted(Gui& g, const KsState& st, double v[3]) {
    int sel = std::clamp((int)g.kindData[st.key], 0, ksCount(st) - 1);
    v[0] = st.stored[(size_t)sel][0];
    v[1] = st.stored[(size_t)sel][1];
    v[2] = sel > 0 ? st.stored[(size_t)sel][2] : 0;
}

static void ksReadouts(Gui& g, const Hit& h, KsState& st) {
    const Json& ro = g.wid(h).json["readouts"];
    double v[3];
    ksWanted(g, st, v);
    for (int k = 0; k < 3; ++k) {
        std::string id = ro[ksNames[k]].str();
        int p = g.state().indexOf(id);
        if (p < 0) continue;
        g.state().set((size_t)p, v[k]);
        g.kindData[id + "\x01v"] = v[k];
        g.kindData[id + "\x01s"] = g.state().get((size_t)p);
    }
}

static void ksDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    KsState& st = ksState(g, h);
    EnvGeo e = ksGeo(g, h, r);
    EnvMap mp{e, ksZoom(g, h, st), 0};
    int n = ksCount(st), sel = (int)g.kindData[st.key];
    std::vector<int> xs((size_t)n), ys((size_t)n);
    std::vector<double> slopes;
    for (int i = 0; i < n; ++i) {
        xs[(size_t)i] = mp.px(st.stored[(size_t)i][0]);
        ys[(size_t)i] = mp.py(st.stored[(size_t)i][1]);
        if (i) slopes.push_back(st.stored[(size_t)i][2]);
    }
    Rect saved = c.clip;
    c.clip = c.clip & Rect{e.Px0, e.Py0 - (e.isStatic ? e.D(3) : 0), e.Px1 - e.Px0, e.Py1 - e.Py0 + (e.isStatic ? e.D(3) : 0)};
    const EnvColours k = envColours(g.wid(h));
    paintGraph(c, e, curveBuffer(mp, xs, ys, slopes), k);
    if (!e.isStatic)
        for (int i = 0; i < n; ++i) {
            pointHandle(c, e, xs[(size_t)i], ys[(size_t)i], sel == i, k.handle);
            if (i) slopeHandle(c, e, xs[(size_t)i - 1], ys[(size_t)i - 1], xs[(size_t)i], ys[(size_t)i], st.stored[(size_t)i][2], sel == i);
        }
    c.clip = saved;
}

// Level and slope readout edits write this curve's selected point; the readouts follow the curve.
static void ksTick(Gui& g, const Hit& h, Rect r) {
    KsState& st = kindState<KsState>(g, h);
    std::string before = st.seen;
    ksState(g, h);
    if (st.seen != before) g.invalidate(r);
    if (g.wid(h).json["mode"].str() == "strip") return;
    const Json& ro = g.wid(h).json["readouts"];
    State& s = g.state();
    auto known = [&](const std::string& key, double& v) {
        auto it = g.kindData.find(key);
        if (it == g.kindData.end()) return false;
        v = it->second;
        return true;
    };
    double want[3], edited[3];
    bool refresh = false, edit = false;
    ksWanted(g, st, want);
    for (int k = 0; k < 3; ++k) {
        std::string id = ro[ksNames[k]].str();
        int p = s.indexOf(id);
        double held = 0, asked = 0;
        edited[k] = -1e300;
        if (p < 0) continue;
        if (!known(id + "\x01s", held) || !known(id + "\x01v", asked)) { refresh = true; continue; }
        if (k > 0 && s.get((size_t)p) != held) { edited[k] = s.get((size_t)p); edit = true; }
        else if (asked != want[k]) refresh = true;
    }
    if (edit) {
        int i = std::clamp((int)g.kindData[st.key], 0, ksCount(st) - 1);
        EnvGeo e = ksGeo(g, h, r);
        Env env = ksEnv(st);
        if (edited[1] != -1e300) env.level[(size_t)i] = std::clamp(edited[1], e.lmin, e.lmax);
        if (edited[2] != -1e300 && i > 0) env.slope[(size_t)i] = std::clamp(edited[2], 0.01, 0.99);
        ksWrite(g, st, env);
        g.invalidate(r);
    }
    if (edit || refresh) ksReadouts(g, h, st);
}

struct KsHit {
    enum { None, Point, Slope } what = None;
    int i = -1;
};

static KsHit ksHit(const EnvMap& mp, const KsState& st, int mx, int my) {
    int n = ksCount(st), d5 = mp.e.D(5);
    for (int i = 0; i < n; ++i)
        if (std::abs(mx - mp.px(st.stored[(size_t)i][0])) < d5 && std::abs(my - mp.py(st.stored[(size_t)i][1])) < d5) return {KsHit::Point, i};
    for (int i = 1; i < n; ++i) {
        int x = mp.px(st.stored[(size_t)i][0]), y = mp.py(st.stored[(size_t)i][1]);
        int xp = mp.px(st.stored[(size_t)i - 1][0]), yp = mp.py(st.stored[(size_t)i - 1][1]);
        int cx = (xp + x) / 2, cy = (int)((y - yp) * envSlopeCurve(st.stored[(size_t)i][2]) + 0.5) + yp;
        if (std::abs(mx - cx) < d5 && std::abs(my - cy) < d5) return {KsHit::Slope, i};
    }
    return {};
}

static bool ksDown(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    EnvGeo e = ksGeo(g, h, r);
    if (e.isStatic) return false;
    KsState& st = ksState(g, h);
    EnvMap mp{e, ksZoom(g, h, st), 0};
    KsHit hit = ksHit(mp, st, x, y);
    if (hit.what == KsHit::None) return false;   // no view drag here
    int n = ksCount(st);
    g.kindData[st.key] = hit.i;
    st.point = hit.i;
    st.drag = hit.what == KsHit::Slope ? DragSlope : hit.i == 0 || hit.i == n - 1 ? DragLevel : DragPoint;
    st.lastX = x;
    st.lastY = y;
    ksReadouts(g, h, st);
    g.invalidate(r);
    return true;
}

static void ksDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    EnvGeo e = ksGeo(g, h, r);
    KsState& st = ksState(g, h);
    EnvMap mp{e, ksZoom(g, h, st), 0};
    Env env = ksEnv(st);
    int i = st.point, dy = st.lastY - y;
    st.lastX = x;
    st.lastY = y;
    if (i < 0 || i >= (int)env.count()) return;
    if (st.drag == DragSlope) {
        env.slope[(size_t)i] += (env.level[(size_t)i - 1] < env.level[(size_t)i] ? 0.01 : -0.01) * dy / e.d;
        env.slope[(size_t)i] = std::clamp(env.slope[(size_t)i], 0.01, 0.99);
    } else {
        env.level[(size_t)i] = mp.levelAt(y);   // kept unrounded
        if (st.drag == DragPoint) {   // FIX: the next point stays where it is, at least 1 note apart
            double dtNew = std::max(1.0, mp.timeAt(x) - env.cum(i - 1)), delta = env.dt[(size_t)i] - dtNew;
            double next = std::clamp(env.dt[(size_t)i + 1] + delta, 1.0, 999999.0);
            delta = next - env.dt[(size_t)i + 1];
            env.dt[(size_t)i + 1] = next;
            env.dt[(size_t)i] = std::clamp(env.dt[(size_t)i] - delta, 1.0, 999999.0);
        }
    }
    ksWrite(g, st, env);
    ksReadouts(g, h, st);
    g.invalidate(r);
}

static void ksUp(Gui& g, const Hit& h, Rect, int, int) { kindState<KsState>(g, h).drag = DragNone; }

// Double-click refits the view to the plot width (127 over P.w - 1), which moves every point a
// little right until the page is rebuilt, a quirk kept on purpose.
static void ksFit(Gui& g, const Hit& h, Rect r, int, int) {
    EnvGeo e = ksGeo(g, h, r);
    if (e.isStatic) return;
    ksState(g, h).m = 127.0 / std::max(e.Px1 - e.Px0 - 1, 1);
    g.invalidate(r);
}

// Right press: on a point that is neither the first nor the last, delete it (while more than 2;
// its note distance is dropped, so later points move left, and a stored point past the new end may
// come back); elsewhere in the plot add one (up to 31) with the slope of the segment it splits.
static void ksRight(Gui& g, const Hit& h, Rect r, int x, int y, bool, bool up) {
    EnvGeo e = ksGeo(g, h, r);
    if (up || e.isStatic || !(x >= e.Px0 && x < e.Px1 && y >= e.Py0 && y < e.Py1)) return;
    KsState& st = ksState(g, h);
    EnvMap mp{e, ksZoom(g, h, st), 0};
    Env env = ksEnv(st);
    int n = (int)env.count();
    KsHit hit = ksHit(mp, st, x, y);
    double& sel = g.kindData[st.key];
    if (hit.what == KsHit::Point) {
        int i = hit.i;
        if (i == 0 || i == n - 1 || n <= 2) return;
        env.dt.erase(env.dt.begin() + i);
        env.level.erase(env.level.begin() + i);
        env.slope.erase(env.slope.begin() + i);
        ksWrite(g, st, env);
        n = ksCount(st);
        sel = n - 1;
        for (int k = 0; k < n; ++k)
            if (mp.px(st.stored[(size_t)k][0]) > x) { sel = k; break; }
    } else {
        if (n >= 31) return;
        double t = mp.timeAt(x), level = mp.levelAt(y);
        int i = 0;
        while (i < n && env.cum(i) < t) ++i;
        if (i >= n) return;
        double before = env.cum(i - 1), at = env.cum(i);
        env.dt.insert(env.dt.begin() + i, t - before);
        env.dt[(size_t)i + 1] = at - t;
        env.slope.insert(env.slope.begin() + i, i == 0 ? 0.5 : env.slope[(size_t)i]);
        env.level.insert(env.level.begin() + i, level);
        ksWrite(g, st, env);
        sel = i;
    }
    ksReadouts(g, h, st);
    g.invalidate(r);
}

// The host loaded a state: the shared view fits the longest of the nine envelopes over the plot
// width of the skin's shared-view envelope widgets.
void kindsLoaded(Gui& g) {
    int zoom = g.state().indexOf("envui.zoom"), scroll = g.state().indexOf("envui.scroll");
    if (zoom < 0) return;
    for (auto& v : g.skin().views)
        for (auto& w : v.second.widgets) {
            if (w.customKind != "envelope" || w.json["view"].str("shared") != "shared" || w.json["static"].flag()) continue;
            int plotW = w.rect.w - (w.json["strip"].flag(true) ? 2 * g.skin().dp(3) : 0);
            g.setParam(zoom, std::clamp(fitZoom(g, defaultEnv(false), plotW, true) * g.skin().density, 0.1, 999999.0));
            g.setParam(scroll, 0);
            return;
        }
}

// ---- the "presets" action: template menus ----------------------------------------------------------

// The slots (a JSON array like the table's "presets") are text data "presets.<table>", seeded from
// data/<table>.json. A slot with "points" is an envelope for the text key `key`. Any other slot sets
// params: "params" maps param templates (with {vars}) to values, and the table's own "params" gives
// the values of templates a slot leaves out; matrix slots ("amounts" by
// "<source>.<destination>", "opOn", "inputOn", missing amounts 0) read as matrix.<s>.<d>.{timbre},
// op.<o>.on.{timbre} and master.input_on.{timbre}. Loading writes them in one gesture ("editAll"
// applies); saving stores the current values of every template the table knows under the typed name.
static std::vector<std::pair<std::string, double>> slotParams(Gui& g, const Json& slot, const Json& table) {
    std::vector<std::pair<std::string, double>> out;
    auto put = [&](const std::string& tpl, double v) {
        for (auto& o : out)
            if (o.first == tpl) { o.second = v; return; }
        out.push_back({tpl, v});
    };
    for (auto& m : table["params"].members) put(m.first, m.second.num());
    if (slot.has("amounts") || slot.has("opOn")) {   // matrix templates
        State& s = g.state();
        for (size_t p = 0; p < s.size(); ++p) {
            const std::string& id = s.def(p).id;
            if (id.compare(0, 7, "matrix.") == 0 && id.size() > 10 && id.compare(id.size() - 3, 3, ".bl") == 0)
                put(id.substr(0, id.size() - 2) + "{timbre}", 0);
        }
        for (auto& m : slot["amounts"].members) put("matrix." + m.first + ".{timbre}", m.second.num());
        for (auto& m : slot["opOn"].members) put("op." + m.first + ".on.{timbre}", m.second.flag() ? 1 : 0);
        if (slot.has("inputOn")) put("master.input_on.{timbre}", slot["inputOn"].flag() ? 1 : 0);
    }
    for (auto& m : slot["params"].members) put(m.first, m.second.num());
    return out;
}

void presetMenu(Gui& g, const Hit& h, const Action& a) {
    State& s = g.state();
    const Json& table = g.skin().table(a.target);
    std::string slotsKey = "presets." + a.target;
    Json slots;
    if (!s.hasData(slotsKey) || !parseJson(s.data(slotsKey), slots) || slots.type != Json::Array) slots = table["presets"];
    if (slots.type != Json::Array || !slots.size()) return;
    std::vector<MenuEntry> menu;
    for (size_t k = 0; k < slots.size(); ++k) {
        MenuEntry e;
        e.label = slots[k]["name"].str();
        e.id = (int)k;
        e.columnBreak = a.perColumn > 0 && k > 0 && k % (size_t)a.perColumn == 0;
        menu.push_back(e);
    }
    std::string key = g.subst(*h.node, a.key), nameKey = g.subst(*h.node, a.nameKey);
    g.openMenu(menu, g.menuAnchor(h), "native", [&g, h, a, slots, key, nameKey, slotsKey](int k) mutable {
        if (k < 0 || k >= (int)slots.size() || !g.live(h)) return;
        State& s = g.state();
        const Json& table = g.skin().table(a.target);
        bool envelope = false;
        for (auto& slot : slots.items) envelope = envelope || slot.has("points");
        if (a.save) {   // the current envelope, or the current values of every template, into slot k
            Json fresh;
            fresh.type = Json::Object;
            Json name;
            name.type = Json::String;
            name.s = s.data(nameKey);
            fresh.members.push_back({"name", name});
            if (envelope) {
                Env env;
                if (!s.hasData(key) || !parseEnv(s.data(key), env)) env = defaultEnv(key.size() >= 5 && key.compare(key.size() - 5, 5, "pitch") == 0);
                Json v;
                parseJson("{\"loopStart\":" + std::to_string(env.loopStart) + ",\"loopEnd\":" + std::to_string(env.loopEnd) + "}", v);
                fresh.members.insert(fresh.members.end(), v.members.begin(), v.members.end());
                Json points;
                points.type = Json::Array;
                for (size_t m = 0; m < env.count(); ++m) {
                    Json pt;
                    parseJson("[" + formatNumber("%.7g", env.dt[m]) + "," + formatNumber("%.7g", env.level[m]) + "," + formatNumber("%.7g", env.slope[m]) + "]", pt);
                    points.items.push_back(pt);
                }
                fresh.members.push_back({"points", points});
            } else {
                std::vector<std::string> tpls;
                for (auto& slot : slots.items)
                    for (auto& t : slotParams(g, slot, table))
                        if (std::find(tpls.begin(), tpls.end(), t.first) == tpls.end()) tpls.push_back(t.first);
                Json params;
                params.type = Json::Object;
                for (auto& t : tpls) {
                    int q = s.indexOf(g.subst(*h.node, t));
                    if (q < 0) continue;
                    Json v;
                    v.type = Json::Number;
                    v.n = s.get((size_t)q);
                    params.members.push_back({t, v});
                }
                fresh.members.push_back({"params", params});
            }
            slots.items[(size_t)k] = fresh;
            s.setData(slotsKey, writeJson(slots));
            return;
        }
        const Json& slot = slots[(size_t)k];
        if (slot.has("points")) {
            Env env;
            env.loopStart = slot["loopStart"].integer();
            env.loopEnd = slot["loopEnd"].integer(1);
            for (auto& pt : slot["points"].items) {
                env.dt.push_back(pt[0].num());
                env.level.push_back(pt[1].num());
                env.slope.push_back(pt[2].num(0.5));
            }
            if (env.count() >= 2) s.setData(key, formatEnv(env));
        } else {
            std::vector<std::pair<int, double>> writes;
            for (auto& t : slotParams(g, slot, table))
                for (int q : g.paramsFor(*h.node, t.first)) writes.push_back({q, t.second});
            g.setParams(writes);
        }
        if (!nameKey.empty()) s.setData(nameKey, slot["name"].str());
    });
}

// ---- stage_env: an envelope of stages whose levels and times are params (the FS1R's EGs) -----------

// Fields: "start" (the level at key on: a param id or a number), "points" ([[time, level], ...], each a
// param id or a number; a point's time is how long the stage into it takes), "sustain" (the index of
// the point held until key off; the points after it are the release), "range" ([lo, hi] of the
// levels, default the first level param's range), "gap" (the key-off plateau in time units, default
// 25), "minTime" (the least time shown across the width, default 150), colours grid (the zero line),
// area, curve, handle (a point's inside) and dots (the key-off marker). Dragging a point moves its
// time along x and its level along y; Shift is fine.
//
// With "model" naming a registered StageModel (hollow.h), the widget draws that model's curve instead,
// on a time axis in seconds. Each handle sits where the curve reaches its point.
// "points" still names the params a handle drags. Extra fields:
//   "inputs"       {name: param id or number}, passed to the model
//   "minSpan"      shortest time across the width, in seconds (default 0.1)
//   "dbParam"      a param; while off, a dB model is shown as linear amplitude
//   "overlay"      {"var", "values", "param"}: also draws the model for the var's other values while param is on
//   "overlayCurve" the overlay's colour
//   "syncParam"    a param; while on, the view's stage_envs naming it share one time axis
//   text.font      labels the time grid
// Dragging keeps the handle under the pointer. The curve is drawn from unrounded values during the drag.
struct StageVal {
    int param = -1;
    double v = 0;
};

struct StageGeo {
    std::vector<StageVal> t, l;   // per point
    std::vector<int> x, y;        // the start, then one vertex per point (the plateau's end after the sustain point)
    std::vector<int> hx, hy;      // each point's handle
    int keyOff = -1, zero = -1;
    double lo = 0, hi = 1, scale = 1;
    Rect in;
};

struct SigState : KindState {
    std::vector<double> sig;   // the values of every param the widget's fields name, at the last draw
};

using StageIn = std::map<std::string, double>;

struct StageAxis {
    double keyOff = 0, span = 1;   // in seconds: where key off is drawn, and the width of the fitted plot
};

struct StageZoom {
    bool on = false;               // zoomed or panned by the user
    double v0 = 0, span = 1;       // first second shown, and how many
};

struct StageState : SigState {
    int drag = -1, x0 = 0, y0 = 0;
    double t0 = 0, l0 = 0;
    bool pinned = false;           // freeze the axis during a drag
    StageAxis pin;
    double grabX = 0, grabY = 0;   // handle minus pointer, at the press
    std::map<int, double> fine;    // unrounded values while dragging, by param
    StageZoom zoom;
    bool pan = false;              // dragging empty plot
    int panX = 0;
    double panV0 = 0;
    double fitSpan = 0;            // last fitted width, see stageFit
    std::vector<int> shown;        // the params last drawn; refit when they change
};

static void fieldValues(Gui& g, const Hit& h, const Json& j, std::vector<double>& out) {
    if (j.type == Json::String) {
        int p = g.paramOf(h, j.s);
        if (p >= 0) out.push_back(g.state().get(p));
    }
    for (auto& i : j.items) fieldValues(g, h, i, out);
    for (auto& m : j.members) fieldValues(g, h, m.second, out);
}

// Redraws when any param a field names changes (the kinds below read params by their own fields).
template <class T> static void sigTick(Gui& g, const Hit& h, Rect r) {
    std::vector<double> v;
    fieldValues(g, h, g.wid(h).json, v);
    T& st = kindState<T>(g, h);
    if (v != st.sig) {
        st.sig = v;
        g.invalidate(r);
    }
}

static StageVal stageVal(Gui& g, const Hit& h, const Json& j) {
    StageVal v;
    if (j.type == Json::String) {
        v.param = g.paramOf(h, j.str());
        v.v = v.param >= 0 ? g.state().get(v.param) : 0;
    } else {
        v.v = j.num();
    }
    return v;
}

static StageGeo stageGeo(Gui& g, const Hit& h, Rect r) {
    const Json& j = g.wid(h).json;
    const Json& pts = j["points"];
    StageGeo e;
    StageVal start = stageVal(g, h, j["start"]);
    int sus = j["sustain"].integer(-1);
    double gap = sus >= 0 ? j["gap"].num(25) : 0, total = gap;
    for (size_t i = 0; i < pts.size(); ++i) {
        e.t.push_back(stageVal(g, h, pts[i][0]));
        e.l.push_back(stageVal(g, h, pts[i][1]));
        total += std::max(0.0, e.t.back().v);
    }
    if (j["range"].size() == 2) {
        e.lo = j["range"][0].num(), e.hi = j["range"][1].num(0);
    } else {
        int p = start.param;
        for (auto& l : e.l) p = p >= 0 ? p : l.param;
        if (p >= 0) e.lo = g.state().def(p).min, e.hi = g.state().def(p).max;
    }
    if (e.hi <= e.lo) e.hi = e.lo + 1;
    int pad = g.skin().dp(6);
    e.in = {r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad};
    e.scale = (e.in.w - 1) / std::max(total, j["minTime"].num(150));
    auto Y = [&](double v) { return e.in.y + (e.in.h - 1) - (int)std::lround((v - e.lo) / (e.hi - e.lo) * (e.in.h - 1)); };
    if (e.lo < 0 && e.hi > 0) e.zero = Y(0);
    double x = e.in.x;
    e.x.push_back(e.in.x);
    e.y.push_back(Y(start.v));
    for (size_t i = 0; i < e.t.size(); ++i) {
        x += std::max(0.0, e.t[i].v) * e.scale;
        e.hx.push_back((int)std::lround(x));
        e.hy.push_back(Y(e.l[i].v));
        e.x.push_back(e.hx.back());
        e.y.push_back(e.hy.back());
        if ((int)i == sus) {   // held until key off
            x += gap * e.scale;
            e.keyOff = (int)std::lround(x);
            e.x.push_back(e.keyOff);
            e.y.push_back(e.hy.back());
        }
    }
    return e;
}

// A curve through canvas points, `width` design units wide, over its glow (a line four times as wide, if
// the glow colour is set).
static void drawCurve(Gui& g, Canvas& c, const std::vector<std::pair<double, double>>& pts, uint32_t col, uint32_t glow, double width) {
    double lw = std::max(1.0, width * g.skin().density);
    if (glow >> 24) drawPolyline(c, pts, glow, 4 * lw);
    drawPolyline(c, pts, col, lw);
}

// ---- the model path

static std::map<std::string, StageModel>& stageModels() {
    static std::map<std::string, StageModel> m;
    return m;
}

void registerStageModel(const std::string& name, StageModel model) { stageModels()[name] = std::move(model); }

static const StageModel* stageModel(const Widget& w) {
    auto it = stageModels().find(w.json["model"].str());
    return it == stageModels().end() ? nullptr : &it->second;
}

// Replaces {var} in a param template with val.
static std::string withVar(std::string s, const std::string& var, const std::string& val) {
    if (var.empty()) return s;
    std::string key = "{" + var + "}";
    for (size_t pos; (pos = s.find(key)) != std::string::npos;) s.replace(pos, key.size(), val);
    return s;
}

static std::string jsonText(const Json& v) { return v.type == Json::String ? v.s : formatNumber("%g", v.num()); }

// The widget's "inputs": each one's value, and the param it comes from (-1 for a number).
struct StageInputs {
    StageIn in;
    std::map<std::string, int> param;
};

static StageInputs stageInputs(Gui& g, const Hit& h, const std::string& var = "", const std::string& val = "") {
    StageInputs s;
    for (auto& m : g.wid(h).json["inputs"].members) {
        int p = -1;
        double v = m.second.num();
        if (m.second.type == Json::String) {
            p = g.paramOf(h, withVar(m.second.s, var, val));
            v = p >= 0 ? g.state().get(p) : 0;
        }
        s.in[m.first] = v;
        s.param[m.first] = p;
    }
    return s;
}

static StageCurve stageCurve(Gui& g, const Hit& h, const StageIn& in) { return (*stageModel(g.wid(h)))(in); }

static double stageTime(const StageCurve& c, size_t i, const StageAxis& a) { return (int)i < c.keyOff ? c.t[i] : a.keyOff + c.t[i]; }
static double stageNote(const StageCurve& c) { return c.t[(size_t)c.keyOff - 1]; }
static double stageRelease(const StageCurve& c) { return c.t.back(); }

// Fits the time axis to the longest curve, with 20% spare.
// Keeps the previous width while the curves still fill at least a quarter of it,
// so small edits don't rescale the plot.
static StageAxis stageFit(const std::vector<StageCurve>& cs, double minSpan, double keep) {
    double note = 0, rel = 0;
    for (auto& c : cs) note = std::max(note, stageNote(c)), rel = std::max(rel, stageRelease(c));
    StageAxis a;
    a.span = std::max(minSpan, (note + rel) * 1.2);
    if (a.span <= keep && a.span * 4 > keep) a.span = keep;
    a.keyOff = std::max(note, a.span - rel);
    return a;
}

struct StageView {
    Rect in;
    StageAxis a;
    double v0 = 0, span = 1; // seconds shown: from v0, span wide
    double lo = 0, hi = 1;   // level axis
    bool lin = false;        // dB curve drawn as linear amplitude
    double xs = 1;           // pixels per second
    double X(double t) const { return in.x + (t - v0) * xs; }
    double T(double x) const { return v0 + (x - in.x) / xs; }
    double Y(const StageCurve& c, double v) const {
        double d = c.db && lin ? (v <= -150 ? 0.0 : std::pow(10.0, v / 20.0)) : std::clamp(v, lo, hi);
        return in.y + (in.h - 1) - (d - lo) / (hi - lo) * (in.h - 1);
    }
    int Y(double d) const { return in.y + (in.h - 1) - (int)std::lround((d - lo) / (hi - lo) * (in.h - 1)); }
    // Pixel position of point i on a curve. False if the curve never reaches it.
    bool corner(const StageCurve& c, int i, double& x, double& y) const {
        if (i < 0 || i >= (int)c.corner.size() || c.corner[(size_t)i] < 0) return false;
        const size_t at = (size_t)c.corner[(size_t)i];
        x = X(stageTime(c, at, a)), y = Y(c, c.v[at]);
        return true;
    }
};

struct StageModelGeo {
    StageView vw;
    StageInputs own;
    StageCurve curve;                 // this widget's
    std::vector<StageCurve> others;   // overlay curves
    std::vector<StageVal> t, l;       // each point's time and level
    std::vector<int> hx, hy, hi;      // handles in view, and which point each is
    const Font* font = nullptr;       // for the time labels
};

static bool stageOverlayOn(Gui& g, const Hit& h) {
    const Json& ov = g.wid(h).json["overlay"];
    if (!ov.has("var")) return false;
    int p = ov.has("param") ? g.paramOf(h, ov["param"].str()) : -1;
    return p < 0 || g.state().get(p) > g.state().def(p).min;
}

// The other stage_envs in this view with the same "syncParam", if it is on.
static std::vector<Hit> stagePeers(Gui& g, const Hit& h) {
    std::vector<Hit> peers;
    const std::string sync = g.wid(h).json["syncParam"].str();
    int p = sync.empty() ? -1 : g.paramOf(h, sync);
    if (p < 0 || g.state().get(p) <= g.state().def(p).min) return peers;
    for (int i = 0; i < (int)h.node->w.size(); ++i) {
        Hit o{h.node, i, h.gen};
        if (i != h.i && g.wid(o).ops == g.wid(h).ops && g.wid(o).json["syncParam"].str() == sync) peers.push_back(o);
    }
    return peers;
}

static StageModelGeo stageModelGeo(Gui& g, const Hit& h, Rect r) {
    const Json& j = g.wid(h).json;
    StageModelGeo e;
    StageState& st = kindState<StageState>(g, h);
    e.own = stageInputs(g, h);
    std::vector<int> shown;
    for (auto& kv : e.own.param) {
        shown.push_back(kv.second);
        if (st.fine.count(kv.second)) e.own.in[kv.first] = st.fine[kv.second];   // unrounded during a drag
    }
    if (shown != st.shown) st.shown = shown, st.zoom = {}, st.fitSpan = 0;
    e.curve = stageCurve(g, h, e.own.in);
    std::vector<StageCurve> all = {e.curve};
    if (stageOverlayOn(g, h)) {
        const Json& ov = j["overlay"];
        std::string var = ov["var"].str(), cur = g.subst(*h.node, "{" + var + "}");
        for (auto& v : ov["values"].items)
            if (jsonText(v) != cur) e.others.push_back(stageCurve(g, h, stageInputs(g, h, var, jsonText(v)).in));
        all.insert(all.end(), e.others.begin(), e.others.end());
    }
    const std::vector<Hit> peers = stagePeers(g, h);
    for (auto& o : peers) all.push_back(stageCurve(g, o, stageInputs(g, o).in));
    for (auto& p : j["points"].items) {
        e.t.push_back(stageVal(g, h, p[0]));
        e.l.push_back(stageVal(g, h, p[1]));
    }
    StageView& vw = e.vw;
    if (g.wid(h).text.font >= 0) e.font = &g.skin().fonts[(size_t)g.wid(h).text.font];
    int pad = g.skin().dp(6), top = e.font ? e.font->height + g.skin().dp(4) : pad;   // room for the time labels above the plot
    vw.in = {r.x + pad, r.y + top, r.w - 2 * pad, r.h - top - pad};
    vw.a = stageFit(all, j["minSpan"].num(0.1), st.fitSpan);
    st.fitSpan = vw.a.span;
    for (auto& o : peers) kindState<StageState>(g, o).fitSpan = vw.a.span;   // keep synced plots the same width
    if (st.pinned) {   // grows only if the dragged curve no longer fits
        vw.a.keyOff = std::max(st.pin.keyOff, stageNote(e.curve));
        vw.a.span = std::max(st.pin.span, vw.a.keyOff + stageRelease(e.curve));
    }
    int dbp = j.has("dbParam") ? g.paramOf(h, j["dbParam"].str()) : -1;
    vw.lin = e.curve.db && dbp >= 0 && g.state().get(dbp) <= g.state().def(dbp).min;
    vw.lo = vw.lin ? 0 : e.curve.lo;
    vw.hi = vw.lin ? 1 : e.curve.hi;
    vw.span = st.zoom.on ? std::min(st.zoom.span, vw.a.span) : vw.a.span;   // can't zoom out past the fit
    vw.v0 = st.zoom.on ? std::min(st.zoom.v0, vw.a.span - vw.span) : 0;
    vw.xs = (vw.in.w - 1) / vw.span;
    for (size_t i = 0; i < e.t.size(); ++i) {
        double x, y;
        if ((e.t[i].param < 0 && e.l[i].param < 0) || !vw.corner(e.curve, (int)i, x, y)) continue;
        if (x < vw.in.x || x >= vw.in.x + vw.in.w) continue;   // out of view
        e.hx.push_back((int)std::lround(x)), e.hy.push_back((int)std::lround(y)), e.hi.push_back((int)i);
    }
    return e;
}

// Draws one curve, clipped to the plot, antialiased and `width` design units wide over its glow.
// With `area` set it also fills between the curve and `base`, fading to areaBottom at the base.
static void stageTrace(Gui& g, Canvas& cv, const StageView& vw, const StageCurve& c, uint32_t col, uint32_t glow, double width, uint32_t area, uint32_t areaBottom, int base) {
    const double left = vw.in.x - 1, right = vw.in.x + vw.in.w;
    std::vector<uint32_t> row((size_t)std::max(0, vw.in.h));   // fill colour per row
    if (area >> 24) {
        double reach = std::max(1, std::max(base - vw.in.y, vw.in.y + vw.in.h - 1 - base));
        for (int y = 0; y < vw.in.h; ++y) row[(size_t)y] = mixColour(areaBottom, area, std::min(1.0, std::abs(vw.in.y + y - base) / reach));
    }
    Rect saved = cv.clip;
    cv.clip = cv.clip & Rect{vw.in.x, cv.clip.y, vw.in.w, cv.clip.h};
    std::vector<std::pair<double, double>> pts;   // the visible stretch of the curve
    for (size_t i = 0; i + 1 < c.t.size(); ++i) {
        double x0 = vw.X(stageTime(c, i, vw.a)), x1 = vw.X(stageTime(c, i + 1, vw.a));
        double y0 = vw.Y(c, c.v[i]), y1 = vw.Y(c, c.v[i + 1]);
        if (x1 < left || x0 > right) continue;
        if (x1 > x0) {   // clip, so we don't walk a long line pixel by pixel
            auto at = [&](double x) { return y0 + (y1 - y0) * (x - x0) / (x1 - x0); };
            if (x0 < left) y0 = at(left), x0 = left;
            if (x1 > right) y1 = at(right), x1 = right;
            if (area >> 24)
                for (int x = (int)std::ceil(x0); x < (int)std::ceil(x1); ++x) {
                    int y = (int)std::lround(at(x));
                    for (int yy = std::max(vw.in.y, std::min(y, base)); yy < std::min(vw.in.y + vw.in.h, std::max(y, base)); ++yy) pixel(cv, x, yy, row[(size_t)(yy - vw.in.y)]);
                }
        }
        if (pts.empty() || pts.back() != std::make_pair(x0 + 0.5, y0 + 0.5)) pts.push_back({x0 + 0.5, y0 + 0.5});
        pts.push_back({x1 + 0.5, y1 + 0.5});
    }
    if (pts.size() == 1) pts.push_back(pts[0]);
    drawCurve(g, cv, pts, col, glow, width);
    cv.clip = saved;
}

static std::string stageSeconds(double s) {
    if (s < 1) return formatNumber(s < 0.01 ? "%.1f ms" : "%.0f ms", s * 1000);
    return formatNumber(s < 10 && s != std::floor(s) ? "%.1f s" : "%.0f s", s);
}

static void stageModelDraw(Gui& g, Canvas& c, const Hit& h, Rect r, const StageModelGeo& e) {
    const Widget& w = g.wid(h);
    const StageView& vw = e.vw;
    const int k = g.skin().lw();
    uint32_t grid = colourField(w, "grid", 0xffc3c3c3), area = colourField(w, "area", 0xa5fbffff), curve = colourField(w, "curve", 0xff888888),
             inside = colourField(w, "handle", 0xfffdfdfd), dots = colourField(w, "dots", 0xff0c0c0c),
             faint = colourField(w, "overlayCurve", (curve & 0x00ffffff) | 0x60000000),
             areaBottom = colourField(w, "area2", (area & 0x00ffffff) | ((uint32_t)((area >> 24) * 0.15) << 24));
    Rect saved = c.clip;
    c.clip = c.clip & r;
    // Time grid: a dotted line at a round step, roughly every 80 px.
    static const double kSteps[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100};
    double step = kSteps[sizeof kSteps / sizeof *kSteps - 1];
    for (double s : kSteps)
        if (s * vw.xs >= g.skin().dp(80)) {
            step = s;
            break;
        }
    for (long long i = std::max(1LL, (long long)std::ceil(vw.v0 / step)); (double)i * step < vw.v0 + vw.span; ++i) {
        int x = (int)std::lround(vw.X((double)i * step));
        for (int y = vw.in.y; y < vw.in.y + vw.in.h; y += 3 * k) fillRect(c, {x, y, k, k}, grid);
        const std::string label = stageSeconds((double)i * step);
        const int lx = x + g.skin().dp(3);
        if (e.font && lx + e.font->width(label) <= r.x + r.w) drawText(c, *e.font, label, {lx, r.y + g.skin().dp(2), r.w, e.font->height}, 0, 0, false);
    }
    if (e.curve.db && !vw.lin)   // every 24 dB
        for (double d = vw.hi - 24; d > vw.lo; d -= 24) hline(c, vw.in.x, vw.Y(d), vw.in.w, (grid & 0x00ffffff) | 0x50000000);
    int zero = vw.lo < 0 && vw.hi > 0 ? vw.Y(0.0) : -1;
    if (zero >= 0) hline(c, vw.in.x, zero, vw.in.w, grid);
    int base = zero >= 0 ? zero : vw.in.y + vw.in.h - 1;
    for (auto& o : e.others) stageTrace(g, c, vw, o, faint, 0, 1, 0, 0, base);
    stageTrace(g, c, vw, e.curve, curve, colourField(w, "glow", 0), w.json["width"].num(1), area, areaBottom, base);
    int ko = (int)std::lround(vw.X(vw.a.keyOff));
    if (ko >= vw.in.x && ko < vw.in.x + vw.in.w)
        for (int y = vw.in.y; y < vw.in.y + vw.in.h; y += 2 * k) fillRect(c, {ko, y, 1, k}, dots);
    StageState& st = kindState<StageState>(g, h);
    uint32_t ring = colourField(w, "ring", 0);
    for (size_t i = 0; i < e.hx.size(); ++i) handleDot(g, c, e.hx[i] + 0.5, e.hy[i] + 0.5, inside, ring, st.drag == e.hi[i]);
    c.clip = saved;
}

static bool stageModelDown(Gui& g, const Hit& h, int x, int y, const StageModelGeo& e) {
    StageState& st = kindState<StageState>(g, h);
    int best = -1, reach = g.skin().dp(8);
    for (size_t i = 0; i < e.hx.size(); ++i) {   // the nearest handle in reach; the later one on a tie
        int d = std::max(std::abs(x - e.hx[i]), std::abs(y - e.hy[i]));
        if (d > reach) continue;
        best = e.hi[i], reach = d;
        st.grabX = e.hx[i] - x, st.grabY = e.hy[i] - y;
    }
    st.drag = best;
    st.pan = best < 0;
    if (st.pan) {
        st.panX = x, st.panV0 = e.vw.v0;
        return true;
    }
    st.x0 = x, st.y0 = y;
    st.pinned = true, st.pin = e.vw.a;
    g.beginParams({e.t[(size_t)best].param, e.l[(size_t)best].param});
    return true;
}

// Copies this widget's zoom to the widgets it is synced with.
static void stageShare(Gui& g, const Hit& h) {
    const StageState& st = kindState<StageState>(g, h);
    for (auto& o : stagePeers(g, h)) {
        StageState& peer = kindState<StageState>(g, o);
        peer.zoom = st.zoom, peer.fitSpan = st.fitSpan;
        g.invalidate(g.rectOf(*o.node, o.i));
    }
}

// Sets the view after a zoom or pan, limited to the fitted range.
// Zooming all the way out goes back to the fit.
static void stageZoom(Gui& g, const Hit& h, const StageView& vw, double v0, double span) {
    StageState& st = kindState<StageState>(g, h);
    span = std::clamp(span, 0.002, vw.a.span);
    st.zoom = {span < vw.a.span, std::clamp(v0, 0.0, vw.a.span - span), span};
    if (!st.zoom.on) st.fitSpan = 0;   // refit tightly
    stageShare(g, h);
}

// Moves the dragged point to the pointer.
// Level first, then time: each is solved by bisection through the model, as a fractional param value.
// This assumes the point moves monotonically with the param.
// The curve is drawn from the fractional values; the params get them rounded.
static void stageModelDrag(Gui& g, const Hit& h, int x, int y, bool shift, const StageModelGeo& e) {
    StageState& st = kindState<StageState>(g, h);
    const double f = shift ? 0.1 : 1;
    const Rect& r = e.vw.in;
    const double px = std::clamp(st.x0 + (x - st.x0) * f + st.grabX, (double)r.x, (double)(r.x + r.w - 1));
    const double py = std::clamp(st.y0 + (y - st.y0) * f + st.grabY, (double)r.y, (double)(r.y + r.h - 1));
    StageIn in = e.own.in;
    // Where the point ends up, along x or y. Far away if the curve never reaches it.
    auto land = [&](bool alongX) {
        double cx = 1e9, cy = 1e9;
        e.vw.corner(stageCurve(g, h, in), st.drag, cx, cy);
        return alongX ? cx : cy;
    };
    // Finds the value of param p that puts the point at goal. Skipped if p doesn't move the point.
    auto solve = [&](int p, double goal, bool alongX) {
        double* v = nullptr;
        for (auto& kv : e.own.param)
            if (p >= 0 && kv.second == p) v = &in[kv.first];
        if (!v) return;
        const ParamDef& d = g.state().def(p);
        const double now = *v;
        double a = d.min, b = d.max;
        const double fa = (*v = a, land(alongX)), fb = (*v = b, land(alongX));
        for (int n = 0; n < 24 && fa != fb; ++n) {
            *v = (a + b) / 2;
            ((land(alongX) < goal) == (fa < fb) ? a : b) = *v;
        }
        st.fine[p] = *v = fa != fb ? (a + b) / 2 : now;
        if (std::round(*v) != g.state().get(p)) g.editParam(p, std::round(*v));
    };
    solve(e.l[(size_t)st.drag].param, py, false);
    solve(e.t[(size_t)st.drag].param, px, true);
}

// Redraws when a param changes: the widget's own, a synced widget's or an overlay curve's.
static void stageTick(Gui& g, const Hit& h, Rect r) {
    std::vector<double> v;
    const Json& j = g.wid(h).json;
    fieldValues(g, h, j, v);
    if (stageModel(g.wid(h))) {
        for (auto& o : stagePeers(g, h)) fieldValues(g, o, g.wid(o).json["inputs"], v);
        const Json& ov = j["overlay"];
        std::string var = ov["var"].str();
        v.push_back((double)std::hash<std::string>()(g.subst(*h.node, "{" + var + "}")));
        for (auto& val : ov["values"].items)
            for (auto& kv : stageInputs(g, h, var, jsonText(val)).in) v.push_back(kv.second);
    }
    StageState& st = kindState<StageState>(g, h);
    if (v != st.sig) {
        st.sig = v;
        g.invalidate(r);
    }
}

// ---- the hooks: the model path if the widget has a model, else the legacy drawing

static void stageDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    if (stageModel(w)) return stageModelDraw(g, c, h, r, stageModelGeo(g, h, r));
    StageGeo e = stageGeo(g, h, r);
    const int k = g.skin().lw();
    uint32_t grid = colourField(w, "grid", 0xffc3c3c3), area = colourField(w, "area", 0xa5fbffff), curve = colourField(w, "curve", 0xff888888),
             inside = colourField(w, "handle", 0xfffdfdfd), dots = colourField(w, "dots", 0xff0c0c0c);
    Rect saved = c.clip;
    c.clip = c.clip & r;
    if (e.zero >= 0) hline(c, e.in.x, e.zero, e.in.w, grid);
    int base = e.zero >= 0 ? e.zero : e.in.y + e.in.h - 1;
    uint32_t area2 = colourField(w, "area2", area);
    for (size_t i = 0; i + 1 < e.x.size(); ++i)   // the area between the curve and its base, column by column
        for (int x = e.x[i]; x < e.x[i + 1]; ++x) {
            int y = e.y[i] + (int)std::lround((e.y[i + 1] - e.y[i]) * double(x - e.x[i]) / std::max(1, e.x[i + 1] - e.x[i]));
            areaColumn(c, x, y, base, e.in.y, e.in.y + e.in.h - 1, area, area2);
        }
    if (e.keyOff >= 0)   // key off: a fine dashed line
        for (int y = e.in.y; y < e.in.y + e.in.h; y += 2 * k) fillRect(c, {e.keyOff, y, 1, k}, dots);
    std::vector<std::pair<double, double>> pts;   // the curve, smooth
    for (size_t i = 0; i < e.x.size(); ++i) pts.push_back({e.x[i] + 0.5, e.y[i] + 0.5});
    if (pts.size() == 1) pts.push_back(pts[0]);
    drawCurve(g, c, pts, curve, colourField(w, "glow", 0), w.json["width"].num(1));
    StageState& st = kindState<StageState>(g, h);
    uint32_t ring = colourField(w, "ring", 0);
    for (size_t i = 0; i < e.hx.size(); ++i) {
        if (e.t[i].param < 0 && e.l[i].param < 0) continue;
        handleDot(g, c, e.hx[i] + 0.5, e.hy[i] + 0.5, inside, ring, st.drag == (int)i);
    }
    c.clip = saved;
}

static bool stageDown(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    if (stageModel(g.wid(h))) {
        g.invalidate(r);
        return stageModelDown(g, h, x, y, stageModelGeo(g, h, r));
    }
    StageGeo e = stageGeo(g, h, r);
    StageState& st = kindState<StageState>(g, h);
    int best = -1, reach = g.skin().dp(8);
    for (size_t i = 0; i < e.hx.size(); ++i) {   // the nearest handle in reach; the later one on a tie
        if (e.t[i].param < 0 && e.l[i].param < 0) continue;
        int d = std::max(std::abs(x - e.hx[i]), std::abs(y - e.hy[i]));
        if (d <= reach) best = (int)i, reach = d;
    }
    st.drag = best;
    if (best < 0) return false;
    st.x0 = x, st.y0 = y, st.t0 = e.t[(size_t)best].v, st.l0 = e.l[(size_t)best].v;
    g.invalidate(r);
    return true;
}

// ponytail: every move is a whole host gesture per param; one gesture per drag if a host minds.
static void stageDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool shift) {
    StageState& st = kindState<StageState>(g, h);
    if (stageModel(g.wid(h))) {
        StageModelGeo m = stageModelGeo(g, h, r);
        if (st.pan) stageZoom(g, h, m.vw, st.panV0 - (x - st.panX) / m.vw.xs, m.vw.span);
        else stageModelDrag(g, h, x, y, shift, m);
        g.invalidate(r);
        return;
    }
    if (st.drag < 0) return;
    StageGeo e = stageGeo(g, h, r);
    double f = shift ? 0.1 : 1;
    const StageVal& t = e.t[(size_t)st.drag];
    const StageVal& l = e.l[(size_t)st.drag];
    auto put = [&](const StageVal& v, double to) {
        if (v.param < 0) return;
        const ParamDef& d = g.state().def(v.param);
        to = std::clamp(std::round(to), d.min, d.max);
        if (to != g.state().get(v.param)) g.setParam(v.param, to);
    };
    put(t, st.t0 + (x - st.x0) * f / e.scale);
    put(l, st.l0 - (y - st.y0) * f * (e.hi - e.lo) / std::max(1, e.in.h - 1));
    g.invalidate(r);
}

static void stageUp(Gui& g, const Hit& h, Rect r, int, int) {
    StageState& st = kindState<StageState>(g, h);
    st.drag = -1;
    st.pinned = st.pan = false;
    st.fine.clear();   // back to the rounded values
    g.invalidate(r);
}

// Double-click: back to the fit. Model path only.
static void stageDbl(Gui& g, const Hit& h, Rect r, int, int) {
    if (!stageModel(g.wid(h))) return;
    StageState& st = kindState<StageState>(g, h);
    st.zoom = {}, st.fitSpan = 0;
    stageShare(g, h);
    g.invalidate(r);
}

// Wheel: zoom around the pointer, 1.25x per notch. Shift+wheel: pan a tenth of the view per notch. Model path only.
static void stageWheel(Gui& g, const Hit& h, Rect r, int x, int, double notches, bool shift) {
    if (!stageModel(g.wid(h))) return;
    const StageView vw = stageModelGeo(g, h, r).vw;
    const double span = shift ? vw.span : std::min(vw.span / std::pow(1.25, notches), vw.a.span);
    stageZoom(g, h, vw, shift ? vw.v0 - notches * 0.1 * vw.span : vw.T(x) - (vw.T(x) - vw.v0) * span / vw.span, span);
    g.invalidate(r);
}

// ---- fseq: a formant sequence from a table (data/<table>.json, one entry per Fseq) -----------------

// Each entry: "frames", and per track (8) "vfreq", "vlevel", "ufreq", "ulevel" as hex strings of one
// byte per frame, and "pitch". Fields: "fseq" (the param whose plain value is the entry's index),
// "table" (default "fseqs"), "layer" ("v" voiced, "u" unvoiced), "loop": [start param, end param],
// colours line (the tracks, their opacity following each frame's level), line2 (the pitch) and dots
// (the loop points). Frequencies run bottom to top, scaled to the range the Fseq's audible frames use.
// With "key" the entry is the text data under it instead, one such entry as JSON (a product's own Fseq).
// "position" names a param holding the frame playback is at, drawn as a solid line in "cursor".
static int hexByte(const std::string& s, int i) {
    auto nib = [](char ch) { return ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10; };
    return (size_t)(2 * i + 1) < s.size() ? nib(s[(size_t)(2 * i)]) * 16 + nib(s[(size_t)(2 * i + 1)]) : 0;
}

static void fseqDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    const Json& j = w.json;
    int p = g.paramOf(h, j["fseq"].str());
    Json own;
    if (j.has("key") && !parseJson(g.state().data(g.subst(*h.node, j["key"].str())), own)) return;
    const Json& f = j.has("key") ? own : g.skin().table(j["table"].str("fseqs"))[(size_t)std::max(0, p >= 0 ? (int)std::lround(g.state().get(p)) : 0)];
    int n = f["frames"].integer();
    if (n < 2) return;
    const int k = g.skin().lw();
    uint32_t col = colourField(w, "line", 0xffffffff), col2 = colourField(w, "line2", 0xffc80800), dots = colourField(w, "dots", 0xffc80800);
    bool u = j["layer"].str() == "u";
    const std::string& pitch = f["pitch"].s;
    std::vector<int> seen;   // the range: the 2nd to 98th percentile of what is heard, so a stray frame does not squash the rest
    for (int i = 0; i < n; ++i) {
        for (int t = 0; t < 8; ++t)
            if (hexByte(f[u ? "ulevel" : "vlevel"][(size_t)t].s, i) >= 4) seen.push_back(hexByte(f[u ? "ufreq" : "vfreq"][(size_t)t].s, i));
        seen.push_back(hexByte(pitch, i));
    }
    std::sort(seen.begin(), seen.end());
    int lo = seen[seen.size() / 50], hi = seen[seen.size() - 1 - seen.size() / 50];
    if (hi <= lo) lo = 0, hi = 127;
    auto in = [&](int b) { return b >= lo && b <= hi; };
    const double span = hi - lo + 2;
    auto X = [&](int i) { return r.x + (int)std::lround(double(i) * (r.w - 1) / (n - 1)); };
    auto Y = [&](int b) { return r.y + (r.h - 1) - (int)std::lround((std::clamp(b, lo, hi) - lo + 1) * (r.h - 1) / span); };
    Rect saved = c.clip;
    c.clip = c.clip & r;
    for (int t = 0; t < 8; ++t) {
        const std::string& fr = f[u ? "ufreq" : "vfreq"][(size_t)t].s;
        const std::string& lv = f[u ? "ulevel" : "vlevel"][(size_t)t].s;
        for (int i = 0; i + 1 < n; ++i) {
            int a = std::min(255, hexByte(lv, i) * 2);
            if (a < 8 || hexByte(lv, i + 1) * 2 < 8 || !in(hexByte(fr, i)) || !in(hexByte(fr, i + 1))) continue;
            line(c, X(i), Y(hexByte(fr, i)), X(i + 1), Y(hexByte(fr, i + 1)), (uint32_t)a << 24 | (col & 0xffffff), k);
        }
    }
    for (int i = 0; i + 1 < n; ++i) line(c, X(i), Y(hexByte(pitch, i)), X(i + 1), Y(hexByte(pitch, i + 1)), col2, k);
    for (auto& lp : j["loop"].items) {
        int q = g.paramOf(h, lp.str());
        if (q < 0) continue;
        int x = X(std::clamp((int)std::lround(g.state().get(q)), 0, n - 1));
        for (int y = r.y; y < r.y + r.h; y += 3 * k) fillRect(c, {x, y, k, 2 * k}, dots);
    }
    if (int q = g.paramOf(h, j["position"].str()); q >= 0)   // where playback is, a solid line in "cursor"
        fillRect(c, {X(std::clamp((int)std::lround(g.state().get(q)), 0, n - 1)), r.y, k, r.h}, colourField(w, "cursor", 0xffe8b84b));
    c.clip = saved;
}

// ---- level_scale: a level key scaling curve over the keyboard (the DX7 and FS1R kind) --------------

// Fields: "breakPoint" (a param whose plain value counts semitones from "breakBase", default 21, A-1),
// "leftDepth", "rightDepth" (0..99), "leftCurve", "rightCurve" (0 -lin, 1 -exp, 2 +exp, 3 +lin),
// colours line, grid (the centre line) and dots (the break point). The shape is drawn, not the chip's
// table: linear across 72 semitones, exponential doubling every 12, both to full depth at the edge.
static void levelScaleDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    const Json& j = w.json;
    auto get = [&](const char* key, double def) {
        int p = g.paramOf(h, j[key].str());
        return p >= 0 ? g.state().get(p) : def;
    };
    const int k = g.skin().lw();
    double bp = j["breakBase"].num(21) + get("breakPoint", 39);
    double depth[2] = {get("leftDepth", 0), get("rightDepth", 0)};
    int curve[2] = {(int)get("leftCurve", 0), (int)get("rightCurve", 0)};
    uint32_t col = colourField(w, "line", 0xffffffff), grid = colourField(w, "grid", 0x80ffffff), dots = colourField(w, "dots", 0xffc80800);
    int mid = r.y + r.h / 2;
    hline(c, r.x, mid, r.w, grid);
    auto Y = [&](int note) {
        int side = note < bp ? 0 : 1;
        double d = std::abs(note - bp), span = side ? 127 - bp : bp;
        double shape = curve[side] == 0 || curve[side] == 3 ? d / std::max(span, 1.0) : (std::pow(2.0, d / 12) - 1) / std::max(std::pow(2.0, span / 12) - 1, 1e-9);
        double v = depth[side] / 99 * shape * (curve[side] >= 2 ? 1 : -1);
        return mid - (int)std::lround(v * (r.h / 2 - k));
    };
    auto X = [&](int note) { return r.x + (int)std::lround(note * (r.w - 1) / 127.0); };
    std::vector<std::pair<double, double>> pts;
    for (int n = 0; n <= 127; ++n) pts.push_back({X(n) + 0.5, Y(n) + 0.5});
    drawPolyline(c, pts, col, std::max(1.0, g.skin().density));
    int bx = X((int)std::lround(bp));
    for (int y = r.y; y < r.y + r.h; y += 2 * k) fillRect(c, {bx, y, k, k}, dots);
}

// ---- key_range: a part's note range on a small keyboard, and its note shift -----------------------

// Fields: "low", "high" (params, MIDI notes), "shift" (a param in semitones, optional), "first" and "count"
// (the keys shown, default 0 and 128), colours white, black, range (white keys inside the range), rangeBlack,
// gap (between keys), arrow (the shift's triangle and label) and rail. Dragging a key moves the nearer end of
// the range to it. With "shift" a strip over the keys is its slider: a rail under the keys the shift's range
// reaches from "centre" (default 60, middle C), a tick at no shift, and a triangle over the key the centre
// moves to, labelled +1, -12 and so on in the widget's text font on the side it moved to, but never left of
// "labelLeft" (skin px from the widget's left, clear of a caption there). A press on the rail puts the triangle
// there and a drag slides it; the strip either side of the rail is the skin's.
struct KeyRangeState : SigState {
    int drag = 0;   // 1 low, 2 high, 3 shift
};

static void keyRangeDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool);

struct KeyGeo {
    Rect kb;              // the keys
    int strip = 0;        // the shift slider's height over them
    int band = 0, track = 0;   // in the strip: the triangle and label's band (its top, then its height), the track's row
    int first = 0, count = 128;
    std::vector<int> wx;  // each white key's left edge, then the right edge of the last
    double semitone = 1;  // px per semitone across the keys
};

static bool blackKey(int n) { int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

static KeyGeo keyGeo(Gui& g, const Hit& h, Rect r) {
    const Json& j = g.wid(h).json;
    KeyGeo k;
    k.first = std::clamp(j["first"].integer(0), 0, 127);
    k.count = std::clamp(j["count"].integer(128), 1, 128 - k.first);
    int font = g.wid(h).text.font;
    if (j.has("shift") && font >= 0) {   // a margin, the band as tall as the label's capitals, a gap, the rail, a gap
        const Font& f = g.skin().fonts[font];
        k.band = std::max(f.ink1 - f.ink0 + 1, g.skin().dp(4));
        k.track = g.skin().dp(2) + k.band + g.skin().dp(1.5);
        k.strip = k.track + g.skin().lw() + g.skin().dp(2.5);
    }
    k.kb = {r.x, r.y + k.strip, r.w, r.h - k.strip};
    int whites = 0;
    for (int n = k.first; n < k.first + k.count; ++n) whites += !blackKey(n);
    for (int i = 0; i <= whites; ++i) k.wx.push_back(k.kb.x + (int)std::lround(double(i) * k.kb.w / std::max(whites, 1)));
    k.semitone = double(k.kb.w) / k.count;
    return k;
}

// A key's rect: a white key its column less a pixel of gap, a black key 3/5 of a white one over the seam.
static Rect keyRect(const KeyGeo& k, int n) {
    int w = 0;
    for (int m = k.first; m < n; ++m) w += !blackKey(m);
    if (!blackKey(n)) return {k.wx[(size_t)w], k.kb.y, std::max(1, k.wx[(size_t)w + 1] - k.wx[(size_t)w] - 1), k.kb.h};
    int seam = k.wx[(size_t)std::min(w, (int)k.wx.size() - 1)], bw = std::max(1, (int)std::lround((k.wx[1] - k.wx[0]) * 0.6));
    return {seam - bw / 2, k.kb.y, bw, k.kb.h * 3 / 5};
}

static int keyAt(const KeyGeo& k, int x, int y) {
    x = std::clamp(x, k.kb.x, k.kb.x + k.kb.w - 1);
    for (int n = k.first; n < k.first + k.count; ++n)   // the black keys lie over the white ones
        if (blackKey(n) && keyRect(k, n).contains(x, std::max(y, k.kb.y))) return n;
    for (int n = k.first; n < k.first + k.count; ++n)
        if (!blackKey(n)) {
            Rect kr = keyRect(k, n);
            if (x < kr.x + kr.w + 1) return n;
        }
    return k.first + k.count - 1;
}

static double paramValue(Gui& g, const Hit& h, const char* key, double def) {
    int p = g.paramOf(h, g.wid(h).json[key].str());
    return p >= 0 ? g.state().get(p) : def;
}

static void putParam(Gui& g, const Hit& h, const char* key, double v) {
    int p = g.paramOf(h, g.wid(h).json[key].str());
    if (p < 0) return;
    v = std::clamp(std::round(v), g.state().def(p).min, g.state().def(p).max);
    if (v != g.state().get(p)) g.setParam(p, v);
}

// The shift's rail: from the centre's key less the shift's range to the centre's key plus it, in canvas x.
static std::pair<double, double> shiftRail(Gui& g, const Hit& h, const KeyGeo& k) {
    const Json& j = g.wid(h).json;
    int sp = g.paramOf(h, j["shift"].str());
    double centre = j["centre"].num(60), lo = sp >= 0 ? g.state().def(sp).min : -24, hi = sp >= 0 ? g.state().def(sp).max : 24;
    auto X = [&](double note) { return k.kb.x + (note - k.first + 0.5) * k.semitone; };
    return {X(centre + lo), X(centre + hi)};
}

static void keyRangeDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    KeyGeo k = keyGeo(g, h, r);
    int lo = (int)paramValue(g, h, "low", 0), hi = (int)paramValue(g, h, "high", 127);
    if (hi < lo) std::swap(lo, hi);
    uint32_t white = colourField(w, "white", 0xffe6ecee), black = colourField(w, "black", 0xff1d2224), range = colourField(w, "range", 0xff82ca9c),
             rangeBlack = colourField(w, "rangeBlack", 0xff3f7a55), gap = colourField(w, "gap", 0xff0c1112), arrow = colourField(w, "arrow", 0xff17301f);
    fillRect(c, k.kb, gap);
    for (int pass = 0; pass < 2; ++pass)
        for (int n = k.first; n < k.first + k.count; ++n)
            if (blackKey(n) == (pass == 1)) {
                bool in = n >= lo && n <= hi;
                fillRect(c, keyRect(k, n), pass ? (in ? rangeBlack : black) : (in ? range : white));
            }
    if (!k.strip) return;
    int shift = (int)std::lround(paramValue(g, h, "shift", 0));
    double centre = w.json["centre"].num(60);
    auto X = [&](double note) { return k.kb.x + (note - k.first + 0.5) * k.semitone; };
    auto [x0, x1] = shiftRail(g, h, k);
    uint32_t rail = colourField(w, "rail", (arrow & 0xffffff) | 0x66000000);
    double ty = r.y + k.track, lw = g.skin().lw(), tick = g.skin().dp(2);
    fillRect(c, {(int)std::lround(x0), (int)ty, (int)std::lround(x1 - x0), (int)lw}, rail);
    fillRect(c, {(int)X(centre), (int)ty, (int)lw, (int)(tick + lw)}, rail);   // no shift: a tick under the rail, clear of the label
    double tx = X(centre + shift), half = g.skin().dp(3), bottom = r.y + g.skin().dp(2) + k.band;   // the triangle points down at the track
    fillTriangle(c, tx - half, bottom - g.skin().dp(4.5), tx + half, bottom - g.skin().dp(4.5), tx, bottom, arrow);
    if (!shift || w.text.font < 0) return;
    const Font& f = g.skin().fonts[w.text.font];
    std::string label = (shift > 0 ? "+" : "") + std::to_string(shift);
    int tw = f.width(label), gapPx = g.skin().dp(2.5);
    int after = (int)(tx + half) + gapPx, before = (int)(tx - half) - gapPx - tw;   // the side it moved to, else the other
    int minX = r.x + (int)std::lround(w.json["labelLeft"].num(0) * g.skin().scale);
    int lx = shift < 0 ? (before >= minX ? before : after) : (after + tw <= r.x + r.w ? after : before);
    drawText(c, f, label, {lx, r.y + g.skin().dp(2), tw + 1, k.band}, 0, 1, false);
}

static bool keyRangeDown(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    KeyGeo k = keyGeo(g, h, r);
    KeyRangeState& st = kindState<KeyRangeState>(g, h);
    if (y < k.kb.y) {   // the shift's slider: the triangle to a press on its rail
        auto [x0, x1] = shiftRail(g, h, k);
        double reach = g.skin().dp(6);
        if (x < x0 - reach || x > x1 + reach) return false;
        st.drag = 3;
        keyRangeDrag(g, h, r, x, y, false);
        return true;
    }
    int n = keyAt(k, x, y), lo = (int)paramValue(g, h, "low", 0), hi = (int)paramValue(g, h, "high", 127);
    st.drag = n <= lo ? 1 : n >= hi ? 2 : n - lo < hi - n ? 1 : 2;
    putParam(g, h, st.drag == 1 ? "low" : "high", n);
    g.invalidate(r);
    return true;
}

static void keyRangeDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    KeyGeo k = keyGeo(g, h, r);
    KeyRangeState& st = kindState<KeyRangeState>(g, h);
    if (st.drag == 3) {
        double note = k.first + (x - k.kb.x) / std::max(k.semitone, 1e-9) - 0.5;
        putParam(g, h, "shift", note - g.wid(h).json["centre"].num(60));
    } else if (st.drag) {
        int n = keyAt(k, x, k.kb.y + k.kb.h - 1);   // along the white keys, wherever the pointer is
        if (st.drag == 1) putParam(g, h, "low", std::min(n, (int)paramValue(g, h, "high", 127)));
        else putParam(g, h, "high", std::max(n, (int)paramValue(g, h, "low", 0)));
    }
    g.invalidate(r);
}

static void keyRangeUp(Gui& g, const Hit& h, Rect r, int, int) {
    kindState<KeyRangeState>(g, h).drag = 0;
    g.invalidate(r);
}

static void keyRangeDbl(Gui& g, const Hit& h, Rect r, int x, int y) {   // on the rail: no shift; on the keys, as a press
    KeyGeo k = keyGeo(g, h, r);
    if (y >= k.kb.y) return (void)keyRangeDown(g, h, r, x, y, false);
    auto [x0, x1] = shiftRail(g, h, k);
    if (x >= x0 - g.skin().dp(6) && x <= x1 + g.skin().dp(6)) putParam(g, h, "shift", 0);
}

// ---- velocity: a part's velocity range and sense, as the curve from velocity in to velocity out ----

// Fields: "low", "high" (the range a note's velocity must fall in), "depth" and "offset" (params), colours
// grid, area, curve, handle and off (over the velocities outside the range). Out is the FS1R's velocity sense,
// depth * in / 64 + (offset - 64) * 2, clamped to 1..127 (FSVR's src/fs1r/firmware/notes.cpp, note_on, with
// the system's normal curve). The curve's two ends are handles: the left one drags the range's low end and,
// up and down, the offset; the right one the high end and the depth.
struct VelState : SigState {
    int drag = 0, x0 = 0, y0 = 0;   // 1 the low end, 2 the high end
    double v0 = 0;
};

struct VelGeo {
    Rect in;
    double lo = 1, hi = 127, depth = 64, offset = 64;
    double out(double v) const { return std::clamp(depth * v / 64 + (offset - 64) * 2, 1.0, 127.0); }
    double X(double v) const { return in.x + 0.5 + (v - 1) * (in.w - 1) / 126; }
    double Y(double v) const { return in.y + 0.5 + (in.h - 1) - (v - 1) * (in.h - 1) / 126; }
};

static VelGeo velGeo(Gui& g, const Hit& h, Rect r) {
    VelGeo e;
    int pad = g.skin().dp(6);
    e.in = {r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad};
    e.lo = paramValue(g, h, "low", 1), e.hi = paramValue(g, h, "high", 127);
    e.depth = paramValue(g, h, "depth", 64), e.offset = paramValue(g, h, "offset", 64);
    return e;
}

static void velDraw(Gui& g, Canvas& c, const Hit& h, Rect r) {
    const Widget& w = g.wid(h);
    VelGeo e = velGeo(g, h, r);
    uint32_t grid = colourField(w, "grid", 0xff5d8a5a), area = colourField(w, "area", 0x4017301f), curve = colourField(w, "curve", 0xff17301f),
             inside = colourField(w, "handle", 0xff9fd8b0), off = colourField(w, "off", 0x3017301f);
    Rect saved = c.clip;
    c.clip = c.clip & r;
    drawPolyline(c, {{e.X(1), e.Y(1)}, {e.X(127), e.Y(127)}}, grid, 1);   // the diagonal: out as in, for reference
    double lo = std::min(e.lo, e.hi), hi = std::max(e.lo, e.hi);
    int xl = (int)e.X(lo), xh = (int)e.X(hi);
    fillRect(c, {r.x, r.y, xl - r.x, r.h}, off);
    fillRect(c, {xh + 1, r.y, r.x + r.w - xh - 1, r.h}, off);
    uint32_t area2 = colourField(w, "area2", area);
    std::vector<std::pair<double, double>> pts;
    for (int x = xl; x <= xh; ++x) {
        double v = 1 + (x + 0.5 - e.in.x - 0.5) * 126 / std::max(1, e.in.w - 1), y = e.Y(e.out(v));
        areaColumn(c, x, (int)y, e.in.y + e.in.h, e.in.y, e.in.y + e.in.h - 1, area, area2);
        pts.push_back({x + 0.5, y});
    }
    drawCurve(g, c, pts, curve, colourField(w, "glow", 0), w.json["width"].num(1));
    VelState& st = kindState<VelState>(g, h);
    for (int i = 1; i <= 2; ++i) {
        double v = i == 1 ? e.lo : e.hi;
        handleDot(g, c, e.X(v), e.Y(e.out(v)), inside, colourField(w, "ring", 0), st.drag == i);
    }
    c.clip = saved;
}

static bool velDown(Gui& g, const Hit& h, Rect r, int x, int y, bool) {
    VelGeo e = velGeo(g, h, r);
    VelState& st = kindState<VelState>(g, h);
    double dl = std::hypot(x - e.X(e.lo), y - e.Y(e.out(e.lo))), dh = std::hypot(x - e.X(e.hi), y - e.Y(e.out(e.hi)));
    st.drag = dl <= dh ? 1 : 2;   // the nearer end, wherever the press
    st.x0 = x, st.y0 = y, st.v0 = st.drag == 1 ? e.offset : e.depth;
    g.invalidate(r);
    return true;
}

static void velDrag(Gui& g, const Hit& h, Rect r, int x, int y, bool shift) {
    VelState& st = kindState<VelState>(g, h);
    if (!st.drag) return;
    VelGeo e = velGeo(g, h, r);
    double v = std::clamp(1 + (x - e.in.x) * 126.0 / std::max(1, e.in.w - 1), 1.0, 127.0);
    double dOut = -(y - st.y0) * 126.0 / std::max(1, e.in.h - 1) * (shift ? 0.1 : 1);   // velocity units, up positive
    if (st.drag == 1) {
        putParam(g, h, "low", std::min(v, e.hi));
        putParam(g, h, "offset", st.v0 + dOut / 2);
    } else {
        putParam(g, h, "high", std::max(v, e.lo));
        putParam(g, h, "depth", st.v0 + dOut * 64 / std::max(e.hi, 1.0));
    }
    g.invalidate(r);
}

static void velUp(Gui& g, const Hit& h, Rect r, int, int) {
    kindState<VelState>(g, h).drag = 0;
    g.invalidate(r);
}

// ---- the table ------------------------------------------------------------------------------------

const KindOps* findKind(const std::string& kind) {
    //                                     draw              down       drag       up       dbl         hover    right     tick
    static const KindOps pad          = {padDraw,          padDown,   padDrag,   padUp,   padReset,   nullptr, nullptr,  nullptr};
    static const KindOps morph        = {morphDraw,        morphDown, morphDrag, padUp,   morphReset, nullptr, nullptr,  morphTick};
    static const KindOps piano        = {pianoDraw,        pianoDown, pianoDrag, pianoUp, nullptr,    nullptr, nullptr,  pianoTick};
    static const KindOps waveform     = {waveDraw,         nullptr,   nullptr,   nullptr, nullptr,    nullptr, nullptr,  nullptr};
    static const KindOps spectrum     = {spectrumDraw,     nullptr,   nullptr,   nullptr, nullptr,    nullptr, nullptr,  spectrumTick};
    static const KindOps spectrumWave = {spectrumWaveDraw, nullptr,   nullptr,   nullptr, nullptr,    nullptr, nullptr,  spectrumWaveTick};
    static const KindOps wires        = {wiresDraw,        nullptr,   nullptr,   nullptr, nullptr,    nullptr, nullptr,  wiresTick};
    static const KindOps box          = {boxDraw,          boxDown,   nullptr,   nullptr, boxDbl,     nullptr, boxRight, nullptr};
    static const KindOps envelope     = {envDraw,          envDown,   envDrag,   envUp,   envFit,     nullptr, envRight, envTick};
    static const KindOps keyscale     = {ksDraw,           ksDown,    ksDrag,    ksUp,    ksFit,      nullptr, ksRight,  ksTick};
    static const KindOps stage        = {stageDraw,        stageDown, stageDrag, stageUp, stageDbl,   nullptr, nullptr,  stageTick, stageWheel};
    static const KindOps fseq         = {fseqDraw,         nullptr,   nullptr,   nullptr, nullptr,    nullptr, nullptr,  sigTick<SigState>};
    static const KindOps levelScale   = {levelScaleDraw,   nullptr,   nullptr,   nullptr, nullptr,    nullptr, nullptr,  sigTick<SigState>};
    static const KindOps keyRange     = {keyRangeDraw, keyRangeDown, keyRangeDrag, keyRangeUp, keyRangeDbl, nullptr, nullptr, sigTick<KeyRangeState>};
    static const KindOps velocity     = {velDraw,          velDown,   velDrag,   velUp,   nullptr,    nullptr, nullptr,  sigTick<VelState>};
    if (kind == "pad") return &pad;
    if (kind == "morph_pad") return &morph;
    if (kind == "piano") return &piano;
    if (kind == "waveform") return &waveform;
    if (kind == "spectrum") return &spectrum;
    if (kind == "spectrum_wave") return &spectrumWave;
    if (kind == "matrix_wires") return &wires;
    if (kind == "matrix_op") return &box;
    if (kind == "envelope") return &envelope;
    if (kind == "keyscale") return &keyscale;
    if (kind == "stage_env") return &stage;
    if (kind == "fseq") return &fseq;
    if (kind == "level_scale") return &levelScale;
    if (kind == "key_range") return &keyRange;
    if (kind == "velocity") return &velocity;
    return nullptr;   // "fx_chain" and unknown kinds: the fill only
}

} // namespace hollow
