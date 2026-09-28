// FSVR: the Yamaha FS1R editor, a silent shell around FSVR's own skin (../FSVR/plugin/skin) for trying
// the skin in a standalone or host. FSVR's plug-in (../FSVR/plugin) is the same skin with the engine.
#include <hollow/hollow.h>

namespace hollow {

const Info& pluginInfo() {
    static const Info info{"studio.musica.fsvr-hollow", "FSVR", "musica.studio", "", "0.1.0", "Yamaha FS1R editor GUI shell",
                           /*instrument*/ true, /*inputs*/ 0, /*outputs*/ 2, /*midiIn*/ true, /*midiOut*/ false};
    return info;
}

// The base Processor does nothing, and outputs arrive zeroed, so the plug-in stays silent.
std::unique_ptr<Processor> createProcessor(State&) { return std::make_unique<Processor>(); }

} // namespace hollow
