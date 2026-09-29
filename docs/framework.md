# Using Hollow for a plug-in

FSVR's `plugin/` (in the FSVR repository) is a full product on this framework, with a processor, and Hollow's `plugins/fsvr` the smallest one: FSVR's skin in a silent shell. A Hollow product is three things: a `plugin.cpp` that describes it (and makes sound, if it does), a skin folder that is its GUI, and one `hollow_add_plugin()` call. The framework supplies the rest: the formats, the editor window, parameter plumbing, state, GUI scaling and the embedded skin.

## 1. The product

```cpp
#include <hollow/hollow.h>

namespace hollow {

const Info& pluginInfo() {
    static const Info info{"com.example.my-synth", "My Synth", "Example", "https://example.com", "1.0.0",
                           "A small synth", /*instrument*/ true, /*inputs*/ 0, /*outputs*/ 2,
                           /*midiIn*/ true, /*midiOut*/ false};
    return info;
}

struct Synth : Processor {
    explicit Synth(State& s) : state(s), volume(s.indexOf("master.volume")) {}
    void prepare(double rate, int maxFrames) override { /* allocate */ }
    void midi(int offset, const uint8_t* bytes, int size) override { /* notes */ }
    void process(const float* const* in, float* const* out, int frames) override {
        double v = state.get(volume);   // plain value, lock-free
        /* render into out[0], out[1] */
    }
    State& state;
    int volume;
};

std::unique_ptr<Processor> createProcessor(State& s) { return std::make_unique<Synth>(s); }

} // namespace hollow
```

Parameters are declared in the skin's `params.json`, not in code: the processor finds them by id (`State::indexOf`) once and reads plain values with `State::get` on the audio thread. The host sees each by a stable id hashed from its string id; VST2 uses array order, so append new params at the end. `sendMidi` (set by the format layer) sends MIDI out when `Info::midiOut` is true.

## 2. The skin

Start from any existing skin or from nothing: `skin.json` (name, root view), `params.json`, `views/main.json`, and images under `images/<group>/`. Open it in the editor (`python editor/serve.py path/to/skin`) and build the GUI there; [skin-format.md](skin-format.md) is the reference and [naming.md](naming.md) the naming rules. A widget binds to a parameter with `"param": "<id>"`; pages are views shown in an `embed` that navigation buttons switch with `goto` actions.

## 3. The build

In Hollow itself, add the product's folder to the top-level `CMakeLists.txt` and give it a `CMakeLists.txt` of its own:

```cmake
hollow_add_plugin(my_synth
  NAME "My Synth"
  SKIN "${CMAKE_CURRENT_SOURCE_DIR}/skin"
  SOURCES plugin.cpp
  BUNDLE_ID com.example.my-synth       # must equal Info::id
  VERSION 1.0.0
  VENDOR Example
  AU_MANUFACTURER Exmp                 # 4 characters, at least one upper case
  AU_SUBTYPE MySy                      # 4 characters
  WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico"   # optional: the standalone's .exe, window and shortcuts
  MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.icns")   # optional: the standalone .app's
```

From another repository, vendor Hollow (a git submodule at `external/hollow`, for example) and write a top-level `CMakeLists.txt` like this:

```cmake
cmake_minimum_required(VERSION 3.22)
include(external/hollow/framework/cmake/HollowDefaults.cmake)   # before project(): C++17, static runtime, macOS universal
project(MySynth VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()
add_subdirectory(external/hollow/framework hollow)
hollow_add_plugin(my_synth NAME "My Synth" SKIN "${CMAKE_CURRENT_SOURCE_DIR}/skin"
                  SOURCES src/plugin.cpp BUNDLE_ID com.example.my-synth)
```

Configure and build as in the README; the formats land in `build/<dir>/out/`. `-DHOLLOW_FORMATS="CLAP;VST3"` limits what gets built. For a DXi, keep the same `BUNDLE_ID` forever (its COM class id derives from it) or pass `DXI_CLSID`.

## 4. The loop

- `HOLLOW_SKIN_DIR=<skin folder>` makes any build read the skin from disk and reload it on save, so the editor and a running standalone or host work together.
- `hollow-render <skin> <view> <out.png> [state file]` renders any view without a window, for checks and documentation; a saved state (`hollow-state 1`: params, text data, UI vars and pages) renders the view as that instance would show it.
- Right-click an empty part of the editor window for 1x to 4x scaling; the choice is saved with the instance.
- For a GUI drawn at a size that is not a whole multiple, set `skin.json` `density` to its pixels per unit so the runtime's own drawings follow (FSVR's skin is 1.5).
- `python tools/overflow.py <skin>` lists text wider than its widget: static captions, every choice a dropdown can show, the widest value a number field can show, and list cells, measured with the skin's own fonts as the runtime measures them.

## What the framework does for a processor

- **Threads.** `Processor::process` and `midi` run on the audio thread; nothing else of the processor's is called there. A processor with slower work (files, patch loads) keeps a thread of its own, as FSVR's does, and reads what the GUI asks for out of State's text data.
- **Saving.** `Processor::saving()` runs on the main thread just before the host saves the instance: put what State does not hold (FSVR: the engine's own bulk dumps) into text data there. `State::loads()` counts loads and is odd while one runs, so a processor that acts on param changes can tell a restored session from an edit.
- **Telling the host.** A processor that changes many params itself (a patch load) calls `paramsChanged()`; CLAP hosts then re-read every value on the main thread, VST2 hosts redraw on the editor's next idle.
- **The monitor.** The format layer measures every block's output peak into State (`peak`); a meter with `"source": "level_l"` or `"level_r"` shows it. A processor may publish up to 256 values for a plot with `"source": "scope"` (`setScope`), from any thread but the audio one.

## Platforms

Windows (a child HWND), macOS (an NSView) and Linux, where the editor is an X11 child window of the host's, run by a thread of its own on a Display connection of its own (`src/platform/linux.cpp`): the same code under CLAP, VST3, VST2 and the standalone, with no help from the host's run loop. Linux file dialogs are zenity's, else kdialog's; menus and tooltips are the skin's own (a skin without a `menu` style has none there). `Editor` holds the GUI's lock (`platformHold`) for the few calls it makes from the host's thread.

## Publishing a skin without its surface

`hollow-bake <skin>` burns a skin's `surface` into its images, in place: each image the surface reaches is rewritten as the runtime draws it at its first place in the window, so that place renders the same pixels with the surface off, and the caller then drops `surface` and its textures from `skin.json`. FSVR's skin ships that way.

## Custom widgets

The built-in `custom` kinds (pad, morph pad, piano, envelope, key scaling curve, waveform, spectrum, matrix wires and operator boxes) live in `framework/src/core/kinds.cpp`, each behind a small table of hooks (draw, mouse down, drag, up, double-click, hover) plus one line in `findKind()`. A product that needs a new display adds a kind there without touching the rest of the runtime. Kinds read tables from the skin's `data/` folder and keep shapes in the instance's text data.
