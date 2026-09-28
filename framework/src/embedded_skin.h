// The skin folder compiled into the plug-in. framework/cmake/HollowEmbed.cmake generates the
// definitions (one array per file) for each product; the core reads them in loadSkin().
#pragma once
#include <cstddef>

namespace hollow {
struct EmbeddedFile {
    const char* path;            // relative to the skin folder, '/' separated: "views/main.json"
    const unsigned char* data;
    size_t size;
};
extern const EmbeddedFile kSkinFiles[];
extern const size_t kSkinFileCount;
} // namespace hollow
