#include "info_dialog.h"

#include "apo_internal.h"
#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include <array>
#include <cwchar>
#include <string>

namespace ui {
namespace {

constexpr wchar_t kDialogClass[] = L"BetterMicroApoInfoDialog";

enum Row : int {
    RowEmbeddedHash,
    RowInstalledHash,
    RowInstallPath,
    RowDllFile,
    RowComClsid,
    RowComRegistration,
    RowApoKey,
    RowCaptureKey,
    RowStatus,
    RowApplied,
    RowCount,
};

enum : int {
    IDC_TITLE = 100,
    IDC_ROW_FIRST = 101,
    IDC_OK = IDC_ROW_FIRST + RowCount,
};

struct DialogState {
    apo::FilesStatus files;
    int boundDevices = 0;
    HWND title = nullptr;
    HWND rows[RowCount]{};
    HWND ok = nullptr;
};

std::wstring HashText(unsigned long long hash)
{
    if (hash == 0) {
        return L"unavailable";
    }
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%016llx", hash);
    return buffer;
}

std::wstring DllFileName()
{
    const wchar_t* slash = wcsrchr(apo::kInstalledDll, L'\\');
    return slash != nullptr ? slash + 1 : apo::kInstalledDll;
}

std::wstring KeyState(bool present)
{
    return present ? L"present" : L"missing";
}

std::wstring Summary(const apo::FilesStatus& files)
{
    if (files.DllStale()) {
        return L"Old DLL installed. Use File / Install / Update.";
    }
    if (files.FullyInstalled()) {
        return L"Installed";
    }
    if (files.AnyPresent()) {
        return L"Not installed (partial registration)";
    }
    return L"Not installed";
}

std::wstring ComRegistration(const apo::FilesStatus& files)
{
    if (files.registeredPath.empty()) {
        return L"key missing";
    }
    if (files.comRegistered) {
        return L"registered";
    }
    return L"wrong path: " + files.registeredPath;
}

bool IsRowVisible(const DialogState& state, int row)
{
    if (row == RowInstallPath || row == RowDllFile) {
        return state.files.dllPresent;
    }
    if (row == RowApplied) {
        return state.files.FullyInstalled() || state.boundDevices > 0;
    }
    return true;
}

std::array<std::wstring, RowCount> BuildRows(const DialogState& state)
{
    const apo::FilesStatus& files = state.files;

    std::array<std::wstring, RowCount> rows;
    rows[RowEmbeddedHash] =
        L"Embedded DLL hash: " + HashText(files.embeddedHash);
    rows[RowInstalledHash] = L"Installed DLL hash: " +
                             (files.dllPresent ? HashText(files.installedHash)
                                               : L"not found");
    rows[RowInstallPath] =
        L"Install path: " + std::wstring(apo::kInstallDir);
    rows[RowDllFile] = L"DLL file: " + DllFileName();
    rows[RowComClsid] = L"COM CLSID: " + std::wstring(apo::kClsid);
    rows[RowComRegistration] =
        L"COM registration: " + ComRegistration(files);
    rows[RowApoKey] =
        L"APO registration key: " + KeyState(files.apoKeyPresent);
    rows[RowCaptureKey] =
        L"Capture device key: " +
        KeyState(apo::detail::RegKeyExists(HKEY_LOCAL_MACHINE,
                                           apo::kCaptureRoot));
    rows[RowStatus] = L"Status: " + Summary(files);
    rows[RowApplied] = L"Applied to " + std::to_wstring(state.boundDevices) +
                       (state.boundDevices == 1 ? L" device" : L" devices");
    return rows;
}

void LayoutDialog(HWND window, DialogState& state)
{
    RECT rect{};
    GetClientRect(window, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    const int margin = Scale(16);
    const int rowHeight = Scale(22);
    const int buttonWidth = Scale(90);
    const int buttonHeight = Scale(30);
    const int buttonY = height - margin - buttonHeight;

    MoveWindow(state.title, margin, margin, width - 2 * margin, Scale(26),
               TRUE);

    int rowY = margin + Scale(36);
    for (int row = 0; row < RowCount; ++row) {
        const bool visible = IsRowVisible(state, row);
        ShowWindow(state.rows[row], visible ? SW_SHOW : SW_HIDE);
        if (!visible) {
            continue;
        }
        MoveWindow(state.rows[row], margin, rowY, width - 2 * margin,
                   rowHeight, TRUE);
        rowY += rowHeight;
    }

    MoveWindow(state.ok, width - margin - buttonWidth, buttonY, buttonWidth,
               buttonHeight, TRUE);
}

void ApplyTheme(HWND window)
{
    ApplyTitleBarTheme(window);
    ApplyNativeThemeToChildren(window);
    InvalidateRect(window, nullptr, TRUE);
}

LRESULT CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam)
{
    auto* state = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* created = reinterpret_cast<DialogState*>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(created));
        if (created == nullptr) {
            return -1;
        }

        created->title = Label(window, IDC_TITLE, L"Better Micro APO");
        ApplyFont(created->title, g_boldFont);

        const std::array<std::wstring, RowCount> rows = BuildRows(*created);
        for (int row = 0; row < RowCount; ++row) {
            created->rows[row] =
                Label(window, IDC_ROW_FIRST + row, rows[row].c_str());
        }

        created->ok = Button(window, IDC_OK, L"OK");

        LayoutDialog(window, *created);
        ApplyTheme(window);
        return 0;
    }

    case WM_SIZE:
        if (state != nullptr) {
            LayoutDialog(window, *state);
        }
        return 0;

    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect,
                 GetTheme().backgroundBrush);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(dc, &rect, GetTheme().backgroundBrush);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        return HandleStaticColor(reinterpret_cast<HDC>(wParam), false);
    case WM_CTLCOLORBTN:
        return HandleButtonColor(reinterpret_cast<HDC>(wParam));

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_OK) {
            DestroyWindow(window);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_SETTINGCHANGE:
        if (wParam == 0 && lParam != 0 &&
            wcscmp(reinterpret_cast<const wchar_t*>(lParam),
                   L"ImmersiveColorSet") == 0) {
            RefreshTheme();
            ApplyTheme(window);
        }
        return 0;

    case WM_THEMECHANGED:
        RefreshTheme();
        ApplyTheme(window);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

void ShowInfoDialog(HWND owner, const apo::FilesStatus& files,
                    int boundDevices)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DialogProc;
        windowClass.hInstance = g_instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kDialogClass;
        if (!RegisterClassExW(&windowClass)) {
            return;
        }
        registered = true;
    }

    DialogState state;
    state.files = files;
    state.boundDevices = boundDevices;

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDialogClass,
        L"Better Micro APO - Info", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, Scale(560), Scale(360), owner, nullptr,
        g_instance, &state);
    if (dialog == nullptr) {
        return;
    }

    RECT ownerRect{};
    if (owner != nullptr && GetWindowRect(owner, &ownerRect)) {
        RECT dialogRect{};
        GetWindowRect(dialog, &dialogRect);
        const int x = ownerRect.left +
                      ((ownerRect.right - ownerRect.left) -
                       (dialogRect.right - dialogRect.left)) / 2;
        const int y = ownerRect.top +
                      ((ownerRect.bottom - ownerRect.top) -
                       (dialogRect.bottom - dialogRect.top)) / 2;
        SetWindowPos(dialog, HWND_TOP, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
    }

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);

    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN) {
            if (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE) {
                SendMessageW(dialog, WM_COMMAND,
                             MAKEWPARAM(IDC_OK, BN_CLICKED), 0);
                continue;
            }
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
}

} // namespace ui
