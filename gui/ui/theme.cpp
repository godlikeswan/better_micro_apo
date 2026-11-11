#include "theme.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <cwchar>
#include <iterator>

namespace ui {
namespace {

constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmUseImmersiveDarkModeBefore20H1 = 19;

Theme g_theme{};

using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

HMODULE GetUxTheme()
{
    static HMODULE module = LoadLibraryW(L"uxtheme.dll");
    return module;
}

void SetPreferredAppMode(int mode)
{
    const HMODULE module = GetUxTheme();
    if (module == nullptr) {
        return;
    }
    auto function = reinterpret_cast<SetPreferredAppModeFn>(
        GetProcAddress(module, MAKEINTRESOURCEA(135)));
    if (function != nullptr) {
        function(mode);
    }
}

void AllowDarkModeForWindow(HWND window, bool allow)
{
    const HMODULE module = GetUxTheme();
    if (module == nullptr) {
        return;
    }
    auto function = reinterpret_cast<AllowDarkModeForWindowFn>(
        GetProcAddress(module, MAKEINTRESOURCEA(133)));
    if (function != nullptr) {
        function(window, allow ? TRUE : FALSE);
    }
}

HBRUSH RecreateBrush(HBRUSH& brush, COLORREF color)
{
    if (brush != nullptr) {
        DeleteObject(brush);
    }
    brush = CreateSolidBrush(color);
    return brush;
}

void BuildTheme(bool dark)
{
    Theme& theme = g_theme;
    theme.dark = dark;

    if (dark) {
        theme.color.background = RGB(32, 32, 32);
        theme.color.text = RGB(240, 240, 240);
        theme.color.disabledText = RGB(132, 132, 132);
        theme.color.frame = RGB(92, 92, 92);
        theme.color.error = RGB(255, 120, 120);
        theme.color.editBackground = RGB(45, 45, 45);
        theme.color.editText = RGB(240, 240, 240);
        theme.color.listBackground = RGB(38, 38, 38);
        theme.color.listText = RGB(235, 235, 235);
        theme.color.listHeaderBackground = RGB(50, 50, 50);
        theme.color.controlBackground = RGB(58, 58, 58);
        theme.color.controlHot = RGB(70, 70, 70);
        theme.color.controlPressed = RGB(44, 44, 44);
        theme.color.controlBorder = RGB(96, 96, 96);
        theme.color.controlText = RGB(240, 240, 240);
        theme.color.accent = RGB(0, 120, 212);
        theme.color.accentText = RGB(255, 255, 255);
        theme.color.tabBackground = RGB(40, 40, 40);
        theme.color.tabText = RGB(190, 190, 190);
        theme.color.tabSelectedText = RGB(240, 240, 240);
        theme.color.checkBackground = RGB(45, 45, 45);
        theme.color.checkBorder = RGB(110, 110, 110);
        theme.color.checkMark = RGB(90, 180, 255);
    } else {
        theme.color.background = RGB(240, 240, 240);
        theme.color.text = RGB(0, 0, 0);
        theme.color.disabledText = RGB(109, 109, 109);
        theme.color.frame = RGB(160, 160, 160);
        theme.color.error = RGB(200, 0, 0);
        theme.color.editBackground = RGB(255, 255, 255);
        theme.color.editText = RGB(0, 0, 0);
        theme.color.listBackground = RGB(255, 255, 255);
        theme.color.listText = RGB(0, 0, 0);
        theme.color.listHeaderBackground = RGB(240, 240, 240);
        theme.color.controlBackground = RGB(240, 240, 240);
        theme.color.controlHot = RGB(245, 245, 245);
        theme.color.controlPressed = RGB(225, 225, 225);
        theme.color.controlBorder = RGB(160, 160, 160);
        theme.color.controlText = RGB(0, 0, 0);
        theme.color.accent = RGB(0, 120, 212);
        theme.color.accentText = RGB(255, 255, 255);
        theme.color.tabBackground = RGB(240, 240, 240);
        theme.color.tabText = RGB(0, 0, 0);
        theme.color.tabSelectedText = RGB(0, 0, 0);
        theme.color.checkBackground = RGB(255, 255, 255);
        theme.color.checkBorder = RGB(120, 120, 120);
        theme.color.checkMark = RGB(0, 90, 158);
    }

    RecreateBrush(theme.backgroundBrush, theme.color.background);
    RecreateBrush(theme.editBrush, theme.color.editBackground);
    RecreateBrush(theme.listBrush, theme.color.listBackground);
    RecreateBrush(theme.listHeaderBrush, theme.color.listHeaderBackground);
}

} // namespace

bool SystemUsesDarkMode()
{
    DWORD light = 1;
    DWORD size = sizeof(light);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    return light == 0;
}

const Theme& GetTheme()
{
    return g_theme;
}

void InitializeTheme()
{
    // AllowDark lets the common-control theme follow the system setting.
    SetPreferredAppMode(1);
    BuildTheme(SystemUsesDarkMode());
}

void ShutdownTheme()
{
    DeleteObject(g_theme.backgroundBrush);
    DeleteObject(g_theme.editBrush);
    DeleteObject(g_theme.listBrush);
    DeleteObject(g_theme.listHeaderBrush);
    g_theme = Theme{};
}

void ApplyTitleBarTheme(HWND window)
{
    if (window == nullptr) {
        return;
    }

    HMODULE module = LoadLibraryW(L"dwmapi.dll");
    if (module == nullptr) {
        return;
    }

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID,
                                                     DWORD);
    auto function = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(module, "DwmSetWindowAttribute"));
    if (function != nullptr) {
        const BOOL dark = g_theme.dark ? TRUE : FALSE;
        if (FAILED(function(window, kDwmUseImmersiveDarkMode, &dark,
                            sizeof(dark)))) {
            function(window, kDwmUseImmersiveDarkModeBefore20H1, &dark,
                     sizeof(dark));
        }
    }
    FreeLibrary(module);
}

void ApplyNativeTheme(HWND window)
{
    if (window == nullptr) {
        return;
    }

    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));

    // Only touch the classes that have a dark variant. Calling SetWindowTheme
    // with an empty name on a push button or checkbox would disable its normal
    // visual style, which looks worse in light mode than leaving it alone.
    if (wcscmp(className, L"Edit") == 0 ||
        wcscmp(className, L"ComboBox") == 0) {
        AllowDarkModeForWindow(window, g_theme.dark);
        SetWindowTheme(window, g_theme.dark ? L"DarkMode_CFD" : nullptr,
                       nullptr);
    } else if (wcscmp(className, L"SysListView32") == 0) {
        AllowDarkModeForWindow(window, g_theme.dark);
        SetWindowTheme(window, g_theme.dark ? L"DarkMode_Explorer" : nullptr,
                       nullptr);
        ListView_SetBkColor(window, g_theme.color.listBackground);
        ListView_SetTextBkColor(window, g_theme.color.listBackground);
        ListView_SetTextColor(window, g_theme.color.listText);
    } else if (wcscmp(className, L"SysHeader32") == 0) {
        AllowDarkModeForWindow(window, g_theme.dark);
        SetWindowTheme(window, g_theme.dark ? L"DarkMode_ItemsView" : nullptr,
                       nullptr);
    } else if (wcscmp(className, L"SysTabControl32") == 0) {
        AllowDarkModeForWindow(window, g_theme.dark);
        SetWindowTheme(window, g_theme.dark ? L"DarkMode_Explorer" : nullptr,
                       nullptr);
    }
}

void ApplyNativeThemeToChildren(HWND root)
{
    if (root == nullptr) {
        return;
    }

    EnumChildWindows(
        root,
        [](HWND child, LPARAM) -> BOOL {
            ApplyNativeTheme(child);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(root));
}

void RefreshTheme()
{
    BuildTheme(SystemUsesDarkMode());
}

LRESULT HandleStaticColor(HDC dc, bool error)
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, error ? g_theme.color.error : g_theme.color.text);
    return reinterpret_cast<LRESULT>(g_theme.backgroundBrush);
}

LRESULT HandleButtonColor(HDC dc)
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_theme.color.text);
    return reinterpret_cast<LRESULT>(g_theme.backgroundBrush);
}

LRESULT HandleEditColor(HDC dc)
{
    SetBkColor(dc, g_theme.color.editBackground);
    SetTextColor(dc, g_theme.color.editText);
    return reinterpret_cast<LRESULT>(g_theme.editBrush);
}

LRESULT HandleListBoxColor(HDC dc)
{
    SetBkColor(dc, g_theme.color.listBackground);
    SetTextColor(dc, g_theme.color.listText);
    return reinterpret_cast<LRESULT>(g_theme.listBrush);
}

} // namespace ui
