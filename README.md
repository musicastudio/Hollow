# Hollow

Hollow is a small, portable framework for audio plug-ins whose GUI is data: a skin folder of JSON layouts and PNG artwork, drawn by a software renderer that is identical on every host and platform. One CMake call builds a product in every format, and a browser-based editor lays out its GUI.

Its product is **FSVR**, the GUI of the [FSVR](../FSVR) plug-in, a software Yamaha FS1R. The skin lives in the FSVR repository beside this one, in `plugin/skin`, and everything here points at it: the editor opens it by default, the launcher runs the FSVR standalone on it, and `plugins/fsvr` embeds it in a silent shell for trying the skin in a host. FSVR's own plug-in is the same skin with the engine behind it; its `docs/editor.md` records what the skin does and why.

## Quick start

Everything runs from the root folder, with the FSVR repository checked out beside it (`../FSVR`). On Windows double-click `hollow.cmd`: a menu of every step, in order, with the next one marked. On macOS run `./hollow.command` (or `python3 hollow.py`). The other launchers each do one step:

| Launcher | What it does |
|---|---|
| `hollow.cmd` | The menu: open the editor, build, run, open the plug-ins folder |
| `editor.cmd` | Opens the skin editor in your browser on FSVR's skin. The skin button at the top left opens any other folder |
| `build-standalone.cmd` | Builds the FSVR app into `bin\Standalone` |
| `build-all.cmd` | Builds every format, 64-bit and 32-bit, into `bin\<format>` |
| `run-hollow-fsvr.cmd` | Starts the FSVR app, reading FSVR's skin folder live |

1. **Build the standalone** (menu 2 or `build-standalone.cmd`). The first build downloads the plug-in SDKs and takes several minutes; later builds take seconds.
2. **Run FSVR** (menu 3 or `run-hollow-fsvr.cmd`).
3. **Edit** in the editor (menu 1 or `editor.cmd`) and press Save. The running app redraws within a second, since the launcher points it at the skin folder instead of its built-in copy. Saves go straight into `../FSVR/plugin/skin`, where FSVR's build embeds them.
4. **Build everything** (menu 4 or `build-all.cmd`) to embed the skin in every format, then copy what you need from `bin/` into your host's plug-in folders.

Needs Python 3, CMake 3.22+ (Visual Studio's own copy is found too), Visual Studio 2022 with "Desktop development with C++" on Windows or the Xcode command line tools on macOS, and network access for the first build. From a terminal the same steps are `python hollow.py editor [skin folder]`, `build standalone`, `build all`, `run [--embedded]` and `bin`.

## Formats

| | Windows | macOS | Linux |
|---|---|---|---|
| CLAP | x64 | universal | x64 |
| VST3 | x64 | universal | x64 |
| VST2 | x64 and 32-bit | universal `.vst` | x64 `.so` |
| DXi | 32-bit (inside the 32-bit VST2 DLL) | | |
| AU | | universal (AUv2) | |
| Standalone | x64 | universal `.app` | x64 |

On Linux the editor is an X11 child window run by a thread of its own (`framework/src/platform/linux.cpp`), so it works the same under every format without the host's run loop; it needs `libx11-dev`, and the standalone RtAudio's and RtMidi's ALSA, JACK or PulseAudio headers.

CLAP is the native format. VST3, AU and the standalones come from [clap-wrapper](https://github.com/free-audio/clap-wrapper) (MIT), which CMake fetches with the VST3 SDK, AudioUnitSDK and RtAudio. VST2 is our own adapter over a minimal ABI header, and the DXi is a DirectShow filter speaking Cakewalk's MFX interfaces that drives the VST2 adapter in the same DLL (Microsoft's DirectShow BaseClasses, MIT, are vendored).

## Building by hand

The launchers run these commands. Needs CMake 3.22+, a C++17 compiler (Visual Studio 2022 on Windows, Xcode command line tools on macOS) and network access on the first configure.

```bash
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build/x64 --config Release
```

```bash
cmake -S . -B build/x86 -G "Visual Studio 17 2022" -A Win32
```

```bash
cmake --build build/x86 --config Release
```

On macOS: `cmake -S . -B build/mac -G Xcode` (or Ninja), then `cmake --build build/mac --config Release`. Everything lands in `build/<dir>/out/`, which the launchers copy into `bin/<format>/`. `HOLLOW_FORMATS` (default `CLAP;VST3;AUV2;STANDALONE;VST2;DXI`) switches formats off, and `FSVR_DIR` names the FSVR repository when it is not at `../FSVR`; without it the plug-ins build with an empty GUI. The DXi registers itself with `regsvr32 "FSVR.dll"` from an elevated prompt.

## Layout

```
Hollow/
  hollow.py, *.cmd            the launchers (hollow.command on macOS)
  bin/                        every build, sorted by format (made by the launchers, not tracked)
  framework/                  the portable part; FSVR carries a copy as its hollow/
    include/hollow/hollow.h   the whole public API
    src/core/                 skin loader, renderer, widgets, State, Editor (sources.cmake lists them)
    src/platform/             win32.cpp (child HWND), mac.mm (NSView), linux.cpp (X11 window on its own thread)
    src/formats/              clap/, vst2/, dxi/
    cmake/                    HollowPlugin.cmake (hollow_add_plugin), HollowEmbed.cmake (skin -> C++)
    tools/render.cpp          hollow-render: any view to PNG, no window
    tools/bake.cpp            hollow-bake: a skin's surface burnt into its artwork
    third_party/              stb_image, VST2 ABI header, DirectShow BaseClasses
  plugins/fsvr/               FSVR's skin (../FSVR/plugin/skin) in a silent shell
  editor/                     the web skin editor (serve.py + index.html)
  tools/overflow.py           lists text wider than its widget in a skin (FSVR's by default)
  docs/                       skin-format.md, naming.md, framework.md
```

## The skin and the editor

[docs/skin-format.md](docs/skin-format.md) is the whole format: views of typed widgets (`plate`, `button`, `dropdown`, `dial`, `number`, `textbox`, `meter`, `plot`, `veil`, `list`, `tree`, `embed`, `custom`), tile strips and nine-slice images, glyph-strip fonts, parameters, page stacks with `goto` actions and `{var}` templates, and integer GUI scaling from 1x to 4x with sharp pixels.

The editor runs in any browser against a local folder (`editor.cmd`, or `python editor/serve.py [skin folder] --open`), on FSVR's skin unless given another, and its skin button opens any other folder; a folder without a skin starts a new one. It moves and resizes widgets, adds widgets, views and pages, edits parameters, has a Play mode that behaves like the runtime (pages, buttons, dials, menus, conditions, scrolling), imports and exports images, and exports a tile strip (knob frames, button states) or a font's glyphs as separate PNGs and reassembles them after editing. See [editor/README.md](editor/README.md).

Live editing: set `HOLLOW_SKIN_DIR` to the skin folder and start the standalone or a host; the plug-in reads the skin from disk instead of its embedded copy and reloads within a second of each save.

`python tools/overflow.py [skin folder]` measures every caption, dropdown choice, number field and list cell against its widget with the skin's own fonts, the way the runtime measures them, and prints each one that does not fit.

## Making another plug-in

Copy `plugins/fsvr` to `plugins/<name>`, change `plugin.cpp` (`pluginInfo()` and the `Processor`) and the one `hollow_add_plugin()` call, point `SKIN` at the new product's skin folder and grow it in the editor, and add the folder to the top-level `CMakeLists.txt`. [docs/framework.md](docs/framework.md) walks through it, including using Hollow from another repository as FSVR does.

## Status

Verified on Windows: every format builds for x64 and 32-bit; clap-validator passes with 0 failures, the Steinberg VST3 validator passes 47 of 47, scratch hosts load and drive the VST2 (both bitnesses) and the DXi filter; scripted tests cover the editor window's input, text entry, menus, scrolling, conditions, MIDI from the GUI and from the host, MIDI learn, automation, scaling, live reload and state. Not yet built on macOS: the CMake and code for the Mac bundles, the AU and the Cocoa view are written but have not been compiled (`.github/workflows/build.yml` builds them on a macOS runner once the repository is on GitHub).
