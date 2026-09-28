// hollow-bake <skin_dir>: burns the skin's surface into its artwork, in place. Every image the surface
// reaches is rewritten as the runtime draws it at its first place in the window (views reached from the
// root through embeds and the pages their stacks switch to, in that order), so that place renders the
// same pixels with the surface off. An image nothing draws is baked at the window's corner. Prints each
// image it wrote and anything the bake cannot carry (a colour fill the surface would change); the caller
// then drops "surface" and its textures from skin.json.
#include "core/core.h"
#include "embedded_skin.h"
#include <cstdio>
#include <deque>
#include <set>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIW_WINDOWS_UTF8
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace hollow {
const EmbeddedFile kSkinFiles[] = {{"", nullptr, 0}};
const size_t kSkinFileCount = 0;
}

using namespace hollow;

static bool reaches(const Surface& sf, uint32_t argb) {   // the surface's weight for a colour, as surfaced() takes it
    uint32_t r = argb >> 16 & 255, g = argb >> 8 & 255, b = argb & 255;
    uint32_t hi = std::max({r, g, b}), lo = std::min({r, g, b}), l = (77 * r + 150 * g + 29 * b) >> 8;
    return (argb >> 24) && ((uint32_t)(sf.weight[l] * sf.chroma[hi - lo]) >> 8) > 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: hollow-bake <skin_dir>   (rewrites its images in place)\n");
        return 2;
    }
    std::string err;
    auto skin = loadSkinDir(argv[1], &err);
    if (!skin) {
        std::fprintf(stderr, "%s", err.c_str());
        return 1;
    }
    const Surface& sf = skin->surface;
    if (!sf.tex) {
        std::printf("no surface: nothing to bake\n");
        return 0;
    }
    // Every page a stack can show: goto targets by stack name.
    std::map<std::string, std::vector<std::string>> pages;
    for (auto& [name, v] : skin->views)
        for (auto& w : v.widgets) {
            auto add = [&](const Action& a) {
                if (a.type == Action::Goto) pages[a.stack].push_back(a.target);
            };
            add(w.action);
            for (auto& it : w.items) add(it.action);
        }
    // Breadth first from the root: each view's first origin, and whether the surface reaches into it.
    struct Place { int x, y; bool surf; };
    std::map<std::string, Place> origin;
    std::deque<std::string> queue{skin->root};
    origin[skin->root] = {0, 0, true};
    std::map<int, Place> first;   // image index -> where it is first drawn with the surface on
    std::set<int> plain;          // images also drawn with the surface off somewhere
    int fills = 0;
    while (!queue.empty()) {
        const std::string vn = queue.front();
        queue.pop_front();
        const View* v = skin->view(vn);
        if (!v) continue;
        const Place o = origin[vn];
        auto use = [&](int img, int x, int y, bool on) {
            if (img < 0) return;
            if (!on || !skin->images[(size_t)img].surface) { plain.insert(img); return; }
            if (!first.count(img)) first[img] = {std::max(x, 0), std::max(y, 0), true};
        };
        use(v->fill.image, o.x, o.y, o.surf);
        if (o.surf && v->fill.hasColour && reaches(sf, v->fill.colour)) ++fills;
        for (auto& w : v->widgets) {
            const bool on = o.surf && w.surface;
            const int x = o.x + w.rect.x, y = o.y + w.rect.y;
            use(w.fill.image, x, y, on);
            use(w.image, x, y, on);
            for (auto& c : w.columns) use(c.image, x, y, on);
            if (w.scroll.width > 0) {
                use(w.scroll.track, x + w.rect.w - w.scroll.width, y, on);
                use(w.scroll.thumb, x + w.rect.w - w.scroll.width, y, on);
            }
            if (on && w.fill.hasColour && reaches(sf, w.fill.colour)) ++fills;
            if (w.kind != Kind::Embed) continue;
            std::vector<std::string> shown{w.view};
            auto p = pages.find(w.name);
            if (p != pages.end()) shown.insert(shown.end(), p->second.begin(), p->second.end());
            for (auto& s : shown)
                if (!origin.count(s)) {
                    origin[s] = {x, y, on};
                    queue.push_back(s);
                }
        }
    }
    int written = 0;
    for (size_t i = 0; i < skin->images.size(); ++i) {
        Image& img = skin->images[i];
        if (!img.surface || &img == sf.tex || &img == sf.normals || img.px.empty()) continue;
        auto f = first.find((int)i);
        const Place at = f != first.end() ? f->second : Place{0, 0, true};
        std::vector<uint32_t> before = img.px;
        bakeSurface(img, at.x, at.y, sf);
        if (img.px == before) continue;   // nothing light and grey in it
        if (plain.count((int)i))
            std::printf("warning: %s is also drawn without the surface; baked anyway\n", skin->imageNames[i].c_str());
        std::vector<uint8_t> rgba(img.px.size() * 4);
        for (size_t k = 0; k < img.px.size(); ++k) {
            const uint32_t p = img.px[k];
            rgba[k * 4] = (uint8_t)(p >> 16);
            rgba[k * 4 + 1] = (uint8_t)(p >> 8);
            rgba[k * 4 + 2] = (uint8_t)p;
            rgba[k * 4 + 3] = (uint8_t)(p >> 24);
        }
        const std::string path = std::string(argv[1]) + "/images/" + skin->imageNames[i] + ".png";
        if (!stbi_write_png(path.c_str(), img.w, img.h, 4, rgba.data(), img.w * 4)) {
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
            return 1;
        }
        std::printf("baked %s at %d,%d%s\n", skin->imageNames[i].c_str(), at.x, at.y, f == first.end() ? " (not drawn)" : "");
        ++written;
    }
    if (fills) std::printf("warning: %d colour fills take the surface and stay plain once it is off\n", fills);
    std::printf("%d images baked\n", written);
    return 0;
}
