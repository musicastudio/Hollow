// clap-wrapper's Windows standalone calls four of Windows 10's per-monitor DPI functions directly, so
// Windows 7 would refuse to start the app ("entry point not found"). Each is defined here as the import
// pointer the linker would otherwise take from user32.lib: the app calls the real function where Windows
// has it, and on Windows 7 falls back to the system DPI, which is the only one it has. The standalone is
// 64-bit only, so the names are x64's undecorated ones. FSVR's tools/check_win7.py checks the result.
#include <windows.h>

namespace {

template <class F> F user32(const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name)));
}

UINT systemDpi() {
    HDC dc = GetDC(nullptr);
    const UINT dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi;
}

UINT WINAPI getDpiForWindow(HWND w) {
    static const auto f = user32<UINT(WINAPI*)(HWND)>("GetDpiForWindow");
    return f ? f(w) : systemDpi();
}

int WINAPI getSystemMetricsForDpi(int index, UINT dpi) {
    static const auto f = user32<int(WINAPI*)(int, UINT)>("GetSystemMetricsForDpi");
    return f ? f(index, dpi) : MulDiv(GetSystemMetrics(index), (int)dpi, (int)systemDpi());
}

BOOL WINAPI adjustWindowRectExForDpi(LPRECT r, DWORD style, BOOL menu, DWORD exStyle, UINT dpi) {
    static const auto f = user32<BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT)>("AdjustWindowRectExForDpi");
    return f ? f(r, style, menu, exStyle, dpi) : AdjustWindowRectEx(r, style, menu, exStyle);
}

BOOL WINAPI systemParametersInfoForDpi(UINT action, UINT param, PVOID value, UINT winIni, UINT dpi) {
    static const auto f = user32<BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT)>("SystemParametersInfoForDpi");
    return f ? f(action, param, value, winIni, dpi) : SystemParametersInfoW(action, param, value, winIni);
}

} // namespace

extern "C" {
decltype(&getDpiForWindow) __imp_GetDpiForWindow = getDpiForWindow;
decltype(&getSystemMetricsForDpi) __imp_GetSystemMetricsForDpi = getSystemMetricsForDpi;
decltype(&adjustWindowRectExForDpi) __imp_AdjustWindowRectExForDpi = adjustWindowRectExForDpi;
decltype(&systemParametersInfoForDpi) __imp_SystemParametersInfoForDpi = systemParametersInfoForDpi;
}
