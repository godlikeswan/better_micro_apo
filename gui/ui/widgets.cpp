#include "widgets.h"

#include "theme.h"
#include "ui_state.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <limits>

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

void SelectControlFont(HDC dc, HWND window, HGDIOBJ& oldFont)
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
    SelectControlFont(dc, window, oldFont);
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
    SelectControlFont(dc, window, oldFont);
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
    SelectControlFont(dc, window, oldFont);
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

// ---------------------------------------------------------------------------
// Slider
// ---------------------------------------------------------------------------

constexpr wchar_t kSliderClass[] = L"BetterMicroApoSlider";
constexpr WORD kSliderChange = 0x0700;

struct SliderState {
    double min = 0.0;
    double max = 1.0;
    double step = 1.0;
    bool logarithmic = false;
    int decimals = 0;
    bool infiniteAtMax = false;
    double value = 0.0;
    HFONT font = nullptr;
    bool tracking = false;
};

double ClampValue(double value, double low, double high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

double SnapSliderValue(const SliderState& state, double value)
{
    if (state.infiniteAtMax && std::isinf(value) && value > 0.0) {
        return value;
    }
    if (std::isinf(value)) {
        // A non-infinite slider never accepts infinity; fall back to its end.
        // On an infinite slider a finite value at or above max is still the
        // finite end stop; infinity is selected only at the right edge.
        value = (value > 0.0) ? state.max : state.min;
    }

    if (state.logarithmic) {
        // Three significant digits keep the whole log range adjustable.
        if (value != 0.0) {
            const double magnitude = std::pow(
                10.0, std::floor(std::log10(std::fabs(value))) - 2.0);
            value = std::round(value / magnitude) * magnitude;
        }
    } else if (state.step > 0.0) {
        value = std::round(value / state.step) * state.step;
    }
    if (value == 0.0) {
        // Rounding can produce negative zero, which would read as "-0".
        value = 0.0;
    }
    return ClampValue(value, state.min, state.max);
}

double SliderNormalized(const SliderState& state, double value)
{
    if (state.max <= state.min) {
        return 0.0;
    }
    if (state.infiniteAtMax && std::isinf(value) && value > 0.0) {
        return 1.0;
    }
    value = ClampValue(value, state.min, state.max);
    if (state.logarithmic && state.min > 0.0) {
        return std::log(value / state.min) /
               std::log(state.max / state.min);
    }
    return (value - state.min) / (state.max - state.min);
}

double SliderValueAt(const SliderState& state, double normalized)
{
    normalized = ClampValue(normalized, 0.0, 1.0);
    double value = state.min;
    if (state.logarithmic && state.min > 0.0) {
        value = state.min * std::pow(state.max / state.min, normalized);
    } else {
        value = state.min + normalized * (state.max - state.min);
    }
    return SnapSliderValue(state, value);
}

std::wstring SliderText(const SliderState& state)
{
    if (state.infiniteAtMax && std::isinf(state.value) && state.value > 0.0) {
        return L"\u221E";
    }
    wchar_t text[48]{};
    if (state.logarithmic) {
        swprintf_s(text, std::size(text), L"%g", state.value);
    } else {
        swprintf_s(text, std::size(text), L"%.*f", state.decimals,
                   state.value);
    }
    return text;
}

void SliderTrackRect(HWND window, RECT& track)
{
    RECT client{};
    GetClientRect(window, &client);
    const int valueWidth = Scale(52);
    track.left = client.left + Scale(7);
    track.right = client.right - valueWidth - Scale(6);
    if (track.right < track.left + Scale(24)) {
        track.right = client.right - Scale(6);
    }
    track.top = client.top;
    track.bottom = client.bottom;
}

void SliderNotify(HWND window)
{
    SendMessageW(GetParent(window), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(window), kSliderChange),
                 reinterpret_cast<LPARAM>(window));
}

void SliderApply(HWND window, SliderState& state, double value, bool notify)
{
    const double snapped = SnapSliderValue(state, value);
    if (snapped == state.value) {
        return;
    }
    state.value = snapped;
    InvalidateRect(window, nullptr, FALSE);
    if (notify) {
        SliderNotify(window);
    }
}

void SliderSetFromPoint(HWND window, SliderState& state, int x)
{
    RECT track{};
    SliderTrackRect(window, track);
    const int span = std::max(static_cast<int>(track.right - track.left), 1);
    if (state.infiniteAtMax && x >= track.right) {
        // The right edge is a separate end stop for infinity. The pixel just
        // before it is the last finite position (state.max).
        SliderApply(window, state, std::numeric_limits<double>::infinity(),
                    true);
        return;
    }

    // Reserve one pixel for the infinity end stop, so the finite scale ends
    // at exactly max one pixel before the right edge.
    const int finiteSpan = state.infiniteAtMax ? std::max(span - 1, 1) : span;
    const int offset =
        std::clamp(static_cast<int>(x - track.left), 0, finiteSpan);
    const double normalized =
        static_cast<double>(offset) / static_cast<double>(finiteSpan);
    SliderApply(window, state, SliderValueAt(state, normalized), true);
}

void PaintSlider(HWND window, const SliderState& state, HDC dc)
{
    const Theme& theme = GetTheme();

    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, theme.backgroundBrush);

    const bool enabled = IsWindowEnabled(window) != FALSE;
    const COLORREF grooveColor =
        enabled ? theme.color.frame : theme.color.disabledText;
    const COLORREF accent =
        enabled ? theme.color.accent : theme.color.disabledText;
    const COLORREF textColor =
        enabled ? theme.color.text : theme.color.disabledText;

    RECT track{};
    SliderTrackRect(window, track);
    const int centerY = (client.top + client.bottom) / 2;
    const int grooveHeight = std::max(Scale(4), 2);
    const int thumbRadius = Scale(6);

    RECT groove{track.left, centerY - grooveHeight / 2, track.right,
                centerY - grooveHeight / 2 + grooveHeight};
    HBRUSH brush = CreateSolidBrush(grooveColor);
    FillRect(dc, &groove, brush);
    DeleteObject(brush);

    const double normalized =
        ClampValue(SliderNormalized(state, state.value), 0.0, 1.0);
    const int thumbX =
        track.left + static_cast<int>(normalized *
                                      static_cast<double>(track.right -
                                                          track.left));

    if (thumbX > track.left) {
        RECT filled{groove.left, groove.top, thumbX, groove.bottom};
        brush = CreateSolidBrush(accent);
        FillRect(dc, &filled, brush);
        DeleteObject(brush);
    }

    brush = CreateSolidBrush(accent);
    HPEN thumbPen = CreatePen(PS_SOLID, Scale(1), theme.color.background);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, thumbPen);
    Ellipse(dc, thumbX - thumbRadius, centerY - thumbRadius,
            thumbX + thumbRadius + 1, centerY + thumbRadius + 1);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(thumbPen);
    DeleteObject(brush);

    HFONT font = state.font;
    if (font == nullptr) {
        font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor);
    const std::wstring valueText = SliderText(state);
    RECT valueRect{track.right + Scale(6), client.top, client.right,
                   client.bottom};
    DrawTextW(dc, valueText.c_str(), -1, &valueRect,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);

    if (GetFocus() == window) {
        RECT focus{track.left - Scale(2), centerY - Scale(10),
                   track.right + Scale(2), centerY + Scale(10)};
        HPEN focusPen = CreatePen(PS_SOLID, Scale(1), theme.color.accent);
        oldPen = SelectObject(dc, focusPen);
        oldBrush =
            SelectObject(dc, static_cast<HBRUSH>(GetStockObject(NULL_BRUSH)));
        Rectangle(dc, focus.left, focus.top, focus.right, focus.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(focusPen);
    }
}

LRESULT CALLBACK SliderProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam)
{
    auto* state = reinterpret_cast<SliderState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<SliderState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete state;
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintSlider(window, *state, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SETFONT:
        state->font = reinterpret_cast<HFONT>(wParam);
        if (LOWORD(lParam) != 0) {
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->font);
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        SetCapture(window);
        state->tracking = true;
        SliderSetFromPoint(window, *state, GET_X_LPARAM(lParam));
        return 0;
    case WM_MOUSEMOVE:
        if (state->tracking) {
            SliderSetFromPoint(window, *state, GET_X_LPARAM(lParam));
        }
        return 0;
    case WM_LBUTTONUP:
        if (state->tracking) {
            state->tracking = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        state->tracking = false;
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const double factor =
            state->logarithmic ? 1.05 : state->step;
        if (state->infiniteAtMax) {
            const bool positiveInfinity =
                std::isinf(state->value) && state->value > 0.0;
            if (positiveInfinity) {
                if (delta < 0) {
                    SliderApply(window, *state, state->max, true);
                }
                return 0;
            }
            if (delta > 0 && state->value >= state->max) {
                SliderApply(window, *state,
                            std::numeric_limits<double>::infinity(), true);
                return 0;
            }
        }
        double value = state->value;
        if (state->logarithmic) {
            value = delta > 0 ? value * factor : value / factor;
        } else {
            value += delta > 0 ? factor : -factor;
        }
        SliderApply(window, *state, value, true);
        return 0;
    }
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void EnsureSliderClass()
{
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = SliderProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kSliderClass;
    RegisterClassExW(&windowClass);
    registered = true;
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

HWND IconButton(HWND parent, int id, const wchar_t* glyph)
{
    // BS_OWNERDRAW keeps the native button behavior (including BN_CLICKED)
    // while leaving the paint entirely to DrawIconButton, so there is no
    // border or background in either theme.
    HWND control = CreateWindowExW(
        0, WC_BUTTONW, glyph,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

bool DrawIconButton(DRAWITEMSTRUCT* item)
{
    if (item == nullptr || item->CtlType != ODT_BUTTON ||
        item->hwndItem == nullptr) {
        return false;
    }

    wchar_t glyph[8]{};
    GetWindowTextW(item->hwndItem, glyph, static_cast<int>(std::size(glyph)));
    if (glyph[0] == L'\0') {
        return true;
    }

    const Theme& theme = GetTheme();
    const bool disabled =
        (item->itemState & ODS_DISABLED) != 0 || !IsWindowEnabled(item->hwndItem);
    const bool active =
        (item->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    const COLORREF color = disabled ? theme.color.disabledText
                                    : (active ? theme.color.accent
                                              : theme.color.text);

    HGDIOBJ oldFont = nullptr;
    HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    if (font != nullptr) {
        oldFont = SelectObject(item->hDC, font);
    }
    const int oldBkMode = SetBkMode(item->hDC, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(item->hDC, color);
    DrawTextW(item->hDC, glyph, -1, &item->rcItem,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(item->hDC, oldColor);
    SetBkMode(item->hDC, oldBkMode);
    if (oldFont != nullptr) {
        SelectObject(item->hDC, oldFont);
    }
    return true;
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

HWND Slider(HWND parent, int id, const SliderSpec& spec)
{
    EnsureSliderClass();

    auto* state = new SliderState;
    state->min = spec.min;
    state->max = spec.max;
    state->step = spec.step;
    state->logarithmic = spec.logarithmic;
    state->decimals = spec.decimals;
    state->infiniteAtMax = spec.infiniteAtMax;
    state->value = SnapSliderValue(*state, spec.min);

    HWND control = CreateWindowExW(
        0, kSliderClass, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance,
        state);
    if (control == nullptr) {
        delete state;
        return nullptr;
    }
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

void SetSliderValue(HWND slider, double value)
{
    auto* state = reinterpret_cast<SliderState*>(
        GetWindowLongPtrW(slider, GWLP_USERDATA));
    if (state == nullptr) {
        return;
    }

    const double snapped = SnapSliderValue(*state, value);
    if (snapped == state->value) {
        return;
    }
    state->value = snapped;
    InvalidateRect(slider, nullptr, FALSE);
}

bool GetSliderValue(HWND slider, double& value)
{
    auto* state = reinterpret_cast<SliderState*>(
        GetWindowLongPtrW(slider, GWLP_USERDATA));
    if (state == nullptr) {
        return false;
    }
    value = state->value;
    return true;
}

bool IsSliderChange(WORD notification)
{
    return notification == kSliderChange;
}

} // namespace ui
