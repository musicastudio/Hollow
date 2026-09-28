// Skin loading: a minimal JSON reader, PNG decoding (stb_image), fonts, views and widgets.
#include "core.h"
#include "embedded_skin.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb/stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace fs = std::filesystem;

namespace hollow {

// ---- JSON -------------------------------------------------------------------------------------

static const Json kNull;

const Json& Json::operator[](const std::string& key) const {
    if (type == Object)
        for (auto& m : members)
            if (m.first == key) return m.second;
    return kNull;
}

const Json& Json::operator[](size_t i) const {
    return type == Array && i < items.size() ? items[i] : kNull;
}

// Exact (correctly rounded) and locale-independent: from_chars where the library has it for
// doubles, else strtod on a copy whose '.' is the current locale's decimal point.
bool parseNumber(const char*& p, const char* end, double& out) {
    const char* s = p < end && *p == '+' ? p + 1 : p;
    if (s >= end || !(std::isdigit((unsigned char)*s) || *s == '-' || *s == '.')) return false;
#if defined(__cpp_lib_to_chars)
    auto r = std::from_chars(s, end, out);
    if (r.ec != std::errc()) return false;
    p = r.ptr;
#else
    char buf[64];
    size_t n = std::min<size_t>(end - s, sizeof buf - 1);
    std::memcpy(buf, s, n);
    buf[n] = 0;
    char dp = *std::localeconv()->decimal_point;
    for (char* c = buf; *c && dp != '.'; ++c)
        if (*c == '.') *c = dp;
    char* e;
    out = std::strtod(buf, &e);
    if (e == buf) return false;
    p = s + (e - buf);
#endif
    return true;
}

std::string formatNumber(const char* fmt, double v) {
    char buf[128];
    std::snprintf(buf, sizeof buf, fmt, v);
    char dp = *std::localeconv()->decimal_point;   // a host may have set a locale with ','
    if (dp != '.')
        for (char* c = buf; *c; ++c)
            if (*c == dp) *c = '.';
    return buf;
}

namespace {
struct Parser {
    const char* p;
    const char* end;
    std::string err;

    bool fail(const char* m) { if (err.empty()) err = m; return false; }
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if ((size_t)(end - p) < n || std::memcmp(p, w, n) != 0) return fail("bad literal");
        p += n;
        return true;
    }
    static void utf8(std::string& o, unsigned c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xc0 | c >> 6); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xe0 | c >> 12); o += (char)(0x80 | (c >> 6 & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xf0 | c >> 18); o += (char)(0x80 | (c >> 12 & 63)); o += (char)(0x80 | (c >> 6 & 63)); o += (char)(0x80 | (c & 63)); }
    }
    bool hex4(unsigned& c) {
        if (end - p < 4) return fail("bad \\u escape");
        c = 0;
        for (int i = 0; i < 4; ++i) {
            char h = *p++;
            c = c * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 99);
            if (c > 0xffff) return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& o) {
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (p >= end) break;
            char e = *p++;
            switch (e) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                unsigned c1;
                if (!hex4(c1)) return false;
                if (c1 >= 0xd800 && c1 < 0xdc00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned c2;
                    if (!hex4(c2)) return false;
                    c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                }
                utf8(o, c1);
                break;
            }
            default: o += e;
            }
        }
        return fail("unterminated string");
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return fail("nested too deep");
        ws();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        if (c == '{') {
            ++p;
            v.type = Json::Object;
            ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                v.members.emplace_back(std::move(k), Json());
                if (!value(v.members.back().second, depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            v.type = Json::Array;
            ws();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                v.items.emplace_back();
                if (!value(v.items.back(), depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = Json::String; return string(v.s); }
        if (c == 't') { v.type = Json::Bool; v.b = true; return lit("true"); }
        if (c == 'f') { v.type = Json::Bool; return lit("false"); }
        if (c == 'n') return lit("null");
        v.type = Json::Number;
        return parseNumber(p, end, v.n) || fail("bad value");
    }
};
} // namespace

bool parseJson(const std::string& text, Json& out, std::string* error) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    out = Json();
    bool ok = ps.value(out, 0);
    if (ok) {
        ps.ws();
        if (ps.p != ps.end) ok = ps.fail("trailing characters");
    }
    if (!ok && error) {
        int line = 1 + (int)std::count(text.data(), ps.p, '\n');
        *error = ps.err + " at line " + std::to_string(line);
    }
    return ok;
}

std::string jsonQuote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

uint32_t parseColour(const std::string& s, uint32_t def) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return def;
    uint32_t v = (uint32_t)std::strtoul(s.c_str() + 1, nullptr, 16);
    return s.size() == 7 ? 0xff000000u | v : (v >> 8) | (v & 0xff) << 24;
}

// printf of one int: the format must have exactly one d or i conversion (and any %%).
std::string formatInt(const std::string& f, int v) {
    int convs = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') continue;
        if (++i < f.size() && f[i] == '%') continue;
        while (i < f.size() && std::strchr("-+ 0123456789", f[i])) ++i;
        if (i >= f.size() || (f[i] != 'd' && f[i] != 'i')) return std::to_string(v);
        ++convs;
    }
    if (convs != 1) return std::to_string(v);
    char buf[128];
    std::snprintf(buf, sizeof buf, f.c_str(), v);
    return buf;
}

std::string writeJson(const Json& j) {
    switch (j.type) {
    case Json::Null: return "null";
    case Json::Bool: return j.b ? "true" : "false";
    case Json::Number: return formatNumber("%.17g", j.n);
    case Json::String: return jsonQuote(j.s);
    case Json::Array: {
        std::string o = "[";
        for (size_t i = 0; i < j.items.size(); ++i) o += (i ? "," : "") + writeJson(j.items[i]);
        return o + "]";
    }
    default: {
        std::string o = "{";
        for (size_t i = 0; i < j.members.size(); ++i) o += (i ? "," : "") + jsonQuote(j.members[i].first) + ":" + writeJson(j.members[i].second);
        return o + "}";
    }
    }
}

// ---- geometry, images, fonts ------------------------------------------------------------------

Rect Rect::operator&(const Rect& o) const {
    int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    int x1 = std::min(x + w, o.x + o.w), y1 = std::min(y + h, o.y + o.h);
    return x1 > x0 && y1 > y0 ? Rect{x0, y0, x1 - x0, y1 - y0} : Rect{};
}

Rect Rect::operator|(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    int x0 = std::min(x, o.x), y0 = std::min(y, o.y);
    int x1 = std::max(x + w, o.x + o.w), y1 = std::max(y + h, o.y + o.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

Rect Image::tile(int i) const {
    int n = std::max(tiles, 1);
    i = std::clamp(i, 0, n - 1);
    if (axisX) return {i * (w / n), 0, w / n, h};
    return {0, i * (h / n), w, h / n};
}

int Font::width(const std::string& s) const {
    int total = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp = c;
        int len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (len > 1) {
            cp = c & (0x3f >> (len - 1));
            for (int k = 1; k < len && i + k < s.size(); ++k) cp = cp << 6 | ((unsigned char)s[i + k] & 63);
        }
        i += len;
        total += w[cp < 256 ? cp : '?'];
    }
    return total;
}

// A number's "valueText": a data table and how to index it, or a scale.
static void readValueText(const Json& vt, Widget::ValueText& out) {
    if (vt.type != Json::Object) return;
    auto axis = [&](const char* k, int& fixed, std::string& param) {
        if (vt[k].type == Json::Number) fixed = vt[k].integer();
        else param = vt[k].str();
    };
    out.table = vt["table"].str();
    axis("row", out.row, out.rowParam);
    axis("col", out.col, out.colParam);
    out.first = vt["first"].num(0);
    out.scale = vt["scale"].num(0);
    out.format = vt["format"].str();
}

const Json& Skin::table(const std::string& key) const {
    auto it = tables.find(key);
    return it == tables.end() ? kNull : it->second;
}

const View* Skin::view(const std::string& viewName) const {
    auto it = views.find(viewName);
    return it == views.end() ? nullptr : &it->second;
}

// ---- loading ------------------------------------------------------------------------------------

namespace {
using Files = std::map<std::string, std::string>;

std::string utf8(const fs::path& p) {   // generic_u8string is std::u8string from C++20 on
    auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

struct Loader {
    const Files& files;
    Skin& skin;
    const Json& meta;                         // skin.json "images"
    std::map<std::string, int> imageIds, fontIds;
    std::string err;

    bool decode(const std::string& path, Image& img) {
        auto f = files.find(path);
        if (f == files.end()) return false;
        int w, h, n;
        unsigned char* d = stbi_load_from_memory((const unsigned char*)f->second.data(), (int)f->second.size(), &w, &h, &n, 4);
        if (!d) { err += "cannot decode " + path + "\n"; return false; }
        img.w = w;
        img.h = h;
        img.px.resize((size_t)w * h);
        for (size_t i = 0; i < img.px.size(); ++i)
            img.px[i] = (uint32_t)d[i * 4 + 3] << 24 | (uint32_t)d[i * 4] << 16 | (uint32_t)d[i * 4 + 1] << 8 | d[i * 4 + 2];
        stbi_image_free(d);
        return true;
    }

    int image(const std::string& name) {
        if (name.empty()) return -1;
        auto it = imageIds.find(name);
        if (it != imageIds.end()) return it->second;
        Image img;
        int id = -1;
        if (decode("images/" + name + ".png", img)) {
            const Json& m = meta[name];
            img.tiles = std::max(1, m["tiles"].integer(1));
            img.axisX = m["axis"].str("y") == "x";
            img.passes = std::clamp(m["passes"].integer(1), 1, 4);
            img.surface = m["surface"].flag(true);
            if (m["slice"].type == Json::Array) {
                img.sliced = true;
                for (int i = 0; i < 4; ++i) img.slice[i] = std::max(0, m["slice"][i].integer());
            }
            id = (int)skin.images.size();
            skin.images.push_back(std::move(img));
            skin.imageNames.push_back(name);
        } else {
            err += "missing image " + name + "\n";
        }
        return imageIds[name] = id;
    }

    int font(const std::string& name) {
        if (name.empty()) return -1;
        auto it = fontIds.find(name);
        if (it != fontIds.end()) return it->second;
        Font f;
        int id = -1;
        if (decode("fonts/" + name + ".png", f.img) && f.img.w > 0) {
            std::vector<int> marks;
            for (int x = 0; x < f.img.w; ++x)
                if (f.img.px[x] >> 24) marks.push_back(x);
            if (marks.size() == 256) {
                f.top = 1;
                f.height = f.img.h - 1;
                for (int i = 0; i < 256; ++i) {
                    f.x[i] = marks[i];
                    f.w[i] = (i < 255 ? marks[i + 1] : f.img.w) - marks[i];
                }
            } else {
                int cw = f.img.w / 256;
                f.height = f.img.h;
                for (int i = 0; i < 256; ++i) { f.x[i] = i * cw; f.w[i] = cw; }
            }
            uint64_t sum[3] = {0, 0, 0}, n = 0;
            for (size_t i = (size_t)f.top * f.img.w; i < f.img.px.size(); ++i) {
                uint32_t p = f.img.px[i];
                if (p >> 24 < 128) continue;
                sum[0] += p >> 16 & 255; sum[1] += p >> 8 & 255; sum[2] += p & 255; ++n;
            }
            if (n) f.colour = 0xff000000u | (uint32_t)(sum[0] / n) << 16 | (uint32_t)(sum[1] / n) << 8 | (uint32_t)(sum[2] / n);
            id = (int)skin.fonts.size();
            skin.fonts.push_back(std::move(f));
            skin.fontNames.push_back(name);
        } else {
            err += "missing font " + name + "\n";
        }
        return fontIds[name] = id;
    }

    static uint32_t colour(const std::string& s, uint32_t def) { return parseColour(s, def); }

    Fill fill(const Json& j) {
        Fill f;
        if (j.has("colour")) { f.hasColour = true; f.colour = colour(j["colour"].str(), 0); }
        f.image = image(j["image"].str());
        f.tile = j["tile"].integer(-1);
        return f;
    }

    Text text(const Json& j) {
        Text t;
        t.text = j["text"].str();
        t.font = font(j["font"].str());
        std::string a = j["align"].str(), v = j["valign"].str();
        t.align = a == "center" ? 1 : a == "right" ? 2 : 0;
        t.valign = v == "middle" ? 1 : v == "bottom" ? 2 : 0;
        return t;
    }

    static std::string valueText(const Json& v) { return v.type == Json::Number ? formatNumber("%g", v.n) : v.str(); }

    static Vars vars(const Json& j) {
        Vars v;
        for (auto& m : j.members) v.emplace_back(m.first, valueText(m.second));
        return v;
    }

    // "param.id" (true while above its minimum), { "param" | "var", "equals" | "notEquals" |
    // "atLeast" | "atMost": value }, or { "stack", "view", "vars" }.
    static Cond cond(const Json& j) {
        Cond c;
        if (j.type == Json::String) { c.op = Cond::Set; c.param = j.s; return c; }
        if (j.type != Json::Object) return c;
        if (j.has("all") || j.has("any")) {
            c.op = j.has("all") ? Cond::All : Cond::Any;
            for (auto& part : j[j.has("all") ? "all" : "any"].items) c.parts.push_back(cond(part));
            return c;
        }
        if (j.has("stack")) {
            c.op = Cond::Shows;
            c.stack = j["stack"].str();
            c.view = j["view"].str();
            c.vars = vars(j["vars"]);
            return c;
        }
        c.param = j["param"].str();
        c.var = j["var"].str();
        static const std::pair<const char*, Cond::Op> ops[] = {
            {"equals", Cond::Equals}, {"notEquals", Cond::NotEquals}, {"atLeast", Cond::AtLeast}, {"atMost", Cond::AtMost}};
        c.op = Cond::Set;
        for (auto& o : ops)
            if (j.has(o.first)) {
                c.op = o.second;
                c.value = j[o.first].num();
                c.text = valueText(j[o.first]);
            }
        if (c.param.empty() && c.var.empty()) c.op = Cond::None;
        return c;
    }

    static Action action(const Json& a) {
        Action r;
        if (a.has("value")) {   // a radio button, and with a goto a chooser that then shows that view
            r.type = Action::Value; r.amount = a["value"].num(); r.target = a["goto"].str(); r.stack = a["stack"].str(); r.vars = vars(a["vars"]);
        }
        else if (a.has("goto")) { r.type = Action::Goto; r.target = a["goto"].str(); r.stack = a["stack"].str(); r.vars = vars(a["vars"]); }
        else if (a.has("set")) { r.type = Action::Set; r.vars = vars(a["set"]); }
        else if (a.has("toggle")) { r.type = Action::Toggle; r.target = a["toggle"].str(); }
        else if (a.has("url")) { r.type = Action::Url; r.target = a["url"].str(); }
        else if (a.has("standalone")) { r.type = Action::Standalone; r.target = a["standalone"].str(); }
        else if (a.has("file")) {
            r.type = Action::File;
            r.target = a["file"].str();
            r.key = a["key"].str();
            r.nameKey = a["name"].str();
            r.title = a["title"].str();
            for (auto& m : a["types"].members) r.vars.push_back({m.first, m.second.str()});
        }
        else if (a.has("step")) { r.type = Action::Step; r.amount = a["step"].num(); }
        else if (a.has("midi_map")) { r.type = Action::MidiMap; r.target = a["midi_map"].str(); }
        else if (a.has("data")) { r.type = Action::Data; r.vars = vars(a["data"]); }
        else if (a.has("presets")) {
            r.type = Action::Presets;
            r.target = a["presets"].str();
            r.key = a["key"].str();
            r.nameKey = a["name"].str();
            r.save = a["save"].flag();
            r.perColumn = std::max(0, a["columns"].integer());
        } else if (a.has("sequence")) {
            r.type = Action::Sequence;
            r.target = a["sequence"].str();
            r.prefix = a["prefix"].str();
            r.index = valueText(a["index"]);
            r.leaf = a["leaf"].str();
            r.count = std::max(0, a["count"].integer());
        }
        return r;
    }

    static std::vector<Item> items(const Json& j, int depth = 0) {
        std::vector<Item> out;
        for (auto& it : j.items) {
            Item item;
            item.separator = it["separator"].flag();
            item.label = it["label"].str();
            item.shortLabel = it["short"].str(item.label);
            item.value = it["value"].num();
            item.check = it["check"].flag();
            item.columnBreak = it["columnBreak"].flag();
            item.action = action(it["action"]);
            if (depth < 8) item.items = items(it["items"], depth + 1);
            out.push_back(std::move(item));
        }
        return out;
    }

    Widget widget(const Json& j) {
        static const std::map<std::string, Kind> kinds = {
            {"plate", Kind::Plate}, {"button", Kind::Button}, {"dropdown", Kind::Dropdown}, {"dial", Kind::Dial},
            {"number", Kind::Number}, {"textbox", Kind::Textbox}, {"meter", Kind::Meter}, {"plot", Kind::Plot},
            {"veil", Kind::Veil}, {"list", Kind::List}, {"tree", Kind::Tree}, {"embed", Kind::Embed}, {"custom", Kind::Custom}};
        Widget w;
        std::string type = j["type"].str();
        auto k = kinds.find(type);
        if (k != kinds.end()) w.kind = k->second;
        else { w.kind = Kind::Custom; w.customKind = type; }   // unknown types draw their fill, like unknown custom kinds
        w.name = j["name"].str();
        const Json& r = j["rect"];
        w.rect = {r[0].integer(), r[1].integer(), r[2].integer(), r[3].integer()};
        w.layer = std::clamp(j["layer"].integer(3), 0, 7);
        w.surface = j["surface"].flag(true);
        w.hidden = j["hidden"].flag();
        w.disabled = j["disabled"].flag();
        w.showIf = cond(j["showIf"]);
        w.enableIf = cond(j["enableIf"]);
        w.key = j["key"].str();
        w.editable = j["editable"].flag();
        w.status = j["status"].flag();
        w.editOnClick = j["editOn"].str() == "click";
        static const std::map<std::string, Widget::Source> sources = {
            {"cpu", Widget::Cpu}, {"midi_in", Widget::MidiIn}, {"modified", Widget::Modified}, {"voices", Widget::Voices}, {"midi_map", Widget::MidiMap},
            {"scope", Widget::Scope}, {"level_l", Widget::LevelL}, {"level_r", Widget::LevelR}};
        auto src = sources.find(j["source"].str());
        if (src != sources.end()) w.source = src->second;
        w.param = j["param"].str();
        w.tip = j["tip"].str();
        w.value = j["value"].num();
        w.fill = fill(j["fill"]);
        w.text = text(j["text"]);
        for (int i = 0; i < 4; ++i) w.pad[i] = j["pad"][i].integer();
        w.toggle = j["toggle"].flag();
        w.hoverTiles = j["hoverTiles"].flag();
        w.pressedTiles = j["pressedTiles"].flag();
        w.disabledTile = j["disabledTile"].flag();
        w.captionFromItem = j["captionFromItem"].flag();
        std::string press = j["press"].str();
        w.press = press == "momentary" ? PressMomentary : press == "repeat" ? PressRepeat : press == "up" ? PressUp : PressDown;
        w.pressOffset[0] = j["pressOffset"][0].integer();
        w.pressOffset[1] = j["pressOffset"][1].integer();
        w.action = action(j["action"]);
        w.items = items(j["items"]);
        w.context = items(j["context"]);
        w.menuStyle = j["menuStyle"].str();
        w.hasMenuAt = j.has("menuAt");
        w.menuAt[0] = j["menuAt"][0].integer();
        w.menuAt[1] = j["menuAt"][1].integer();
        w.image = image(j["image"].str());
        std::string drag = j["drag"].str();
        w.drag = drag == "absolute" ? DragAbsolute : drag == "rotary" ? DragRotary : DragLinear;
        w.vertical = j["vertical"].flag(true);
        w.slider = j["slider"].flag();
        w.floorFrames = j["frames"].str() == "floor";
        w.sensitivity = j["sensitivity"].num(w.kind == Kind::Number ? 0 : 1);   // number: 0 = range / 200 per pixel
        w.fine = j["fine"].num(0.1);
        w.wheel = j["wheel"].num(w.kind == Kind::Dial ? 0.05 : 0);
        const Json& reset = j["reset"];
        w.reset = reset.type == Json::Bool && !reset.b ? Widget::ResetNone : reset.str() == "zero" ? Widget::ResetZero : Widget::ResetDefault;
        w.margin = j["margin"].integer();
        w.rangeMin = j["range"][0].num(0);
        w.rangeMax = j["range"][1].num(1);
        w.hasRange = j.has("range");
        readValueText(j["valueText"], w.valueText);
        w.steps = std::max(0, j["steps"].integer());
        w.def = j["default"].num(w.rangeMin);
        std::string midi = j["midi"].str();
        if (midi == "bend") w.midi = 128;
        else if (midi.size() > 2 && midi.compare(0, 2, "cc") == 0 && midi.find_first_not_of("0123456789", 2) == std::string::npos)
            w.midi = std::min(std::atoi(midi.c_str() + 2), 127);
        w.spring = j["spring"].num(-1);
        w.dragOn = j["drag"].type != Json::Bool || j["drag"].b;
        for (auto& z : j["sensitivityZones"].items) w.zones.push_back(z.num());
        for (auto& m : j["mirror"].items) w.mirror.push_back(m.str());
        w.hasZeroText = j.has("zeroText");
        w.zeroText = j["zeroText"].str();
        w.format = j["format"].str();
        w.multiline = j["multiline"].flag();
        w.line = colour(j["line"].str(), 0xffffffff);
        w.line2 = colour(j["line2"].str(), w.line);
        static const std::map<std::string, int> draws = {{"trace", 0}, {"bars", 1}, {"bars_trace", 2}, {"centre_bars", 3}};
        auto dm = draws.find(j["draw"].str());
        w.drawMode = dm != draws.end() ? dm->second : 0;
        w.header = text(j["header"]);
        w.row = text(j["row"]);
        w.selectRow = text(j["selectRow"]);
        w.rowHeight = j["rowHeight"].integer();
        w.rowGap = j["rowGap"].integer();
        w.colGap = j["colGap"].integer();
        for (auto& c : j["columns"].items) {
            std::string a = c["align"].str();
            w.columns.push_back({c["width"].integer(), a == "center" ? 1 : a == "right" ? 2 : 0, c["edit"].str() == "int", image(c["image"].str())});
        }
        for (auto& f : j["fixed"].items) w.fixed.push_back({f["param"].str(), f["label"].str(), f["ccParam"].str()});
        for (auto& row : j["rows"].items) {
            w.rows.emplace_back();
            for (auto& cell : row.items) w.rows.back().push_back(valueText(cell));
        }
        auto optColour = [&](const char* key, bool& has, uint32_t& out) {
            has = j.has(key);
            out = colour(j[key].str(), 0);
        };
        optColour("gapFill", w.hasGapFill, w.gapFill);
        optColour("rowFill", w.hasRowFill, w.rowFill);
        optColour("selectFill", w.hasSelectFill, w.selectFill);
        if (w.row.valign == 0 && j["valign"].str() == "middle") w.row.valign = 1;
        w.view = j["view"].str();
        w.vars = vars(j["vars"]);
        w.fit = j["fit"].flag();
        const Json& sc = j["scroll"];
        if (sc.type == Json::Object) {
            w.scroll.track = image(sc["track"].str());
            w.scroll.thumb = image(sc["thumb"].str());
            w.scroll.width = std::max(1, sc["width"].integer(skin.dp(12)));
            w.scroll.always = sc["always"].flag();
            w.scroll.follow = sc["follow"].flag();
            w.scroll.reveal = sc["reveal"].flag();
        }
        if (w.kind == Kind::Custom && w.customKind.empty()) w.customKind = j["kind"].str();
        if (w.kind == Kind::Custom) w.ops = findKind(w.customKind);
        for (auto& p : j["params"].items) w.params.push_back(p.str());
        w.first = j["first"].integer(36);
        w.count = std::clamp(j["count"].integer(61), 0, 128);
        w.velocity = std::clamp(j["velocity"].integer(0), 0, 127);
        w.glide = j["glide"].flag(true);
        static const char* keyNames[9] = {"c", "d", "e", "f", "g", "a", "b", "black", "top"};
        for (int i = 0; i < 9; ++i) w.keyImages[i] = image(j["keyImages"][keyNames[i]].str());
        if (w.kind == Kind::Custom) {   // custom kinds read their own fields; their images and fonts load here
            w.json = j;
            for (const char* f : {"image", "image2", "track", "thumb"}) image(j[f].str());
        }
        if (w.kind == Kind::List) w.json = j;   // a chooser list's "values", "first" and "set"
        return w;
    }

    // A menu style: the fields present in j over base; "hoverBand": null removes the band.
    MenuStyle menuStyle(const Json& j, MenuStyle m) {
        auto pad = [](const Json& p, int* out) {
            for (int i = 0; i < 4 && p.type == Json::Array; ++i) out[i] = std::max(0, p[i].integer(out[i]));
        };
        if (j.has("font")) m.font = font(j["font"].str());
        if (j.has("hoverFont")) m.hoverFont = font(j["hoverFont"].str());
        if (j.has("disabledFont")) m.disabledFont = font(j["disabledFont"].str());
        m.fill = colour(j["fill"].str(), m.fill);
        m.border = colour(j["border"].str(), m.border);
        m.hoverFill = colour(j["hoverFill"].str(), m.hoverFill);
        m.separator = colour(j["separator"].str(), m.separator);
        m.shadow = colour(j["shadow"].str(), m.shadow);
        for (auto& kv : j.members)
            if (kv.first == "hoverBand") m.hoverBand = kv.second.type == Json::Null ? 0 : colour(kv.second.str(), m.hoverBand);
        m.rowHeight = std::max(1, j["rowHeight"].integer(m.rowHeight));
        m.separatorHeight = std::max(1, j["separatorHeight"].integer(m.separatorHeight));
        pad(j["pad"], m.pad);
        if (j.has("check")) m.check = image(j["check"].str());
        if (j.has("arrow")) m.arrow = image(j["arrow"].str());
        if (j.has("place")) m.over = j["place"].str() == "over";
        if (j.has("keys")) m.escapeOnly = j["keys"].str() == "escape";
        m.releaseGuard = std::max(0, j["releaseGuard"].integer(m.releaseGuard));
        m.on = m.font >= 0;   // no font, no labels: native menus
        return m;
    }
};

std::shared_ptr<Skin> loadFiles(const Files& files, std::string* error) {
    auto skin = std::make_shared<Skin>();
    auto get = [&](const std::string& path, Json& out) -> bool {
        auto f = files.find(path);
        std::string e;
        if (f == files.end()) e = "missing";
        else if (parseJson(f->second, out, &e)) return true;
        if (error) *error += path + ": " + e + "\n";
        return false;
    };
    Json sj, pj;
    if (!get("skin.json", sj)) return nullptr;
    skin->name = sj["name"].str();
    skin->root = sj["root"].str("main");
    skin->density = std::clamp(sj["density"].num(1), 0.25, 8.0);
    skin->vars = Loader::vars(sj["vars"]);
    if (files.count("params.json") && !get("params.json", pj)) return nullptr;
    for (auto& p : pj.items) {
        ParamDef d;
        d.id = p["id"].str();
        d.name = p["name"].str(d.id);
        d.unit = p["unit"].str();
        d.format = p["format"].str();
        d.min = p["min"].num(0);
        d.max = p["max"].num(1);
        d.def = p["default"].num(d.min);
        d.steps = std::max(0, p["steps"].integer());
        d.log = p["taper"].str() == "log";
        d.host = p["host"].flag(true);
        for (auto& l : p["labels"].items) d.labels.push_back(l.str());
        uint32_t h = 2166136261u;
        for (unsigned char c : d.id) h = (h ^ c) * 16777619u;
        d.hostId = h & 0x7fffffff;
        skin->params.push_back(std::move(d));
    }
    Loader ld{files, *skin, sj["images"], {}, {}, {}};
    auto pad = [](const Json& j, int* out) {
        for (int i = 0; i < 4 && j.type == Json::Array; ++i) out[i] = std::max(0, j[i].integer(out[i]));
    };
    if (const Json& mj = sj["menu"]; mj.type == Json::Object) {
        MenuStyle base;   // the defaults are in design pixels
        base.rowHeight = skin->dp(base.rowHeight);
        base.separatorHeight = skin->dp(base.separatorHeight);
        for (int& p : base.pad) p = skin->dp(p);
        skin->menu = ld.menuStyle(mj, base);
        for (auto& st : mj["styles"].members) skin->menuStyles[st.first] = ld.menuStyle(st.second, skin->menu);
    }
    if (const Json& tj = sj["tooltip"]; tj.type == Json::Object) {
        TipStyle& t = skin->tooltip;
        for (int& p : t.pad) p = skin->dp(p);
        t.font = ld.font(tj["font"].str());
        t.fill = Loader::colour(tj["fill"].str(), t.fill);
        t.border = Loader::colour(tj["border"].str(), t.border);
        pad(tj["pad"], t.pad);
        t.delay = std::max(0, tj["delay"].integer(t.delay));
        t.on = t.font >= 0;
    }
    if (const Json& ej = sj["editAll"]; ej.type == Json::Object) {
        skin->editAllParam = ej["param"].str();
        skin->editAllVar = ej["var"].str();
        for (auto& v : ej["values"].items) skin->editAllValues.push_back(Loader::valueText(v));
    }
    if (const Json& lj = sj["learn"]; lj.type == Json::Object) {
        skin->learnParam = lj["param"].str();
        skin->learnOutline = Loader::colour(lj["outline"].str(), skin->learnOutline);
        for (auto& d : lj["defaults"].members) skin->learnDefaults.push_back({std::atoi(d.first.c_str()), d.second.str()});
    }
    for (auto& f : files) {
        const std::string& path = f.first;
        if (path.compare(0, 5, "data/") == 0 && path.size() > 10 && path.compare(path.size() - 5, 5, ".json") == 0 &&
            path.find('/', 5) == std::string::npos) {
            Json tj;
            if (get(path, tj)) skin->tables[path.substr(5, path.size() - 10)] = std::move(tj);
            continue;
        }
        if (path.compare(0, 6, "views/") != 0 || path.size() < 11 || path.compare(path.size() - 5, 5, ".json") != 0) continue;
        if (path.find('/', 6) != std::string::npos) continue;
        Json vj;
        if (!get(path, vj)) return nullptr;
        View v;
        v.name = path.substr(6, path.size() - 11);
        v.w = vj["size"][0].integer();
        v.h = vj["size"][1].integer();
        v.flow = vj["flow"].str() == "column";
        v.fit = vj["fit"].flag();
        v.gap = vj["gap"].integer();
        v.animate = std::max(0, vj["animate"].integer());
        v.fill = ld.fill(vj["fill"]);
        for (auto& wj : vj["widgets"].items) v.widgets.push_back(ld.widget(wj));
        for (int i = 0; i < (int)v.widgets.size(); ++i) v.order.push_back(i);
        std::stable_sort(v.order.begin(), v.order.end(), [&](int a, int b) { return v.widgets[a].layer < v.widgets[b].layer; });
        skin->views[v.name] = std::move(v);
    }
    for (auto& fj : sj["fonts"].members) {   // text entry colours per font
        int id = ld.font(fj.first);
        if (id < 0) continue;
        Font& f = skin->fonts[id];
        f.caret = Loader::colour(fj.second["caret"].str(), f.caret);
        f.selection = Loader::colour(fj.second["selection"].str(), f.selection);
    }
    if (const Json& sj2 = sj["surface"]; sj2.type == Json::Object) {   // the texture and its two ramps
        int tex = ld.image(sj2["image"].str()), nrm = ld.image(sj2["normals"].str());
        skin->surface.relief = std::clamp(sj2["relief"][0].integer(0), 0, 512);
        skin->surface.dents = std::clamp(sj2["relief"][1].integer(0), 0, 512);
        auto ramp = [](const Json& r, double lo, double hi, int* out, bool down) {
            double a = r[0].num(lo) * 255, b = r[1].num(hi) * 255;
            for (int i = 0; i < 256; ++i) {
                double t = b > a ? std::clamp((i - a) / (b - a), 0.0, 1.0) : i >= a ? 1.0 : 0.0;
                t = t * t * (3 - 2 * t);   // smoothstep
                out[i] = (int)std::lround(256 * (down ? 1 - t : t));
            }
        };
        ramp(sj2["lightness"], 0.7, 0.86, skin->surface.weight, false);
        ramp(sj2["chroma"], 0.06, 0.14, skin->surface.chroma, true);
        double k = std::clamp(sj2["strength"].num(1), 0.0, 1.0);
        for (int& w : skin->surface.weight) w = (int)std::lround(w * k);
        if (tex >= 0) skin->surface.tex = &skin->images[tex];   // images no longer move: every widget has loaded
        if (nrm >= 0) skin->surface.normals = &skin->images[nrm];
        if (skin->surface.relief > 0)   // the slope of each surfaced image's shading, within each tile
            for (Image& img : skin->images) {
                if (!img.surface || &img == skin->surface.tex || &img == skin->surface.normals || img.px.empty()) continue;
                img.slope.assign((size_t)img.w * img.h * 2, 0);
                for (int t = 0; t < std::max(img.tiles, 1); ++t) {
                    Rect tr = img.tile(t);
                    auto lum = [&](int x, int y) {   // lightness times opacity, clamped to the tile
                        uint32_t p = img.px[(size_t)std::clamp(y, tr.y, tr.y + tr.h - 1) * img.w + std::clamp(x, tr.x, tr.x + tr.w - 1)];
                        return (int)(((77 * (p >> 16 & 255) + 150 * (p >> 8 & 255) + 29 * (p & 255)) >> 8) * (p >> 24) / 255);
                    };
                    for (int y = tr.y; y < tr.y + tr.h; ++y)
                        for (int x = tr.x; x < tr.x + tr.w; ++x) {   // Sobel, a full slope 127
                            int gx = lum(x + 1, y - 1) + 2 * lum(x + 1, y) + lum(x + 1, y + 1) - lum(x - 1, y - 1) - 2 * lum(x - 1, y) - lum(x - 1, y + 1);
                            int gy = lum(x - 1, y + 1) + 2 * lum(x, y + 1) + lum(x + 1, y + 1) - lum(x - 1, y - 1) - 2 * lum(x, y - 1) - lum(x + 1, y - 1);
                            img.slope[2 * ((size_t)y * img.w + x)] = (int8_t)std::clamp(-gx / 8, -127, 127);   // lighter below: tilted up
                            img.slope[2 * ((size_t)y * img.w + x) + 1] = (int8_t)std::clamp(-gy / 8, -127, 127);
                        }
                }
            }
    }
    if (error) *error += ld.err;   // missing art is reported but not fatal
    return skin;
}

Files readDir(const std::string& dir) {
    Files files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::u8path(dir), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = utf8(fs::relative(it->path(), fs::u8path(dir), ec));
        std::ifstream in(it->path(), std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        files[rel] = ss.str();
    }
    return files;
}
} // namespace

std::shared_ptr<Skin> loadSkinDir(const std::string& dir, std::string* error) {
    auto skin = loadFiles(readDir(dir), error);
    if (skin) skin->dir = dir;
    return skin;
}

uint64_t skinDirStamp(const std::string& dir) {
    uint64_t h = 1469598103934665603ull;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::u8path(dir), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        auto t = (uint64_t)fs::last_write_time(it->path(), ec).time_since_epoch().count();
        for (char c : utf8(it->path())) h = (h ^ (unsigned char)c) * 1099511628211ull;
        h = (h ^ t) * 1099511628211ull;
    }
    return h;
}

std::shared_ptr<Skin> loadSkin() {
    std::string err;
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)   // getenv is fine here: read once, never stored
#endif
    const char* env = std::getenv("HOLLOW_SKIN_DIR");
    if (env && *env) {
        if (auto s = loadSkinDir(env, &err)) return s;
        std::fprintf(stderr, "hollow: HOLLOW_SKIN_DIR %s: %s", env, err.c_str());
    }
    Files files;
    for (size_t i = 0; i < kSkinFileCount; ++i) files[kSkinFiles[i].path].assign((const char*)kSkinFiles[i].data, kSkinFiles[i].size);
    auto skin = loadFiles(files, &err);
    if (!skin) skin = std::make_shared<Skin>();   // an empty skin still opens, as a blank window
    if (env && *env) skin->dir = env;              // keep watching the folder until it loads
    return skin;
}

const std::vector<ParamDef>& skinParams(const Skin& skin) { return skin.params; }

void applyLearnDefaults(State& state, const Skin& skin) {
    for (auto& d : skin.learnDefaults) state.assign(d.first, state.indexOf(d.second));
}

} // namespace hollow
