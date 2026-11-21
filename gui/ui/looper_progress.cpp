#include "looper_progress.h"

#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include <algorithm>
#include <array>

namespace ui {
namespace {

constexpr wchar_t kLooperProgressClass[] =
    L"BetterMicroApoLooperProgress";

// Fixed recording / overdub / playback colors match the other live overlays in
// the graph. Stopped and Empty rows are intentionally unfilled.
constexpr COLORREF kRecordingColor = RGB(225, 60, 60);
constexpr COLORREF kOverdubColor = RGB(235, 170, 45);
constexpr COLORREF kPlaybackColor = RGB(72, 190, 110);

struct LooperProgressState
{
    std::array<LooperProgressTrack, apo::kLooperTrackCount> tracks{};
    HFONT font = nullptr;
};

void FillColor(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FrameColor(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

COLORREF ModeColor(const LooperProgressTrack& track, bool enabled)
{
    if (!enabled) {
        return GetTheme().color.disabledText;
    }
    switch (track.state) {
    case apo::LooperState::Recording:
        return track.overdub ? kOverdubColor : kRecordingColor;
    case apo::LooperState::Playback:
        return kPlaybackColor;
    case apo::LooperState::Empty:
    case apo::LooperState::Stopped:
    default:
        return GetTheme().color.disabledText;
    }
}

void PaintProgress(HWND window, const LooperProgressState& state, HDC dc)
{
    RECT client{};
    GetClientRect(window, &client);

    const Theme& theme = GetTheme();
    FillRect(dc, &client, theme.backgroundBrush);
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const int rowStep = Scale(44);
    const int barHeight = std::max(Scale(5), 1);
    const int barTopInRow = Scale(36);
    const int margin = Scale(2);
    const int minWidth = Scale(28);
    const int available =
        std::max(1, static_cast<int>(client.right - client.left) - 2 * margin);

    ULONG longest = 0;
    for (const LooperProgressTrack& track : state.tracks) {
        if (track.state != apo::LooperState::Empty && track.length > 0) {
            longest = std::max(longest, track.length);
        }
    }

    for (int track = 0; track < apo::kLooperTrackCount; ++track) {
        const int rowTop = track * rowStep;
        const int top = rowTop + barTopInRow;
        if (top >= client.bottom) {
            break;
        }
        const int bottom = std::min(top + barHeight, static_cast<int>(client.bottom));
        if (bottom <= top) {
            continue;
        }

        const LooperProgressTrack& value = state.tracks[static_cast<size_t>(track)];
        int width = minWidth;
        if (value.state != apo::LooperState::Empty && value.length > 0 &&
            longest > 0)
        {
            width = static_cast<int>(
                (static_cast<double>(available) * value.length) / longest);
            width = std::max(width, minWidth);
        }
        width = std::min(width, available);

        RECT bar{margin, top, margin + width, bottom};
        FillColor(dc, bar, theme.color.controlBackground);
        const COLORREF modeColor = ModeColor(value, enabled);
        FrameColor(dc, bar, modeColor);

        const bool active = value.state == apo::LooperState::Playback ||
                            value.state == apo::LooperState::Recording;
        if (!active || value.length == 0 || !enabled) {
            continue;
        }

        const double fraction = std::clamp(
            static_cast<double>(value.position) / value.length, 0.0, 1.0);
        const int inset = 1;
        const int innerWidth = std::max(0, width - 2 * inset);
        int fillWidth = static_cast<int>(innerWidth * fraction);
        if (fillWidth <= 0) {
            continue;
        }
        fillWidth = std::min(fillWidth, innerWidth);
        RECT fill{bar.left + inset, bar.top + inset,
                  bar.left + inset + fillWidth, bar.bottom - inset};
        if (fill.bottom > fill.top) {
            FillColor(dc, fill, modeColor);
        }
    }
}

LRESULT CALLBACK LooperProgressProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam)
{
    auto* state = reinterpret_cast<LooperProgressState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<LooperProgressState*>(create->lpCreateParams);
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
        PaintProgress(window, *state, dc);
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
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void EnsureLooperProgressClass()
{
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = LooperProgressProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kLooperProgressClass;
    RegisterClassExW(&windowClass);
    registered = true;
}

LooperProgressState* StateOf(HWND progress)
{
    return reinterpret_cast<LooperProgressState*>(
        GetWindowLongPtrW(progress, GWLP_USERDATA));
}

} // namespace

HWND LooperProgress(HWND parent, int id)
{
    EnsureLooperProgressClass();

    auto* state = new LooperProgressState;
    HWND control = CreateWindowExW(
        0, kLooperProgressClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, state);
    if (control == nullptr) {
        delete state;
        return nullptr;
    }
    ApplyFont(control, g_font);
    return control;
}

void SetLooperProgressTrack(HWND progress, int track,
                            const LooperProgressTrack& value)
{
    if (progress == nullptr || track < 0 ||
        track >= apo::kLooperTrackCount)
    {
        return;
    }
    LooperProgressState* state = StateOf(progress);
    if (state == nullptr)
    {
        return;
    }
    state->tracks[static_cast<size_t>(track)] = value;
    InvalidateRect(progress, nullptr, FALSE);
}

void ClearLooperProgress(HWND progress)
{
    if (progress == nullptr) {
        return;
    }
    LooperProgressState* state = StateOf(progress);
    if (state == nullptr) {
        return;
    }
    state->tracks = {};
    InvalidateRect(progress, nullptr, FALSE);
}

} // namespace ui
