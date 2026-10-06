// Windows: a child window that presents the GUI canvas with StretchDIBits and feeds it input.
#include "../core/core.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <climits>
#include <map>
#include <mutex>
#include <vector>
#include <cwchar>

namespace hollow {

struct PlatformWindow {
    HWND hwnd = nullptr, tip = nullptr;
    HWND lastFocus = nullptr;   // had the keyboard before text entry or a menu took it
    Gui* gui = nullptr;
    bool tracking = false;
    wchar_t high = 0;           // WM_CHAR: the first half of a surrogate pair
    bool ateKey = false;        // the last key down was ours, so its WM_CHAR / WM_SYSCHAR is ours too
    HWND root = nullptr;        // the standalone's window, while its close asks the GUI first
    WNDPROC rootProc = nullptr; // and its own procedure
    bool closing = false;       // platformCloseApp: it closes without asking
};

static int classUsers = 0;
static wchar_t className[64];

static HMODULE thisModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&thisModule, &m);
    return m;
}

static std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// One tool covers the whole window.
static void tool(PlatformWindow* w, UINT msg, const std::string& text) {
    std::wstring t = wide(text) + L'\0';
    TOOLINFOW ti = {};
    ti.cbSize = TTTOOLINFOW_V2_SIZE;   // also accepted by comctl32 5 (plug-ins have no manifest)
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = w->hwnd;
    ti.uId = (UINT_PTR)w->hwnd;
    ti.lpszText = &t[0];
    SendMessageW(w->tip, msg, 0, (LPARAM)&ti);
}

// A key the editor had no use for goes on to the window the keyboard came from, so the host keeps its own
// shortcuts while the editor holds focus. False when there is nowhere to send it, and the caller then lets
// DefWindowProc have it, which is what keeps Alt+F4 and the system menu working.
static bool forwardKey(PlatformWindow* w, UINT msg, WPARAM wp, LPARAM lp) {
    HWND back = w->lastFocus && IsWindow(w->lastFocus) ? w->lastFocus : GetParent(w->hwnd);
    if (!back || back == w->hwnd) return false;
    PostMessageW(back, msg, wp, lp);
    return true;
}

// A WM_KEYDOWN or WM_SYSKEYDOWN offered to the editor, from the window's own procedure or from the keyboard
// hook below. True when the editor used it. Modifier keys are not offered: alone they are nobody's keystroke.
static bool offerKey(Gui& g, WPARAM wp) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
    const unsigned ch = (wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9') ? (unsigned)wp : 0;   // VK codes are ASCII there
    Key k = KeyNone;
    switch (wp) {
    case VK_LEFT: k = KeyLeft; break;
    case VK_RIGHT: k = KeyRight; break;
    case VK_UP: k = KeyUp; break;
    case VK_DOWN: k = KeyDown; break;
    case VK_HOME: k = KeyHome; break;
    case VK_END: k = KeyEnd; break;
    case VK_BACK: k = KeyBackspace; break;
    case VK_DELETE: k = KeyDelete; break;
    case VK_RETURN: k = KeyEnter; break;
    case VK_ESCAPE: k = KeyEscape; break;
    case VK_TAB: k = KeyTab; break;
    case 'A': k = ctrl ? KeySelectAll : KeyNone; break;
    }
    return g.keyDown(k, GetKeyState(VK_SHIFT) < 0, ctrl, alt, ch);
}

static bool modifierKey(WPARAM wp) {
    return wp == VK_MENU || wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_F10;
}

// ---- the keyboard hook ---------------------------------------------------------------------------
//
// A host may take keys off the plug-in's window by handling them in its own message loop, between
// GetMessage and DispatchMessage, which is where a host puts its accelerators: the editor's window then
// never sees them however well it holds focus, and this is the only way in. A WH_GETMESSAGE hook on the
// host's own UI thread sees each message before its loop acts on it; a chord the editor uses is taken and
// the message blanked so the host never sees it, and everything else is passed on untouched. JUCE answers
// the same problem the same way (juce_WindowsHooks_windows.cpp), and on Windows only: macOS routes keys
// through the responder chain and X11 through input focus, neither of which a host sits in front of.
//
// What is deliberately NOT taken: anything while text entry, a menu, a list or a modal has the keyboard,
// since those need the message to go on and become a WM_CHAR, and any key no binding matches. So the hook
// removes only keystrokes the editor has actually acted on.
struct KeyHook {
    HHOOK hook = nullptr;
    int refs = 0;
};
static std::mutex hooksLock;                      // the maps below: editors may live on several host threads
static std::map<DWORD, KeyHook> hooks;            // one hook per thread that has an editor on it
static std::vector<PlatformWindow*> hookWindows;  // the editor windows a hook may offer a key to

static PlatformWindow* windowForMessage(HWND hwnd) {
    std::lock_guard<std::mutex> g(hooksLock);
    for (PlatformWindow* w : hookWindows)
        if (w->hwnd == hwnd) return w;
    return nullptr;
}

static LRESULT CALLBACK keyHookProc(int code, WPARAM wp, LPARAM lp) {
    DWORD thread = GetCurrentThreadId();
    HHOOK self = nullptr;
    {
        std::lock_guard<std::mutex> g(hooksLock);
        auto it = hooks.find(thread);
        if (it != hooks.end()) self = it->second.hook;
    }
    MSG& msg = *(MSG*)lp;
    // PM_REMOVE: the message is being taken off the queue, so this is its one pass through the loop.
    if (code == HC_ACTION && wp == PM_REMOVE && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) &&
        !modifierKey(msg.wParam)) {
        if (PlatformWindow* w = windowForMessage(msg.hwnd)) {
            Gui& g = *w->gui;
            if (!g.wantsKeys() && !g.skin().keys.empty() && offerKey(g, msg.wParam)) {
                keyLog("hook took msg=%04x wp=%02x hwnd=%p", (unsigned)msg.message, (unsigned)msg.wParam, (void*)msg.hwnd);
                msg = {};                 // the host's loop sees nothing, so no accelerator and no WM_CHAR
                msg.message = WM_USER;
                return 0;
            }
        }
    }
    return CallNextHookEx(self, code, wp, lp);
}

// Installed while an editor with shortcuts is open on this thread, and removed with the last of them, so a
// product without bindings never hooks anything.
static void addKeyHook(PlatformWindow* w) {
    const DWORD thread = GetCurrentThreadId();
    std::lock_guard<std::mutex> g(hooksLock);
    hookWindows.push_back(w);
    KeyHook& h = hooks[thread];
    if (h.refs++ == 0) h.hook = SetWindowsHookExW(WH_GETMESSAGE, keyHookProc, thisModule(), thread);
    keyLog("hook installed thread=%lu hwnd=%p refs=%d ok=%d", thread, (void*)w->hwnd, h.refs, h.hook != nullptr);
}

static void removeKeyHook(PlatformWindow* w) {
    const DWORD thread = GetCurrentThreadId();
    std::lock_guard<std::mutex> g(hooksLock);
    hookWindows.erase(std::remove(hookWindows.begin(), hookWindows.end(), w), hookWindows.end());
    auto it = hooks.find(thread);
    if (it == hooks.end()) return;
    if (--it->second.refs > 0) return;
    if (it->second.hook) UnhookWindowsHookEx(it->second.hook);
    hooks.erase(it);
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = (PlatformWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    Gui& g = *w->gui;
    int s = g.scale();
    int x = floorDiv(GET_X_LPARAM(lp), s), y = floorDiv(GET_Y_LPARAM(lp), s);
    bool shift = (wp & MK_SHIFT) != 0;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const std::vector<uint32_t>& px = g.pixels();
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = g.width();
        bi.bmiHeader.biHeight = -g.height();   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);   // pixel replication, no smoothing
        StretchDIBits(dc, 0, 0, g.width() * s, g.height() * s, 0, 0, g.width(), g.height(), px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetCapture(hwnd);
        if (!g.skin().keys.empty()) {   // a click takes the keyboard back after the host has had it
            platformFocus(w, true);
            if (keyLogOn()) keyLog("click focus-now=%p ours=%p active=%p", (void*)GetFocus(), (void*)hwnd, (void*)GetActiveWindow());
        }
        g.mouseDown(x, y, false, msg == WM_LBUTTONDBLCLK, shift);
        return 0;
    case WM_RBUTTONDOWN:
        g.mouseDown(x, y, true, false, shift);
        return 0;
    case WM_RBUTTONUP:
        g.rightUp(x, y, shift);
        return 0;
    case WM_MOUSEMOVE:
        if (!w->tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            w->tracking = TrackMouseEvent(&t) != 0;
        }
        g.mouseMove(x, y, shift);
        return 0;
    case WM_MOUSELEAVE:
        w->tracking = false;
        g.mouseLeave();
        return 0;
    case WM_LBUTTONUP:
        g.mouseUp(x, y, shift);
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:   // lost the mouse mid-gesture: finish it as a release far outside
        g.mouseUp(INT_MIN / 2, INT_MIN / 2, false);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        g.wheel(floorDiv(p.x, s), floorDiv(p.y, s), GET_WHEEL_DELTA_WPARAM(wp) / 120.0, (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
        return 0;
    }
    case WM_TIMER:
        g.tick();
        return 0;
    // The editor holds the keyboard while its window is up, so a skin's "keys" shortcuts work without a
    // click first. It consumes only what it has a use for: text entry, a menu, a focused list, a modal, or
    // a chord in the skin's bindings. Anything else goes on to the window focus came from, so the host
    // keeps its own keys, the transport bar included.
    case WM_GETDLGCODE:
        if (g.wantsKeys() || !g.skin().keys.empty()) return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
        break;
    // Alt on its own would take the window into menu mode, whose feedback for a chord it finds no mnemonic
    // for is a beep and a flash of the title bar. A skin with shortcuts has already used the chord, so the
    // modifier's press and release stop here rather than reaching DefWindowProc or the host.
    case WM_SYSKEYUP:
        if (!g.skin().keys.empty() && (wp == VK_MENU || wp == VK_F10)) return 0;
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {   // Alt chords arrive as WM_SYSKEYDOWN, never WM_KEYDOWN
        if (!g.skin().keys.empty() && modifierKey(wp))
            return 0;   // a modifier alone is no keystroke for anyone, and Alt reaching DefWindowProc is the flash
        // Whether this key was ours decides what happens to the WM_CHAR or WM_SYSCHAR that follows it.
        w->ateKey = offerKey(g, wp);
        keyLog("key msg=%04x wp=%02x used=%d focus=%p", (unsigned)msg, (unsigned)wp, (int)w->ateKey, (void*)GetFocus());
        if (w->ateKey) return 0;
        if (forwardKey(w, msg, wp, lp)) return 0;
        break;   // not ours and nowhere to send it: DefWindowProc, so Alt+F4 and the system menu still work
    }
    case WM_CHAR:
    case WM_SYSCHAR: {
        // The character of a key we used is used too: forwarding it is what beeps and flashes the title bar,
        // and it would type Shift+1's "!" into the host as well.
        if (w->ateKey) {
            w->ateKey = false;
            return 0;
        }
        if (!g.wantsKeys()) {   // no text entry: the character is the host's
            if (forwardKey(w, msg, wp, lp)) return 0;
            break;
        }
        wchar_t c = (wchar_t)wp;
        if (c >= 0xd800 && c < 0xdc00) { w->high = c; return 0; }
        unsigned cp = c >= 0xdc00 && c < 0xe000 ? (w->high ? 0x10000 + ((w->high - 0xd800u) << 10) + (c - 0xdc00u) : 0) : c;
        w->high = 0;
        if (cp) g.keyChar(cp);   // control characters are ignored there; their keys came as WM_KEYDOWN
        return 0;
    }
    case WM_SETFOCUS:
        keyLog("WM_SETFOCUS from=%p", (void*)wp);
        break;
    case WM_KILLFOCUS:
        keyLog("WM_KILLFOCUS to=%p", (void*)wp);
        g.focusLost();
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The standalone's window, subclassed by platformWatchClose: WM_CLOSE asks the GUI first (a skin's
// "close" modal may stop it), unless platformCloseApp sent it.
static LRESULT CALLBACK rootProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = (PlatformWindow*)GetPropW(hwnd, L"HollowClose");
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_CLOSE && !w->closing && !w->gui->closeRequested()) return 0;
    return CallWindowProcW(w->rootProc, hwnd, msg, wp, lp);
}

PlatformWindow* platformOpen(void* parent, Gui* gui) {
    HMODULE mod = thisModule();
    if (classUsers++ == 0) {
        // One class per module, so two Hollow plug-ins in one host never share a window procedure.
        std::swprintf(className, 64, L"HollowView%p", (void*)mod);
        WNDCLASSEXW wc = {sizeof wc};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = mod;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);
    }
    auto* w = new PlatformWindow;
    w->gui = gui;
    w->hwnd = CreateWindowExW(0, className, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, gui->width() * gui->scale(),
                              gui->height() * gui->scale(), (HWND)parent, nullptr, mod, nullptr);
    if (!w->hwnd) {
        delete w;
        if (--classUsers == 0) UnregisterClassW(className, mod);
        return nullptr;
    }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    SetTimer(w->hwnd, 1, 33, nullptr);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    w->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, w->hwnd, nullptr, mod, nullptr);
    if (w->tip) tool(w, TTM_ADDTOOLW, "");
    // A skin with shortcuts holds the keyboard while its window is up, so its chords work without a click first,
    // and hooks this thread's message loop for the hosts that take keys off the window before it is dispatched.
    if (!gui->skin().keys.empty()) {
        addKeyHook(w);
        const HWND had = GetFocus();
        platformFocus(w, true);
        keyLog("open hwnd=%p parent=%p bindings=%d focus-was=%p focus-now=%p active=%p foreground=%p err=%lu", (void*)w->hwnd, parent,
               (int)gui->skin().keys.size(), (void*)had, (void*)GetFocus(), (void*)GetActiveWindow(), (void*)GetForegroundWindow(), GetLastError());
    }
    return w;
}

void platformClose(PlatformWindow* w) {
    if (!w->gui->skin().keys.empty()) removeKeyHook(w);   // before the window goes, so the hook sees no stale one
    if (w->root && IsWindow(w->root)) {   // the standalone's close goes back to its own procedure
        if (GetPropW(w->root, L"HollowClose") == w) RemovePropW(w->root, L"HollowClose");
        if ((WNDPROC)GetWindowLongPtrW(w->root, GWLP_WNDPROC) == rootProc) SetWindowLongPtrW(w->root, GWLP_WNDPROC, (LONG_PTR)w->rootProc);
    }
    if (w->tip) DestroyWindow(w->tip);
    KillTimer(w->hwnd, 1);
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(w->hwnd);
    delete w;
    if (--classUsers == 0) UnregisterClassW(className, thisModule());
}

const bool kNativeMenus = true;
void platformHold(PlatformWindow*, bool) {}   // the GUI runs on the host's thread

void platformInvalidate(PlatformWindow* w, Rect r) {
    int s = w->gui->scale();
    RECT rc = {r.x * s, r.y * s, (r.x + r.w) * s, (r.y + r.h) * s};
    InvalidateRect(w->hwnd, &rc, FALSE);
}

void platformSize(PlatformWindow* w, int width, int height) {
    SetWindowPos(w->hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(w->hwnd, nullptr, FALSE);
}

// Up the parent chain, each window that closely wraps ours (its frame adds at most 80 px either way)
// gets our new size plus that frame; an MDI client or a bigger window (the host's main window) stops
// the walk. Each window keeps its place.
void platformResizeParents(PlatformWindow* w, int width, int height) {
    HWND child = w->hwnd;
    int dw = 0, dh = 0;
    for (HWND p = GetParent(child); p; child = p, p = GetParent(p)) {
        wchar_t cls[64] = L"";
        if (GetClassNameW(p, cls, 64) && std::wcscmp(cls, L"MDIClient") == 0) break;
        RECT rc, rp;
        GetWindowRect(child, &rc);
        GetWindowRect(p, &rp);
        dw = std::max(0, dw + (int)((rp.right - rp.left) - (rc.right - rc.left)));
        dh = std::max(0, dh + (int)((rp.bottom - rp.top) - (rc.bottom - rc.top)));
        if (dw > 80 || dh > 80) break;
        SetWindowPos(p, nullptr, 0, 0, width + dw, height + dh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// Entries report id + 1 as their command (0 is "no choice"); submenus nest, column breaks start a column.
static void fillMenu(HMENU m, const std::vector<MenuEntry>& items) {
    for (auto& e : items) {
        UINT brk = e.columnBreak ? MF_MENUBREAK : 0;   // a new column without a divider line
        if (e.separator) {
            AppendMenuW(m, MF_SEPARATOR | brk, 0, nullptr);
        } else if (!e.items.empty()) {
            HMENU sub = CreatePopupMenu();
            fillMenu(sub, e.items);
            AppendMenuW(m, MF_POPUP | MF_STRING | brk, (UINT_PTR)sub, wide(e.label).c_str());
        } else {
            AppendMenuW(m, MF_STRING | brk | (e.checked ? MF_CHECKED : 0) | (e.disabled ? MF_GRAYED : 0), (UINT_PTR)(e.id + 1), wide(e.label).c_str());
        }
    }
}

int platformMenu(PlatformWindow* w, const std::vector<MenuEntry>& items, int x, int y) {
    HMENU m = CreatePopupMenu();
    fillMenu(m, items);
    int s = w->gui->scale();
    POINT p = {x * s, y * s};
    ClientToScreen(w->hwnd, &p);
    int r = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, p.x, p.y, 0, w->hwnd, nullptr);
    DestroyMenu(m);
    return r - 1;
}

// Its text follows the widget under the pointer.
void platformTip(PlatformWindow* w, const std::string& text) {
    if (!w->tip) return;
    tool(w, TTM_UPDATETIPTEXTW, text);
    if (text.empty()) SendMessageW(w->tip, TTM_POP, 0, 0);
}

void platformFocus(PlatformWindow* w, bool on) {
    if (on) {
        if (GetFocus() != w->hwnd) w->lastFocus = SetFocus(w->hwnd);
        return;
    }
    if (GetFocus() != w->hwnd) return;
    HWND back = w->lastFocus && IsWindow(w->lastFocus) ? w->lastFocus : GetParent(w->hwnd);
    w->lastFocus = nullptr;
    SetFocus(back);
}

// VkKeyScanW inverts the layout: the low byte of what it returns is the virtual key that types this
// character, which for a letter or a digit is that character's own ASCII code, so "2" comes back off a
// quote typed with Shift on a layout that puts it there, and the high byte's first bit says Shift was what
// it took. -1 means no key on this layout types it. Only the Shift bit is read; a character that needs
// AltGr sets the Ctrl and Alt bits too, and those are the host's to report rather than the layout's.
unsigned platformKeyChar(unsigned codepoint, bool* shift) {
    if (shift) *shift = false;
    if (codepoint > 0xffff) return 0;
    const SHORT s = VkKeyScanW((WCHAR)codepoint);
    if (s == -1) return 0;
    const unsigned vk = (unsigned)(s & 0xff);
    if (!((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))) return 0;
    if (shift) *shift = (s & 0x100) != 0;
    return vk;
}

void platformOpenUrl(const std::string& url) {
    if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0)
        ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// clap-wrapper's standalone window opens its settings from its system menu; the command's id is 0
// (Menu::Identifier::AudioMidiSettings in windows_standalone.h).
const bool kAudioSettings = true;
void platformAudioSettings(PlatformWindow* w) {
    PostMessageW(GetAncestor(w->hwnd, GA_ROOT), WM_SYSCOMMAND, 0, 0);
}

void platformWatchClose(PlatformWindow* w) {
    HWND root = GetAncestor(w->hwnd, GA_ROOT);
    if (!root || root == w->hwnd || w->root) return;
    // Shortcut: SetWindowLongPtr rather than SetWindowSubclass, which comctl32 exports by name only from
    // version 6 and a plug-in without a manifest cannot count on; the old procedure goes back on close.
    SetPropW(root, L"HollowClose", w);
    w->root = root;
    w->rootProc = (WNDPROC)SetWindowLongPtrW(root, GWLP_WNDPROC, (LONG_PTR)rootProc);
}

void platformCloseApp(PlatformWindow* w) {
    w->closing = true;
    PostMessageW(GetAncestor(w->hwnd, GA_ROOT), WM_CLOSE, 0, 0);
}

// The settings window clap-wrapper keeps hidden until its system menu shows it: its comboboxes and the
// MIDI list box are children with the ids 0 to 5 (Settings::Identifier in windows_standalone.h), and a
// change reaches its WM_COMMAND handler as the control's own notification would, so the app applies and
// saves it as when the window is used by hand.
static HWND settingsWindow() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM lp) -> BOOL {
        wchar_t t[64] = L"";
        if (!GetWindowTextW(h, t, 64) || std::wcscmp(t, L"Audio/MIDI Settings") != 0) return TRUE;
        *(HWND*)lp = h;
        return FALSE;
    }, (LPARAM)&found);
    return found;
}

bool platformDevices(PlatformWindow*, std::vector<DeviceList>& out) {
    HWND s = settingsWindow();
    if (!s) return false;
    out.assign(6, {});
    for (int k = 0; k < 6; ++k) {
        HWND c = GetDlgItem(s, k);
        if (!c) return false;
        bool list = k == 5;
        int n = (int)SendMessageW(c, list ? LB_GETCOUNT : CB_GETCOUNT, 0, 0);
        for (int i = 0; i < n; ++i) {
            int len = (int)SendMessageW(c, list ? LB_GETTEXTLEN : CB_GETLBTEXTLEN, i, 0);
            std::wstring t((size_t)std::max(len, 0) + 1, L'\0');
            SendMessageW(c, list ? LB_GETTEXT : CB_GETLBTEXT, i, (LPARAM)&t[0]);
            t.resize(std::wcslen(t.c_str()));
            out[(size_t)k].items.push_back(narrow(t));
            if (list && SendMessageW(c, LB_GETSEL, i, 0) > 0) out[(size_t)k].on.push_back(i);
        }
        int sel = list ? -1 : (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
        if (sel >= 0) out[(size_t)k].on.push_back(sel);
    }
    return true;
}

void platformSetDevice(PlatformWindow*, int list, int item) {
    HWND s = settingsWindow(), c = s ? GetDlgItem(s, list) : nullptr;
    if (!c) return;
    if (list == 5) SendMessageW(c, LB_SETSEL, SendMessageW(c, LB_GETSEL, item, 0) > 0 ? FALSE : TRUE, item);
    else if (SendMessageW(c, CB_SETCURSEL, item, 0) == CB_ERR) return;
    SendMessageW(s, WM_COMMAND, MAKEWPARAM(list, list == 5 ? LBN_SELCHANGE : CBN_SELCHANGE), (LPARAM)c);
}

// The common dialogs: a filter of "Name (*.a;*.b)" then "*.a;*.b" per type, a save's extension the first type's first.
bool platformFileDialog(PlatformWindow* w, bool save, const std::string& title, const Vars& types, const std::string& name, std::string& path) {
    std::wstring filter, ext;
    for (auto& t : types) {
        std::wstring pats;
        size_t a = 0;
        while (a <= t.second.size()) {
            size_t b = t.second.find(';', a);
            std::string e = t.second.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!e.empty()) {
                pats += (pats.empty() ? L"*." : L";*.") + wide(e);
                if (ext.empty()) ext = wide(e);
            }
            if (b == std::string::npos) break;
            a = b + 1;
        }
        filter += wide(t.first) + L" (" + pats + L")" + L'\0' + pats + L'\0';
    }
    filter += L'\0';
    wchar_t file[MAX_PATH * 4] = {};
    std::wstring start = wide(name);
    wcsncpy_s(file, start.c_str(), _TRUNCATE);
    std::wstring caption = wide(title);
    OPENFILENAMEW o = {};
    o.lStructSize = sizeof o;
    o.hwndOwner = GetAncestor(w->hwnd, GA_ROOT);
    o.lpstrFilter = types.empty() ? nullptr : filter.c_str();
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    o.lpstrTitle = caption.empty() ? nullptr : caption.c_str();
    o.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
    o.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o))) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
    path.assign(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, file, -1, &path[0], n, nullptr, nullptr);
    return !path.empty();
}

} // namespace hollow
