// The exported clap_entry. clap-wrapper compiles this file into every module it builds (the .clap,
// the VST3, the AUv2, the standalone), each linked to the plug-in in clap_plugin.cpp.
#include <clap/clap.h>

namespace hollow {
bool clapInit(const char* path);
void clapDeinit();
const void* clapFactory(const char* id);
} // namespace hollow

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, hollow::clapInit, hollow::clapDeinit, hollow::clapFactory};
