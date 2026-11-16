#include "graph.h"

#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include "dsp/transfer.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

constexpr wchar_t kGraphClass[] = L"BetterMicroApoGraph";
constexpr int kCurveSamples = 240;

struct GraphState
{
    apo::EffectInstance effect;
    bool hasEffect = false;
    HFONT font = nullptr;
};

struct GraphColors
{
    COLORREF grid;
    COLORREF reference;
    COLORREF curve;
    COLORREF text;
};

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color,
              int style = PS_SOLID)
{
    HPEN pen = CreatePen(style, 1, color);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void DrawGrid(HDC dc, const RECT& plot, COLORREF color, int columns, int rows)
{
    for (int i = 1; i < columns; ++i) {
        const int x = plot.left + (plot.right - plot.left) * i / columns;
        DrawLine(dc, x, plot.top, x, plot.bottom, color);
    }
    for (int i = 1; i < rows; ++i) {
        const int y = plot.top + (plot.bottom - plot.top) * i / rows;
        DrawLine(dc, plot.left, y, plot.right, y, color);
    }
}

int MapX(const RECT& plot, double normalized)
{
    normalized = std::clamp(normalized, 0.0, 1.0);
    return plot.left + static_cast<int>(
                           normalized * (plot.right - plot.left));
}

int MapY(const RECT& plot, double normalized)
{
    normalized = std::clamp(normalized, 0.0, 1.0);
    return plot.bottom - static_cast<int>(
                             normalized * (plot.bottom - plot.top));
}

// Draws a curve through `kCurveSamples` points. `valueAt` receives the
// horizontal position in [0, 1] and returns the vertical position in [0, 1],
// with 0 at the bottom.
template <typename ValueAt>
void StrokeCurve(HDC dc, const RECT& plot, COLORREF color, ValueAt valueAt)
{
    POINT points[kCurveSamples];
    const int width = plot.right - plot.left;
    const int height = plot.bottom - plot.top;
    for (int i = 0; i < kCurveSamples; ++i) {
        const double t = static_cast<double>(i) / (kCurveSamples - 1);
        const double value = std::clamp(valueAt(t), 0.0, 1.0);
        points[i].x = plot.left + static_cast<LONG>(t * width);
        points[i].y = plot.bottom - static_cast<LONG>(value * height);
    }

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, plot.left, plot.top, plot.right, plot.bottom);
    HPEN pen = CreatePen(PS_SOLID, std::max(2, Scale(2)), color);
    HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, points, kCurveSamples);
    SelectObject(dc, old);
    DeleteObject(pen);
    RestoreDC(dc, saved);
}

void DrawDot(HDC dc, int x, int y, COLORREF color)
{
    const int radius = Scale(3);
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, x - radius, y - radius, x + radius + 1, y + radius + 1);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawCaption(HDC dc, const RECT& client, const RECT& plot,
                 const wchar_t* text, COLORREF color)
{
    RECT rect{plot.left, client.top + Scale(2), client.right,
              client.top + Scale(16)};
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
}

void DrawXLeft(HDC dc, const RECT& plot, const wchar_t* text, COLORREF color)
{
    RECT rect{plot.left + Scale(6), plot.bottom + Scale(2),
              plot.left + Scale(62), plot.bottom + Scale(16)};
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawXCenter(HDC dc, const RECT& plot, int x, const wchar_t* text,
                 COLORREF color)
{
    RECT rect{x - Scale(28), plot.bottom + Scale(2), x + Scale(28),
              plot.bottom + Scale(16)};
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect,
              DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawXRight(HDC dc, const RECT& plot, const wchar_t* text, COLORREF color)
{
    RECT rect{plot.right - Scale(62), plot.bottom + Scale(2),
              plot.right - Scale(6), plot.bottom + Scale(16)};
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect,
              DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawYLabel(HDC dc, int left, const RECT& plot, int y,
                const wchar_t* text, COLORREF color)
{
    const int center = std::clamp(
        y, static_cast<int>(plot.top) + Scale(7),
        static_cast<int>(plot.bottom) - Scale(7));
    RECT rect{left, center - Scale(7), plot.left - Scale(4),
              center + Scale(7)};
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void PaintDistortion(HDC dc, const RECT& client, const RECT& plot,
                     const apo::DistortionSettings& settings,
                     const GraphColors& colors)
{
    DrawGrid(dc, plot, colors.grid, 4, 4);
    DrawLine(dc, plot.left, plot.bottom - 1, plot.right - 1, plot.top,
             colors.reference, PS_DOT);

    const apo::dsp::DistortionCurve curve =
        apo::dsp::MakeDistortionCurve(settings.function, settings.curvature);
    const double drive = apo::dsp::DecibelsToLinear(settings.driveDb);
    const double output = apo::dsp::DecibelsToLinear(settings.outputDb);
    StrokeCurve(dc, plot, colors.curve, [&](double t) {
        const double x = -1.0 + 2.0 * t;
        const double y =
            apo::dsp::ShapeDistortion(curve, settings.function, x * drive) *
            output;
        return (std::clamp(y, -1.0, 1.0) + 1.0) * 0.5;
    });

    DrawCaption(dc, client, plot, L"Transfer curve", colors.text);
    DrawXLeft(dc, plot, L"-1", colors.text);
    DrawXCenter(dc, plot, (plot.left + plot.right) / 2, L"0", colors.text);
    DrawXRight(dc, plot, L"1", colors.text);
    DrawYLabel(dc, client.left, plot, plot.top, L"1", colors.text);
    DrawYLabel(dc, client.left, plot, (plot.top + plot.bottom) / 2, L"0",
               colors.text);
    DrawYLabel(dc, client.left, plot, plot.bottom, L"-1", colors.text);
}

void PaintCompressor(HDC dc, const RECT& client, const RECT& plot,
                     const apo::CompressorSettings& settings,
                     const GraphColors& colors)
{
    DrawGrid(dc, plot, colors.grid, 4, 4);
    DrawLine(dc, plot.left, plot.bottom - 1, plot.right - 1, plot.top,
             colors.reference, PS_DOT);

    const double threshold = std::clamp(settings.thresholdDb, -60.0, 0.0);
    const double ratio = std::max(settings.ratio, 1.0);
    const int thresholdX = MapX(plot, (threshold + 60.0) / 60.0);
    DrawLine(dc, thresholdX, plot.top, thresholdX, plot.bottom,
             colors.reference, PS_DOT);

    StrokeCurve(dc, plot, colors.curve, [&](double t) {
        const double inputDb = -60.0 + 60.0 * t;
        const double outputDb =
            apo::dsp::CompressorTransferDb(inputDb, threshold, ratio);
        return (outputDb + 60.0) / 60.0;
    });
    DrawDot(dc, thresholdX, MapY(plot, (threshold + 60.0) / 60.0),
            colors.curve);

    DrawCaption(dc, client, plot, L"Transfer curve (dBFS)", colors.text);
    DrawXLeft(dc, plot, L"-60", colors.text);
    DrawXCenter(dc, plot, (plot.left + plot.right) / 2, L"-30", colors.text);
    DrawXRight(dc, plot, L"0", colors.text);
    DrawYLabel(dc, client.left, plot, plot.top, L"0", colors.text);
    DrawYLabel(dc, client.left, plot, (plot.top + plot.bottom) / 2, L"-30",
               colors.text);
    DrawYLabel(dc, client.left, plot, plot.bottom, L"-60", colors.text);
}

void PaintGate(HDC dc, const RECT& client, const RECT& plot,
               const apo::GateSettings& settings, const GraphColors& colors)
{
    DrawGrid(dc, plot, colors.grid, 4, 4);
    DrawLine(dc, plot.left, plot.bottom - 1, plot.right - 1, plot.top,
             colors.reference, PS_DOT);

    const double threshold = std::clamp(settings.thresholdDb, -80.0, 0.0);
    const double range = std::max(settings.rangeDb, 0.0);
    const int thresholdX = MapX(plot, (threshold + 80.0) / 80.0);
    DrawLine(dc, thresholdX, plot.top, thresholdX, plot.bottom,
             colors.reference, PS_DOT);

    StrokeCurve(dc, plot, colors.curve, [&](double t) {
        const double inputDb = -80.0 + 80.0 * t;
        const double outputDb =
            apo::dsp::GateTransferDb(inputDb, threshold, range);
        return (outputDb + 80.0) / 80.0;
    });

    DrawCaption(dc, client, plot, L"Transfer curve (dBFS)", colors.text);
    DrawXLeft(dc, plot, L"-80", colors.text);
    DrawXCenter(dc, plot, (plot.left + plot.right) / 2, L"-40", colors.text);
    DrawXRight(dc, plot, L"0", colors.text);
    DrawYLabel(dc, client.left, plot, plot.top, L"0", colors.text);
    DrawYLabel(dc, client.left, plot, (plot.top + plot.bottom) / 2, L"-40",
               colors.text);
    DrawYLabel(dc, client.left, plot, plot.bottom, L"-80", colors.text);
}

void PaintDynamic(HDC dc, const RECT& client, const RECT& plot,
                  const apo::DynamicSettings& settings,
                  const GraphColors& colors)
{
    constexpr double kMinHz = 20.0;
    constexpr double kMaxHz = 20000.0;
    constexpr double kMinDb = -36.0;
    constexpr double kMaxDb = 6.0;
    const double logSpan = std::log10(kMaxHz / kMinHz);
    const double centerHz = std::clamp(settings.frequencyHz, kMinHz, kMaxHz);
    const double q = std::max(settings.q, 0.1);

    auto frequencyPosition = [&](double hz) {
        return std::log10(hz / kMinHz) / logSpan;
    };
    auto decibelPosition = [&](double db) {
        return (db - kMinDb) / (kMaxDb - kMinDb);
    };

    // Vertical grid at the usual decade marks; horizontal grid every 12 dB.
    const double gridHz[] = {100.0, 1000.0, 10000.0};
    for (double hz : gridHz) {
        const int x = MapX(plot, frequencyPosition(hz));
        DrawLine(dc, x, plot.top, x, plot.bottom, colors.grid);
    }
    const double gridDb[] = {0.0, -12.0, -24.0, -36.0};
    for (double db : gridDb) {
        const int y = MapY(plot, decibelPosition(db));
        DrawLine(dc, plot.left, y, plot.right, y, colors.grid);
    }

    DrawLine(dc, plot.left, MapY(plot, decibelPosition(0.0)), plot.right,
             MapY(plot, decibelPosition(0.0)), colors.reference, PS_DOT);
    const int centerX = MapX(plot, frequencyPosition(centerHz));
    DrawLine(dc, centerX, plot.top, centerX, plot.bottom, colors.reference,
             PS_DOT);

    StrokeCurve(dc, plot, colors.curve, [&](double t) {
        const double hz = kMinHz * std::pow(kMaxHz / kMinHz, t);
        const double db = apo::dsp::BandpassMagnitudeDb(hz, centerHz, q);
        return decibelPosition(db);
    });

    DrawCaption(dc, client, plot, L"Band response", colors.text);
    DrawXLeft(dc, plot, L"20", colors.text);
    DrawXCenter(dc, plot, MapX(plot, frequencyPosition(100.0)), L"100",
                colors.text);
    DrawXCenter(dc, plot, MapX(plot, frequencyPosition(1000.0)), L"1k",
                colors.text);
    DrawXRight(dc, plot, L"20k", colors.text);
    DrawYLabel(dc, client.left, plot, MapY(plot, decibelPosition(0.0)), L"0",
               colors.text);
    DrawYLabel(dc, client.left, plot, MapY(plot, decibelPosition(-12.0)),
               L"-12", colors.text);
    DrawYLabel(dc, client.left, plot, MapY(plot, decibelPosition(-24.0)),
               L"-24", colors.text);
    DrawYLabel(dc, client.left, plot, MapY(plot, decibelPosition(-36.0)),
               L"-36", colors.text);
}

void PaintGraph(HWND window, const GraphState& state, HDC dc)
{
    const Theme& theme = GetTheme();

    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, theme.backgroundBrush);

    if (!state.hasEffect) {
        return;
    }

    const int leftMargin = Scale(34);
    const int rightMargin = Scale(8);
    const int captionHeight = Scale(18);
    const int bottomMargin = Scale(16);
    RECT plot{client.left + leftMargin, client.top + captionHeight,
              client.right - rightMargin, client.bottom - bottomMargin};
    if (plot.right - plot.left < Scale(20) ||
        plot.bottom - plot.top < Scale(20)) {
        return;
    }

    FillRect(dc, &plot, theme.editBrush);
    HBRUSH frame = CreateSolidBrush(theme.color.frame);
    FrameRect(dc, &plot, frame);
    DeleteObject(frame);

    GraphColors colors{};
    colors.grid = theme.color.graphGrid;
    colors.reference = theme.color.frame;
    colors.curve =
        IsWindowEnabled(window) ? theme.color.accent : theme.color.disabledText;
    colors.text = theme.color.text;

    HFONT font = state.font != nullptr
                     ? state.font
                     : reinterpret_cast<HFONT>(
                           GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    switch (state.effect.type) {
    case apo::EffectType::Distortion:
        PaintDistortion(dc, client, plot, state.effect.distortion, colors);
        break;
    case apo::EffectType::Compressor:
        PaintCompressor(dc, client, plot, state.effect.compressor, colors);
        break;
    case apo::EffectType::Gate:
        PaintGate(dc, client, plot, state.effect.gate, colors);
        break;
    case apo::EffectType::Dynamic:
        PaintDynamic(dc, client, plot, state.effect.dynamics, colors);
        break;
    default:
        break;
    }

    SelectObject(dc, oldFont);
}

LRESULT CALLBACK GraphProc(HWND window, UINT message, WPARAM wParam,
                           LPARAM lParam)
{
    auto* state = reinterpret_cast<GraphState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<GraphState*>(create->lpCreateParams);
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
        PaintGraph(window, *state, dc);
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

void EnsureGraphClass()
{
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = GraphProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kGraphClass;
    RegisterClassExW(&windowClass);
    registered = true;
}

} // namespace

bool GraphSupported(apo::EffectType type)
{
    switch (type) {
    case apo::EffectType::Distortion:
    case apo::EffectType::Compressor:
    case apo::EffectType::Gate:
    case apo::EffectType::Dynamic:
        return true;
    default:
        return false;
    }
}

HWND Graph(HWND parent, int id)
{
    EnsureGraphClass();

    auto* state = new GraphState;
    HWND control = CreateWindowExW(
        0, kGraphClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, state);
    if (control == nullptr) {
        delete state;
        return nullptr;
    }
    ApplyFont(control, g_font);
    return control;
}

void SetGraphEffect(HWND graph, const apo::EffectInstance& effect)
{
    auto* state = reinterpret_cast<GraphState*>(
        GetWindowLongPtrW(graph, GWLP_USERDATA));
    if (state == nullptr) {
        return;
    }
    state->effect = effect;
    state->hasEffect = GraphSupported(effect.type);
    InvalidateRect(graph, nullptr, FALSE);
}

} // namespace ui
