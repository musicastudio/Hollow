// Software renderer: alpha blending, tiles, nine-slice and picture-font text into a 32-bit canvas.
#include "core.h"
#include <algorithm>

namespace hollow {

// Over an opaque canvas the blends are integer ones: images
// and glyphs (s a + d (255 - a)) >> 8, fills and single pixels (s a + d (256 - a)) >> 8, both
// truncating, so a translucent edge comes out up to a level darker than an exact blend would.
template <uint32_t Rest> static inline void blend(uint32_t& d, uint32_t s) {
    uint32_t sa = s >> 24;
    if (sa == 0) return;
    if (sa == 255) { d = s; return; }
    uint32_t da = d >> 24;
    if (da == 255) {
        uint32_t ia = Rest - sa;
        uint32_t rb = ((s & 0xff00ff) * sa + (d & 0xff00ff) * ia) >> 8 & 0xff00ff;
        uint32_t g = ((s & 0xff00) * sa + (d & 0xff00) * ia) >> 8 & 0xff00;
        d = 0xff000000u | rb | g;
        return;
    }
    // straight alpha over a partly transparent canvas (offscreen renders)
    uint32_t dw = da * (255 - sa) / 255, oa = sa + dw;
    if (!oa) { d = 0; return; }
    uint32_t o = oa << 24;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t c = ((s >> sh & 255) * sa + (d >> sh & 255) * dw + oa / 2) / oa;
        o |= c << sh;
    }
    d = o;
}

// A source pixel drawn at window point (x, y) through the surface: light grey pixels take the
// texture's colour there times their own lightness, blended by how light and how grey they are.
// With a reflecting surface the lookup moves by the image pixel's slope (sl) and the sheet's.
static inline uint32_t surfaced(const Surface* sf, uint32_t s, int x, int y, const int8_t* sl = nullptr) {
    if (!sf) return s;
    uint32_t r = s >> 16 & 255, g = s >> 8 & 255, b = s & 255;
    uint32_t hi = std::max({r, g, b}), lo = std::min({r, g, b}), l = (77 * r + 150 * g + 29 * b) >> 8;
    uint32_t w = (uint32_t)(sf->weight[l] * sf->chroma[hi - lo]) >> 8;
    if (!w) return s;
    const Image& t = *sf->tex;
    int ex = x, ey = y;
    if (sl) {
        ex += sl[0] * sf->relief / 128;
        ey += sl[1] * sf->relief / 128;
    }
    if (sf->normals) {
        const Image& n = *sf->normals;
        uint32_t q = n.px[(size_t)(y % n.h) * n.w + (size_t)(x % n.w)];
        ex += ((int)(q >> 16 & 255) - 128) * sf->dents / 128;
        ey += ((int)(q >> 8 & 255) - 128) * sf->dents / 128;
    }
    ex = (ex % t.w + t.w) % t.w;
    ey = (ey % t.h + t.h) % t.h;
    uint32_t p = t.px[(size_t)ey * t.w + (size_t)ex];
    uint32_t nr = (p >> 16 & 255) * l / 255, ng = (p >> 8 & 255) * l / 255, nb = (p & 255) * l / 255;
    r += (int)((int)nr - (int)r) * (int)w / 256;
    g += (int)((int)ng - (int)g) * (int)w / 256;
    b += (int)((int)nb - (int)b) * (int)w / 256;
    return (s & 0xff000000u) | r << 16 | g << 8 | b;
}

void fillRect(Canvas& c, Rect r, uint32_t argb, const Surface* sf) {
    r = r & c.clip;
    for (int y = r.y; y < r.y + r.h; ++y) {
        uint32_t* row = c.px + (size_t)y * c.w;
        for (int x = r.x; x < r.x + r.w; ++x) blend<256>(row[x], surfaced(sf, argb, x, y));
    }
}

// The slope of image pixel (x, y), when the image has one.
static inline const int8_t* slopeAt(const Image& img, int x, int y) {
    return img.slope.empty() ? nullptr : &img.slope[2 * ((size_t)y * img.w + x)];
}

// Copies src (a rect of img) scaled to dst (nearest neighbour), clipped to clip.
static void blit(Canvas& c, const Image& img, Rect src, Rect dst, Rect clip, const Surface* sf = nullptr) {
    if (src.empty() || dst.empty()) return;
    Rect r = dst & clip;
    for (int y = r.y; y < r.y + r.h; ++y) {
        int sy = src.y + (int)((2LL * (y - dst.y) + 1) * src.h / (2LL * dst.h));
        const uint32_t* srow = img.px.data() + (size_t)sy * img.w;
        uint32_t* drow = c.px + (size_t)y * c.w;
        for (int x = r.x; x < r.x + r.w; ++x) {
            int sx = src.w == dst.w ? src.x + (x - dst.x) : src.x + (int)((2LL * (x - dst.x) + 1) * src.w / (2LL * dst.w));
            blend<255>(drow[x], sf ? surfaced(sf, srow[sx], x, y, slopeAt(img, sx, sy)) : srow[sx]);
        }
    }
}

// Fills dst with src repeated from dst's top-left, clipped to clip: the edges and
// the centre of a nine-slice image are drawn this way (corners come out 1:1 since their sizes match).
static void repeat(Canvas& c, const Image& img, Rect src, Rect dst, Rect clip, const Surface* sf) {
    if (src.empty() || dst.empty()) return;
    Rect r = dst & clip;
    for (int y = r.y; y < r.y + r.h; ++y) {
        int sy = src.y + (y - dst.y) % src.h;
        const uint32_t* srow = img.px.data() + (size_t)sy * img.w + src.x;
        uint32_t* drow = c.px + (size_t)y * c.w;
        for (int x = r.x; x < r.x + r.w; ++x) {
            int sx = (x - dst.x) % src.w;
            blend<255>(drow[x], sf ? surfaced(sf, srow[sx], x, y, slopeAt(img, src.x + sx, sy)) : srow[sx]);
        }
    }
}

// "passes": art composited more than once (the same pixels blended again).
void drawImage(Canvas& c, const Image& img, int tile, Rect dst, bool stretch, const Surface* surface) {
    if (img.px.empty()) return;
    const Surface* sf = surface && surface->tex && img.surface ? surface : nullptr;
    Rect t = img.tile(tile);
    Rect clip = c.clip & dst;
    for (int pass = 0; pass < std::max(img.passes, 1); ++pass) {
    if (!img.sliced || (t.w == dst.w && t.h == dst.h)) {
        blit(c, img, t, stretch ? dst : Rect{dst.x, dst.y, t.w, t.h}, clip, sf);
        continue;
    }
    int top = std::min(img.slice[0], t.h), right = std::min(img.slice[1], t.w);
    int bottom = std::min(img.slice[2], t.h - top), left = std::min(img.slice[3], t.w - right);
    int sx[4] = {0, left, t.w - right, t.w}, sy[4] = {0, top, t.h - bottom, t.h};
    int dx[4] = {0, left, dst.w - right, dst.w}, dy[4] = {0, top, dst.h - bottom, dst.h};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            repeat(c, img, {t.x + sx[i], t.y + sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j]},
                   {dst.x + dx[i], dst.y + dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j]}, clip, sf);
    }
}

void bakeSurface(Image& img, int x, int y, const Surface& sf) {
    if (!sf.tex || img.px.empty()) return;
    for (int t = 0; t < std::max(img.tiles, 1); ++t) {
        const Rect tr = img.tile(t);
        for (int py = tr.y; py < tr.y + tr.h; ++py)
            for (int px = tr.x; px < tr.x + tr.w; ++px) {
                uint32_t& p = img.px[(size_t)py * img.w + px];
                p = surfaced(&sf, p, x + px - tr.x, y + py - tr.y, slopeAt(img, px, py));
            }
    }
}

static unsigned nextCode(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    unsigned cp = len == 1 ? c : c & (0x3f >> (len - 1));
    for (int k = 1; k < len && i + k < s.size(); ++k) cp = cp << 6 | ((unsigned char)s[i + k] & 63);
    i += len;
    return cp < 256 ? cp : '?';
}

void drawText(Canvas& c, const Font& f, const std::string& s, Rect r, int align, int valign, bool multiline) {
    std::vector<std::string> lines(1);
    for (size_t i = 0; i < s.size(); ++i) {
        bool br = s[i] == '\n' || (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n');
        if (!br) { lines.back() += s[i]; continue; }
        if (s[i] == '\\') ++i;
        if (!multiline) break;
        lines.emplace_back();
    }
    int total = f.height * (int)lines.size();
    int dy = valign == 1 ? (r.h - total) / 2 : valign == 2 ? r.h - total : 0;
    Rect saved = c.clip;
    for (auto& line : lines) {
        int tw = f.width(line);
        int dx = align == 1 ? (r.w - tw) / 2 : align == 2 ? r.w - tw : 0;
        c.clip = saved & Rect{r.x + dx, r.y + dy, r.w - dx, r.h - dy};
        int x = r.x + dx;
        for (size_t i = 0; i < line.size() && x < c.clip.x + c.clip.w;) {
            unsigned g = nextCode(line, i);
            blit(c, f.img, {f.x[g], f.top, f.w[g], f.height}, {x, r.y + dy, f.w[g], f.height}, c.clip);
            x += f.w[g];
        }
        dy += f.height;
    }
    c.clip = saved;
}

} // namespace hollow
