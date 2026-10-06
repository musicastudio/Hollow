// hollow-render <skin_dir> <view> <out.png> [state file] [--scale s]: renders one view of a skin folder, as
// the runtime draws it with every value at its default (or the state's), at 1x or one of the skin's scales.
#include "core/core.h"
#include "embedded_skin.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <iterator>

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
// The tool reads a folder; it has no embedded skin.
const EmbeddedFile kSkinFiles[] = {{"", nullptr, 0}};
const size_t kSkinFileCount = 0;
}

int main(int argc, char** argv) {
    double scale = 0;
    if (argc >= 6 && std::string(argv[argc - 2]) == "--scale") {
        scale = std::atof(argv[argc - 1]);
        argc -= 2;
    }
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: hollow-render <skin_dir> <view> <out.png> [state file] [--scale s]\n");
        return 2;
    }
    std::string err;
    auto skin = hollow::loadSkinDir(argv[1], &err);
    if (!err.empty()) std::fprintf(stderr, "%s", err.c_str());
    if (!skin) return 1;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    std::string state;
    if (argc == 5) {   // a saved state (params, text data) to render with
        std::ifstream f(argv[4], std::ios::binary);
        state.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (!hollow::renderView(*skin, argv[2], rgba, w, h, state, scale)) {
        std::fprintf(stderr, "no view '%s' in %s\n", argv[2], argv[1]);
        return 1;
    }
    if (!stbi_write_png(argv[3], w, h, 4, rgba.data(), w * 4)) {
        std::fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    return 0;
}
