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
#include <vector>
#include <cwchar>

namespace hollow {

struct PlatformWindow {
    HWND hwnd = nullptr, tip = nullptr;
    HWND lastFocus = nullptr;   // had the keyboard before text entry or a menu took it
    Gui* gui = nullptr;
    bool tracking = false;
    wchar_t high = 0;           // WM_CHAR: the first half of a surrogate pair
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
    // The keyboard, only while text entry or a skinned menu has it (platformFocus); otherwise keys
    // stay with the host.
    case WM_GETDLGCODE:
        if (g.wantsKeys()) return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
        break;
    case WM_KEYDOWN: {
        if (!g.wantsKeys()) break;
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
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
        if (k != KeyNone) g.keyDown(k, GetKeyState(VK_SHIFT) < 0, ctrl);
        return 0;
    }
    case WM_CHAR: {
        if (!g.wantsKeys()) break;
        wchar_t c = (wchar_t)wp;
        if (c >= 0xd800 && c < 0xdc00) { w->high = c; return 0; }
        unsigned cp = c >= 0xdc00 && c < 0xe000 ? (w->high ? 0x10000 + ((w->high - 0xd800u) << 10) + (c - 0xdc00u) : 0) : c;
        w->high = 0;
        if (cp) g.keyChar(cp);   // control characters are ignored there; their keys came as WM_KEYDOWN
        return 0;
    }
    case WM_KILLFOCUS:
        g.focusLost();
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
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
    return w;
}

void platformClose(PlatformWindow* w) {
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
            AppendMenuW(m, MF_STRING | brk | (e.checked ? MF_CHECKED : 0), (UINT_PTR)(e.id + 1), wide(e.label).c_str());
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
