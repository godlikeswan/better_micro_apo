#include "widgets.h"

#include "theme.h"
#include "ui_state.h"

#include <commctrl.h>

#include <cmath>
#include <cwchar>
#include <iterator>

namespace ui {
namespace {

// The system draws a group box frame but leaves its interior to the parent.
// The page panel uses WS_CLIPCHILDREN, so its erase never reaches the area
// under a group box, and hiding an effect control leaves its pixels behind.
// Paint the face background and frame ourselves so every redraw clears it.
LRESULT CALLBACK GroupFrameProc(HWND window, UINT message, WPARAM wParam,
                                LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message != WM_PAINT) {
        return DefSubclassProc(window, message, wParam, lParam);
    }

    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);

    const Theme& theme = GetTheme();

    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, theme.backgroundBrush);

    wchar_t caption[128]{};
    GetWindowTextW(window, caption, static_cast<int>(std::size(caption)));

    HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(window, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font != nullptr) {
        oldFont = SelectObject(dc, font);
    }

    SIZE text{};
    GetTextExtentPoint32W(dc, caption, static_cast<int>(wcslen(caption)),
                          &text);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme.color.text);

    const int gap = Scale(8);
    const int lineY = text.cy / 2;

    RECT captionRect{client.left + gap, 0, client.left + gap + text.cx,
                     text.cy};
    DrawTextW(dc, caption, -1, &captionRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const int afterCaption = captionRect.right + gap;
    const HBRUSH line = CreateSolidBrush(theme.color.frame);
    RECT topLeft{client.left, lineY, client.left + gap, lineY + 1};
    RECT topRight{afterCaption, lineY, client.right, lineY + 1};
    RECT left{client.left, lineY, client.left + 1, client.bottom - 1};
    RECT right{client.right - 1, lineY, client.right, client.bottom - 1};
    RECT bottom{client.left, client.bottom - 1, client.right, client.bottom};
    FillRect(dc, &topLeft, line);
    FillRect(dc, &topRight, line);
    FillRect(dc, &left, line);
    FillRect(dc, &right, line);
    FillRect(dc, &bottom, line);
    DeleteObject(line);

    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
    EndPaint(window, &paint);
    return 0;
}

constexpr wchar_t kHoverProp[] = L"BetterMicroApoHover";

bool IsHot(HWND window)
{
    return GetPropW(window, kHoverProp) != nullptr;
}

void SetHot(HWND window, bool hot)
{
    if (hot) {
        SetPropW(window, kHoverProp, reinterpret_cast<HANDLE>(1));
    } else {
        RemovePropW(window, kHoverProp);
    }
}

void SelectFont(HDC dc, HWND window, HGDIOBJ& oldFont)
{
    HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(window, WM_GETFONT, 0, 0));
    if (font != nullptr) {
        oldFont = SelectObject(dc, font);
    }
}

void DrawPushButton(HWND window, HDC dc, const Theme& theme)
{
    RECT client{};
    GetClientRect(window, &client);

    // The parent clips children, so each control owns its background pixels.
    // Clear the whole client before drawing the rounded face; otherwise the
    // corners keep whatever was underneath.
    FillRect(dc, &client, theme.backgroundBrush);

    const LONG style = GetWindowLongW(window, GWL_STYLE);
    const bool primary = (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
    const bool disabled = !IsWindowEnabled(window);
    const bool pressed =
        (SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
    const bool hot = IsHot(window) && !disabled;

    COLORREF background = theme.color.controlBackground;
    COLORREF border = theme.color.controlBorder;
    COLORREF text = theme.color.controlText;
    if (disabled) {
        text = theme.color.disabledText;
    } else if (primary) {
        background = theme.color.accent;
        border = theme.color.accent;
        text = theme.color.accentText;
    } else if (pressed) {
        background = theme.color.controlPressed;
    } else if (hot) {
        background = theme.color.controlHot;
    }

    HBRUSH brush = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, Scale(1), border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const int radius = Scale(4);
    RoundRect(dc, client.left, client.top, client.right, client.bottom, radius,
              radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t caption[128]{};
    GetWindowTextW(window, caption, static_cast<int>(std::size(caption)));

    HGDIOBJ oldFont = nullptr;
    SelectFont(dc, window, oldFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);

    RECT textRect = client;
    if (pressed) {
        OffsetRect(&textRect, Scale(1), Scale(1));
    }
    DrawTextW(dc, caption, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (GetFocus() == window && !disabled) {
        RECT focus = client;
        InflateRect(&focus, -Scale(4), -Scale(4));
        DrawFocusRect(dc, &focus);
    }

    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
}

void DrawCheckGlyph(HDC dc, const RECT& box, bool checked, COLORREF border,
                    COLORREF mark, const Theme& theme)
{
    HBRUSH fill = CreateSolidBrush(theme.color.checkBackground);
    FillRect(dc, &box, fill);
    DeleteObject(fill);

    HPEN borderPen = CreatePen(PS_SOLID, Scale(1), border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, borderPen);
    Rectangle(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(borderPen);

    if (!checked) {
        return;
    }

    HPEN checkPen = CreatePen(PS_SOLID, Scale(2), mark);
    oldPen = SelectObject(dc, checkPen);
    const int width = box.right - box.left;
    const int height = box.bottom - box.top;
    POINT points[3] = {
        {box.left + width * 22 / 100, box.top + height * 52 / 100},
        {box.left + width * 42 / 100, box.top + height * 72 / 100},
        {box.left + width * 78 / 100, box.top + height * 28 / 100},
    };
    Polyline(dc, points, static_cast<int>(std::size(points)));
    SelectObject(dc, oldPen);
    DeleteObject(checkPen);
}

void DrawCheckBox(HWND window, HDC dc, const Theme& theme)
{
    RECT client{};
    GetClientRect(window, &client);

    // The page panel clips children, so the checkbox has to erase its own
    // background or the label text sits on stale pixels.
    FillRect(dc, &client, theme.backgroundBrush);

    const bool disabled = !IsWindowEnabled(window);
    const bool checked =
        SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool hot = IsHot(window) && !disabled;

    const int box = Scale(15);
    const int left = client.left + Scale(1);
    const int top = client.top + ((client.bottom - client.top) - box) / 2;
    RECT boxRect{left, top, left + box, top + box};

    DrawCheckGlyph(
        dc, boxRect, checked,
        disabled ? theme.color.disabledText
                 : (hot ? theme.color.checkMark : theme.color.checkBorder),
        disabled ? theme.color.disabledText : theme.color.checkMark, theme);

    wchar_t caption[256]{};
    GetWindowTextW(window, caption, static_cast<int>(std::size(caption)));

    RECT textRect{boxRect.right + Scale(8), client.top, client.right,
                  client.bottom};
    HGDIOBJ oldFont = nullptr;
    SelectFont(dc, window, oldFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? theme.color.disabledText : theme.color.text);
    DrawTextW(dc, caption, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (GetFocus() == window && !disabled) {
        RECT focus = textRect;
        InflateRect(&focus, -Scale(2), -Scale(2));
        DrawFocusRect(dc, &focus);
    }

    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
}

void DrawTabs(HWND window, HDC dc, const Theme& theme)
{
    RECT client{};
    GetClientRect(window, &client);

    HBRUSH strip = CreateSolidBrush(theme.color.tabBackground);
    FillRect(dc, &client, strip);
    DeleteObject(strip);

    RECT content = client;
    TabCtrl_AdjustRect(window, FALSE, &content);
    FillRect(dc, &content, theme.backgroundBrush);

    RECT border{content.left - 1, content.top - 1, content.right + 1,
                content.bottom + 1};
    HBRUSH frame = CreateSolidBrush(theme.color.frame);
    FrameRect(dc, &border, frame);
    DeleteObject(frame);

    HGDIOBJ oldFont = nullptr;
    SelectFont(dc, window, oldFont);
    SetBkMode(dc, TRANSPARENT);

    const int count = TabCtrl_GetItemCount(window);
    const int selected = TabCtrl_GetCurSel(window);

    for (int i = 0; i < count; ++i) {
        RECT item{};
        if (!TabCtrl_GetItemRect(window, i, &item)) {
            continue;
        }

        wchar_t caption[64]{};
        TCITEMW info{};
        info.mask = TCIF_TEXT;
        info.pszText = caption;
        info.cchTextMax = static_cast<int>(std::size(caption));
        if (!TabCtrl_GetItem(window, i, &info)) {
            continue;
        }

        TCITEMW state{};
        state.mask = TCIF_PARAM;
        TabCtrl_GetItem(window, i, &state);
        const bool enabled = state.lParam != 0;
        const bool isSelected = i == selected;

        HBRUSH itemBrush = CreateSolidBrush(
            isSelected ? theme.color.background : theme.color.tabBackground);
        FillRect(dc, &item, itemBrush);
        DeleteObject(itemBrush);

        HPEN pen = CreatePen(PS_SOLID, Scale(1), theme.color.frame);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, item.left, item.top, nullptr);
        LineTo(dc, item.right - 1, item.top);
        MoveToEx(dc, item.left, item.top, nullptr);
        LineTo(dc, item.left, item.bottom - 1);
        MoveToEx(dc, item.right - 1, item.top, nullptr);
        LineTo(dc, item.right - 1, item.bottom - 1);
        if (!isSelected) {
            MoveToEx(dc, item.left, item.bottom - 1, nullptr);
            LineTo(dc, item.right, item.bottom - 1);
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        SetTextColor(dc, isSelected ? theme.color.tabSelectedText
                                    : (enabled ? theme.color.tabText
                                               : theme.color.disabledText));
        DrawTextW(dc, caption, -1, &item,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (GetFocus() == window && isSelected) {
            RECT focus = item;
            InflateRect(&focus, -Scale(4), -Scale(3));
            DrawFocusRect(dc, &focus);
        }
    }

    if (oldFont != nullptr) {
        SelectObject(dc, oldFont);
    }
}

// Push buttons and checkboxes keep their normal control styles and behavior.
// Only the paint path is replaced, and only while the dark theme is active, so
// light mode keeps the native themed appearance.
LRESULT CALLBACK ControlProc(HWND window, UINT message, WPARAM wParam,
                             LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    const Theme& theme = GetTheme();
    if (!theme.dark) {
        // Drop any hover state left over from dark mode so switching back to
        // the native theme does not start with a stale hot control.
        if (IsHot(window)) {
            SetHot(window, false);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        const LONG style = GetWindowLongW(window, GWL_STYLE);
        const LONG type = style & BS_TYPEMASK;
        if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX ||
            type == BS_AUTO3STATE || type == BS_3STATE) {
            DrawCheckBox(window, dc, theme);
        } else {
            DrawPushButton(window, dc, theme);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!IsHot(window)) {
            SetHot(window, true);
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        SetHot(window, false);
        InvalidateRect(window, nullptr, FALSE);
        break;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(window, nullptr, FALSE);
        break;
    default:
        break;
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK TabProc(HWND window, UINT message, WPARAM wParam,
                         LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    const Theme& theme = GetTheme();
    if (!theme.dark) {
        return DefSubclassProc(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        DrawTabs(window, dc, theme);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(window, nullptr, FALSE);
        break;
    default:
        break;
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

constexpr COLORREF kStateImageKey = RGB(255, 0, 255);

// The list view draws its own checkboxes from a state image list. Replace the
// native glyphs with the same glyph the regular owner-drawn checkboxes use so
// the two kinds look identical in dark mode.
//
// The glyphs are replaced inside the control's own list rather than installing
// a list of our own. Applying a window theme makes comctl32 recreate (and
// destroy) the state image list, so a cached handle goes stale; the allocator
// can then hand that same address to the replacement, and destroying the "old"
// list would destroy the new one. That is what made the device list lose its
// checkboxes after the theme was applied.
bool ReplaceCheckStateImages(HWND list, const Theme& theme)
{
    HIMAGELIST images = ListView_GetImageList(list, LVSIL_STATE);
    if (images == nullptr) {
        return false;
    }

    int size = 0;
    int iconHeight = 0;
    if (!ImageList_GetIconSize(images, &size, &iconHeight) || size <= 0) {
        size = Scale(16);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    bool replaced = true;
    for (int image = 0; image < 2; ++image) {
        void* bits = nullptr;
        HBITMAP bitmap =
            CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr) {
            replaced = false;
            break;
        }
        HGDIOBJ old = SelectObject(dc, bitmap);

        RECT rect{0, 0, size, size};
        HBRUSH key = CreateSolidBrush(kStateImageKey);
        FillRect(dc, &rect, key);
        DeleteObject(key);

        // The list view maps check state index 1 (unchecked) to image 0 and
        // index 2 (checked) to image 1. A blank image 0 would hide the
        // unchecked boxes and make checked rows look empty.
        //
        // Draw the box at the image list's real icon size. The native state
        // images are smaller than Scale(15) at 96 DPI (13x13), so centring a
        // larger box would push its border outside the bitmap and leave only
        // the dark fill visible.
        const int box = size;
        const int offset = 0;
        RECT boxRect{offset, offset, offset + box, offset + box};
        DrawCheckGlyph(dc, boxRect, image == 1, theme.color.checkBorder,
                       theme.color.checkMark, theme);

        SelectObject(dc, old);
        if (ImageList_Replace(images, image, bitmap, nullptr) == 0) {
            replaced = false;
        }
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return replaced;
}

LRESULT CALLBACK ListViewProc(HWND window, UINT message, WPARAM wParam,
                              LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    const Theme& theme = GetTheme();
    if (theme.dark && message == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, theme.listBrush);
        return 1;
    }

    // The header sends its custom-draw notifications to the list view, which
    // is this window. Intercept them so the column captions follow the theme
    // instead of staying system-black in dark mode.
    if (message == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header != nullptr &&
            header->hwndFrom == ListView_GetHeader(window) &&
            header->code == NM_CUSTOMDRAW) {
            if (theme.dark) {
                auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
                if (draw->dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                    // NMCUSTOMDRAW has no colour fields, so paint the item
                    // ourselves. This keeps the header dark and its captions
                    // readable regardless of the system header theme.
                    FillRect(draw->hdc, &draw->rc, theme.listHeaderBrush);

                    wchar_t caption[128]{};
                    HDITEMW item{};
                    item.mask = HDI_TEXT;
                    item.pszText = caption;
                    item.cchTextMax = static_cast<int>(std::size(caption));
                    Header_GetItem(header->hwndFrom,
                                   static_cast<int>(draw->dwItemSpec), &item);

                    HFONT font = reinterpret_cast<HFONT>(SendMessageW(
                        header->hwndFrom, WM_GETFONT, 0, 0));
                    HGDIOBJ oldFont = nullptr;
                    if (font != nullptr) {
                        oldFont = SelectObject(draw->hdc, font);
                    }
                    SetBkMode(draw->hdc, TRANSPARENT);
                    SetTextColor(draw->hdc, theme.color.text);

                    RECT text = draw->rc;
                    text.left += Scale(6);
                    text.right -= Scale(4);
                    DrawTextW(draw->hdc, caption, -1, &text,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                  DT_END_ELLIPSIS);
                    if (oldFont != nullptr) {
                        SelectObject(draw->hdc, oldFont);
                    }

                    HPEN pen =
                        CreatePen(PS_SOLID, Scale(1), theme.color.frame);
                    HGDIOBJ oldPen = SelectObject(draw->hdc, pen);
                    MoveToEx(draw->hdc, draw->rc.right - 1, draw->rc.top,
                             nullptr);
                    LineTo(draw->hdc, draw->rc.right - 1, draw->rc.bottom);
                    MoveToEx(draw->hdc, draw->rc.left, draw->rc.bottom - 1,
                             nullptr);
                    LineTo(draw->hdc, draw->rc.right, draw->rc.bottom - 1);
                    SelectObject(draw->hdc, oldPen);
                    DeleteObject(pen);
                    return CDRF_SKIPDEFAULT;
                }
            }
        }
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

void ApplyListStateImages(HWND list)
{
    if (list == nullptr) {
        return;
    }

    const Theme& theme = GetTheme();

    // Set the list colours explicitly. Without this the control paints its
    // default white background for a frame whenever it is repainted.
    if (theme.dark) {
        ListView_SetBkColor(list, theme.color.listBackground);
        ListView_SetTextBkColor(list, theme.color.listBackground);
        ListView_SetTextColor(list, theme.color.listText);
    } else {
        ListView_SetBkColor(list, GetSysColor(COLOR_WINDOW));
        ListView_SetTextBkColor(list, GetSysColor(COLOR_WINDOW));
        ListView_SetTextColor(list, GetSysColor(COLOR_WINDOWTEXT));
    }

    if ((ListView_GetExtendedListViewStyle(list) & LVS_EX_CHECKBOXES) == 0) {
        return;
    }

    // Light mode keeps the native glyphs: applying the window theme makes
    // comctl32 rebuild the state image list with them.
    if (theme.dark) {
        ReplaceCheckStateImages(list, theme);
    }
}

int Scale(int value)
{
    return MulDiv(value, g_dpi, 96);
}
void ApplyFont(HWND control, HFONT font)
{
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void CreateFonts()
{
    g_font = CreateFontW(
        -MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_boldFont = CreateFontW(
        -MulDiv(9, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_titleFont = CreateFontW(
        -MulDiv(14, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void DestroyFonts()
{
    if (g_font != nullptr) {
        DeleteObject(g_font);
        g_font = nullptr;
    }
    if (g_boldFont != nullptr) {
        DeleteObject(g_boldFont);
        g_boldFont = nullptr;
    }
    if (g_titleFont != nullptr) {
        DeleteObject(g_titleFont);
        g_titleFont = nullptr;
    }
}

HWND Label(HWND parent, int id, const wchar_t* text, DWORD style)
{
    HWND control = CreateWindowExW(
        0, WC_STATICW, text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND Button(HWND parent, int id, const wchar_t* text)
{
    HWND control = CreateWindowExW(
        0, WC_BUTTONW, text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_PUSHBUTTON,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyFont(control, g_font);
    SetWindowSubclass(control, ControlProc, 2, 0);
    return control;
}

HWND CheckBox(HWND parent, int id, const wchar_t* text)
{
    HWND control = CreateWindowExW(
        0, WC_BUTTONW, text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_AUTOCHECKBOX,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyFont(control, g_font);
    SetWindowSubclass(control, ControlProc, 2, 0);
    return control;
}

HWND NumberField(HWND parent, int id)
{
    HWND control = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | ES_AUTOHSCROLL |
            ES_RIGHT,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND GroupFrame(HWND parent, int id, const wchar_t* text)
{
    HWND control = CreateWindowExW(
        0, WC_BUTTONW, text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_GROUPBOX, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance,
        nullptr);
    ApplyFont(control, g_font);
    SetWindowSubclass(control, GroupFrameProc, 1, 0);
    return control;
}

HWND ComboBox(HWND parent, int id, DWORD style)
{
    HWND control = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | style, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance,
        nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND ReportList(HWND parent, int id, DWORD style)
{
    HWND control = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT |
            style,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyFont(control, g_font);
    HWND header = ListView_GetHeader(control);
    if (header != nullptr) {
        ApplyFont(header, g_font);
    }
    SetWindowSubclass(control, ListViewProc, 4, 0);
    return control;
}

HWND TabControl(HWND parent, int id)
{
    HWND control = CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | TCS_HOTTRACK,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyFont(control, g_font);
    SetWindowSubclass(control, TabProc, 3, 0);
    return control;
}

std::wstring FormatNumber(double value)
{
    wchar_t text[32]{};
    swprintf_s(text, std::size(text), L"%g", value);
    return text;
}

void SetNumber(HWND field, double value)
{
    SetWindowTextW(field, FormatNumber(value).c_str());
}

bool GetNumber(HWND field, double& value)
{
    wchar_t text[64]{};
    GetWindowTextW(field, text, static_cast<int>(std::size(text)));

    wchar_t* end = nullptr;
    const double parsed = std::wcstod(text, &end);
    if (end == text) {
        return false;
    }
    while (*end == L' ' || *end == L'\t') {
        ++end;
    }
    if (*end != L'\0' || !std::isfinite(parsed)) {
        return false;
    }

    value = parsed;
    return true;
}

bool IsNumberFieldChange(WORD notification)
{
    return notification == EN_CHANGE;
}

} // namespace ui
