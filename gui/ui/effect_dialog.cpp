#include "effect_dialog.h"

#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include <cwchar>

namespace ui {
namespace {

constexpr wchar_t kDialogClass[] = L"BetterMicroApoEffectDialog";

enum : int {
    IDC_TYPE_LABEL = 100,
    IDC_TYPE_COMBO,
    IDC_OK,
    IDC_CANCEL,
};

struct DialogState {
    const apo::ApoSettings* settings = nullptr;
    HWND label = nullptr;
    HWND combo = nullptr;
    HWND ok = nullptr;
    HWND cancel = nullptr;
    bool accepted = false;
    apo::EffectType type = apo::EffectType::Noise;
};

int CountType(const apo::ApoSettings& settings, apo::EffectType type)
{
    int count = 0;
    for (int i = 0; i < settings.effectCount; ++i) {
        if (settings.effects[i].type == type) {
            ++count;
        }
    }
    return count;
}

void LayoutDialog(HWND window, DialogState& state)
{
    RECT rect{};
    GetClientRect(window, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    const int margin = Scale(16);
    const int buttonWidth = Scale(90);
    const int buttonHeight = Scale(30);
    const int buttonY = height - margin - buttonHeight;

    MoveWindow(state.label, margin, margin, width - 2 * margin, Scale(20),
               TRUE);
    MoveWindow(state.combo, margin, margin + Scale(26), width - 2 * margin,
               Scale(200), TRUE);
    MoveWindow(state.cancel, width - margin - buttonWidth, buttonY, buttonWidth,
               buttonHeight, TRUE);
    MoveWindow(state.ok, width - margin - 2 * buttonWidth - Scale(8), buttonY,
               buttonWidth, buttonHeight, TRUE);
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

        created->label =
            Label(window, IDC_TYPE_LABEL, L"Effect type");
        created->combo = ComboBox(window, IDC_TYPE_COMBO, CBS_DROPDOWNLIST);
        created->ok = Button(window, IDC_OK, L"OK");
        created->cancel = Button(window, IDC_CANCEL, L"Cancel");

        const HWND ok = created->ok;
        SendMessageW(ok, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        int selected = -1;
        for (int i = 0; i < static_cast<int>(apo::EffectType::Count); ++i) {
            const auto type = static_cast<apo::EffectType>(i);
            if (created->settings != nullptr &&
                CountType(*created->settings, type) >=
                    apo::EffectTypeCapacity(type)) {
                continue;
            }
            const int item = static_cast<int>(SendMessageW(
                created->combo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(apo::EffectTypeName(type))));
            SendMessageW(created->combo, CB_SETITEMDATA, item,
                         static_cast<LPARAM>(i));
            if (selected < 0) {
                selected = item;
            }
        }
        SendMessageW(created->combo, CB_SETCURSEL, selected, 0);
        if (selected < 0) {
            return -1;
        }

        LayoutDialog(window, *created);
        ApplyTheme(window);
        SetFocus(created->combo);
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
    case WM_CTLCOLOREDIT:
        return HandleEditColor(reinterpret_cast<HDC>(wParam));
    case WM_CTLCOLORLISTBOX:
        return HandleListBoxColor(reinterpret_cast<HDC>(wParam));

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_OK:
            if (state != nullptr) {
                const int item = static_cast<int>(
                    SendMessageW(state->combo, CB_GETCURSEL, 0, 0));
                if (item >= 0) {
                    const int type = static_cast<int>(SendMessageW(
                        state->combo, CB_GETITEMDATA, item, 0));
                    if (type >= 0 &&
                        type < static_cast<int>(apo::EffectType::Count)) {
                        state->type = static_cast<apo::EffectType>(type);
                        state->accepted = true;
                    }
                }
            }
            DestroyWindow(window);
            return 0;
        case IDC_CANCEL:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;

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

bool ShowAddEffectDialog(HWND owner, const apo::ApoSettings& settings,
                         apo::EffectType& type)
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
            return false;
        }
        registered = true;
    }

    DialogState state;
    state.settings = &settings;
    state.type = type;

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDialogClass,
        L"Add effect", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, Scale(340), Scale(180), owner, nullptr,
        g_instance, &state);
    if (dialog == nullptr) {
        return false;
    }

    // The state object is on this stack frame and outlives the modal loop.
    RECT ownerRect{};
    if (owner != nullptr && GetWindowRect(owner, &ownerRect)) {
        RECT dialogRect{};
        GetWindowRect(dialog, &dialogRect);
        const int x = ownerRect.left +
                      ((ownerRect.right - ownerRect.left) -
                       (dialogRect.right - dialogRect.left)) /
                          2;
        const int y = ownerRect.top +
                      ((ownerRect.bottom - ownerRect.top) -
                       (dialogRect.bottom - dialogRect.top)) /
                          2;
        SetWindowPos(dialog, HWND_TOP, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
    }

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
    // WM_CREATE runs before the window is visible, so set the initial focus
    // again now that the dialog can take it.
    if (state.combo != nullptr) {
        SetFocus(state.combo);
    }

    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        const bool dropdownOpen =
            state.combo != nullptr &&
            SendMessageW(state.combo, CB_GETDROPPEDSTATE, 0, 0) != 0;
        if (message.message == WM_KEYDOWN && !dropdownOpen) {
            if (message.wParam == VK_RETURN) {
                SendMessageW(dialog, WM_COMMAND,
                             MAKEWPARAM(IDC_OK, BN_CLICKED), 0);
                continue;
            }
            if (message.wParam == VK_ESCAPE) {
                SendMessageW(dialog, WM_COMMAND,
                             MAKEWPARAM(IDC_CANCEL, BN_CLICKED), 0);
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

    if (state.accepted) {
        type = state.type;
        return true;
    }
    return false;
}

} // namespace ui
