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
- `HOLLOW_KEYLOG=<file>` appends what the keyboard path saw: the window's focus grab and what it took, every key message and whether the editor used it, and every key a host delivered through `Editor::key`. For working out where a host's keys go, which is otherwise guesswork on someone else's machine. Unset, it costs one pointer test and writes nothing.
- `hollow-render <skin> <view> <out.png> [state file] [--scale s]` renders any view without a window, for checks and documentation; a saved state (`hollow-state 1`: params, text data, UI vars and pages) renders the view as that instance would show it, and `--scale` at one of the skin's scales.
- A button with the action `{ "scale": "menu" }` offers the window's scales, and the choice is saved with the instance: the skin's `"scales"`, each a load of the skin at that size with its art from `scales/<scale>/` (FSVR's are 0.5x to 2x, rendered in Blender), else 1x to 4x pixel repeats of the 1x canvas.
- A test can drive a `Gui` without a window (FSVR's `tools/check_gui.cpp`): skinned menus open without one, `widgetRect` finds a widget by embed path and name, `menuLabels` and `chooseMenu` read and pick the open menu, `modal` names the open modal, `tipOf` gives the tooltip a widget would show (its keyboard shortcut included), and `probes` lists every visible control.
- For a GUI drawn at a size that is not a whole multiple, set `skin.json` `density` to its pixels per unit so the runtime's own drawings follow (FSVR's skin is 1.5).
- Text is the skin's fonts: TrueType files named in `skin.json` `"fonts"`, rasterized at the window's scale, or picture fonts (glyph strips).
- `python tools/overflow.py <skin>` lists text wider than its widget: static captions, every choice a dropdown can show, the widest value a number field can show, and list cells, measured with the skin's own fonts as the runtime measures them.

## What the framework does for a processor

- **Threads.** `Processor::process` and `midi` run on the audio thread; nothing else of the processor's is called there. A processor with slower work (files, patch loads) keeps a thread of its own, as FSVR's does, and reads what the GUI asks for out of State's text data.
- **Saving.** `Processor::saving()` runs on the main thread just before the host saves the instance: put what State does not hold (FSVR: the engine's own bulk dumps) into text data there. `State::loads()` counts loads and is odd while one runs, so a processor that acts on param changes can tell a restored session from an edit.
- **Telling the host.** A processor that changes many params itself (a patch load) calls `paramsChanged()`; CLAP hosts then re-read every value on the main thread, VST2 hosts redraw on the editor's next idle.
- **The monitor.** The format layer measures every block's output peak into State (`peak`); a meter with `"source": "level_l"` or `"level_r"` shows it. A processor may publish up to 256 values for a plot with `"source": "scope"` (`setScope`), from any thread but the audio one.

## Platforms

Windows (a child HWND), macOS (an NSView) and Linux, where the editor is an X11 child window of the host's, run by a thread of its own on a Display connection of its own (`src/platform/linux.cpp`): the same code under CLAP, VST3, VST2 and the standalone, with no help from the host's run loop. Linux file dialogs are zenity's, else kdialog's; menus and tooltips are the skin's own (a skin without a `menu` style has none there). `Editor` holds the GUI's lock (`platformHold`) for the few calls it makes from the host's thread.

**The keyboard.** Who holds it depends on whether the skin declares shortcuts (`skin.json` `"keys"`). Without them the editor takes the keyboard only while text entry, a menu, a focused list or a modal wants it, and hands it straight back after, so keys stay with the host. With them the editor takes the keyboard when its window opens and on every click inside it, and keeps it. **Taking it at open is load-bearing, not a convenience:** the Win32 message hook below matches on a message's target window, which is whichever window holds focus, so holding focus from the moment the editor opens is what makes the chords work before anything has been clicked. The mechanics: `Gui::keyDown` returns false for a key it had no use for, and each backend then passes that key on to the window focus came from (`PostMessageW` on Win32, the next responder on macOS, `XSendEvent` to the host's window on X11), so a DAW keeps its own shortcuts, the transport bar included. Win32 also has to answer `WM_SYSKEYDOWN` and `WM_SYSCHAR`, since Alt chords arrive there; letting `DefWindowProc` have what cannot be forwarded is what keeps Alt+F4 and the system menu working. A forwarded press is remembered by its virtual key so that its release is forwarded too: the editor is done with a key on the press, but a host feature that pairs the two — its virtual keyboard sounding a note for as long as a key is held — reads a press with no release as a key still down, and the note then never ends. The character a forwarded press types is not forwarded: the posted press goes through the host's own message loop, whose `TranslateMessage` types it there, so sending the editor's `WM_CHAR` as well types it twice, and a host that acts on the character acts twice, Space starting the transport and stopping it again.

A focused list keeps only the named keys (`Key`, the ones that act on a selection), of which Escape and a MIDI map's Delete do anything; a key that types goes on to the skin's chords and then the host, as though no list had the keyboard, so selecting a row in a browser does not take Space or a host's computer keyboard away.

**The hosts that take keys off the window, and the hook that gets in front of them.** A host may handle keys in its own message loop, between `GetMessage` and `DispatchMessage`, which is where its accelerators live. The editor's window then never sees them however well it holds focus, and no amount of focus handling is a fix; a plug-in that seems to need a click in its window first is usually meeting this. So on Windows, a skin with bindings installs a `WH_GETMESSAGE` hook on the host's UI thread, which sees each message before the loop acts on it: a chord the editor uses is taken and the message blanked, so the host never sees it, and everything else goes on through `CallNextHookEx` untouched. JUCE answers the same problem the same way (`juce_WindowsHooks_windows.cpp`), and on Windows alone — macOS routes keys through the responder chain and X11 through input focus, neither of which a host sits in front of.

It is scoped to remove only keystrokes the editor acted on: one hook per thread, refcounted across the editors on it, installed only for a skin that has bindings, offered only for a message addressed to one of our own windows, and taken only for a key the editor used. While text entry, a menu, a list or a modal has the keyboard that means the keys those use themselves, Escape and Enter among them — a host that closes the plug-in's window on Escape must not see the Escape that just cancelled an edit, and the one after it, with no edit to cancel, is the editor's for nothing and goes on to close the window as it should. The single exception is the key text entry is typing with, whose message has to go on and become a `WM_CHAR` (`Gui::charPending`, below); its press does nothing twice, so the window's own procedure offering it again costs nothing. Its one real cost is that the editor's action runs re-entrantly inside the host's message loop, as JUCE's does. The consequence to be aware of is the intended one: while the editor holds focus, a chord it binds no longer reaches the host, even one the host has bound to an action of its own.

**Keys a host delivers itself.** Some hosts also offer keys through the plug-in API instead of the window, sometimes behind a setting of their own. On Windows the hook makes this route redundant, and it is kept because it costs little and is the only cooperative one: it needs no message-loop interference and is what a platform without a hook would use. `Editor::key` is that entry point: it takes the GUI's lock like every other call from the host's thread, and returns whether the editor used the key, which such a host reads as "mine". The VST2 layer answers `effEditKeyDown` and `effEditKeyUp` through it and makes `effKeysRequired` explicit (0, inverted by convention, means the editor wants keys). VST3 has such a call too, but clap-wrapper stubs it out, and CLAP has no key event at all and never will, so for those two the hook is the whole of the answer.

**A host may deliver one press twice**, through its API and to the window both, which has been seen with Shift+digit. `Gui::runKey` therefore remembers the chord it last acted on and which route brought it, and a matching chord arriving by the other route within 50 ms is taken as ours but does nothing — otherwise a `cycle` binding would step twice and look inert. That memory is spent once used, so the next press acts whichever route it comes by, and two presses along one route are two presses.

Two Win32 details are what stop a used chord **flashing the title bar**, which is what menu mode does when it finds no mnemonic for an Alt chord. First, the character message belongs to the key message it came from: the backend remembers whether `keyDown` took the key down and swallows the `WM_CHAR` / `WM_SYSCHAR` that follows a key it took, rather than forwarding it (that also stops Shift+1 typing its "!" into the host). The key text entry is typing with is the exception, since it is the editor's for a different reason: the press is the entry's and the host never sees it, but the character is the whole of what the entry wants, so `keyDown` reports that one as still to come (`Gui::charPending`) and the backend lets it through to `keyChar` rather than swallowing it with its key. A named key the entry uses itself — Enter, Escape, an arrow — spends the character its key would have typed, and so does an Alt or Ctrl chord, which types none; AltGr, which arrives as both of them held, types one and leaves it pending. Second, a modifier on its own is nobody's keystroke, so with bindings in the skin `VK_MENU`, `VK_SHIFT`, `VK_CONTROL` and `VK_F10` stop at the backend on both press and release and never reach `DefWindowProc`, which is what would start menu mode. The cost is that bare Alt no longer opens the host's menu bar while the editor has the keyboard.

## Publishing a skin without its surface

`hollow-bake <skin>` burns a skin's `surface` into its images, in place: each image the surface reaches is rewritten as the runtime draws it at its first place in the window, so that place renders the same pixels with the surface off, and the caller then drops `surface` and its textures from `skin.json`. FSVR's skin ships that way.

## Custom widgets

The built-in `custom` kinds (pad, morph pad, piano, envelope, key scaling curve, waveform, spectrum, matrix wires and operator boxes) live in `framework/src/core/kinds.cpp`, each behind a small table of hooks (draw, mouse down, drag, up, double-click, hover, right-click, tick, wheel) plus one line in `findKind()`. A product that needs a new display adds a kind there without touching the rest of the runtime. Kinds read tables from the skin's `data/` folder and keep shapes in the instance's text data.
