// The keyboard codes VST2 and VST3 share, for the format layers that take keys from a host rather than
// from the window. VST3 kept VST2's numbering, so one table serves both: VstVirtualKey and VstModifierKey
// in VST2, VirtualKeyCodes and KeyModifier in VST3's keycodes.h. Declared here rather than included from
// either SDK, since the CLAP layer must not depend on VST3's headers.
#pragma once
#include <hollow/hollow.h>

namespace hollow {

enum {   // KeyModifier / VstModifierKey
    kVstModShift = 1 << 0,
    kVstModAlternate = 1 << 1,
    kVstModCommand = 1 << 2,
    kVstModControl = 1 << 3,
};

// A virtual key both formats name, for keys that produce no character. A letter or a digit is in neither
// enum and arrives as the character the key typed instead, which `Editor::key` turns back into a key.
inline Key vstVirtualKey(int vkey) {
    switch (vkey) {
    case 1: return KeyBackspace;   // KEY_BACK
    case 2: return KeyTab;
    case 4: return KeyEnter;       // KEY_RETURN
    case 6: return KeyEscape;
    case 9: return KeyEnd;
    case 10: return KeyHome;
    case 11: return KeyLeft;
    case 12: return KeyUp;
    case 13: return KeyRight;
    case 14: return KeyDown;
    case 19: return KeyEnter;      // KEY_ENTER, the numeric pad's
    case 22: return KeyDelete;
    default: return KeyNone;
    }
}

}   // namespace hollow
