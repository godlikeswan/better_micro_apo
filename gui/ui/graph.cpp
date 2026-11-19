#include "graph.h"

#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include "dsp/transfer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ui {
namespace {

constexpr wchar_t kGraphClass[] = L"BetterMicroApoGraph";
constexpr int kCurveSamples = 240;
constexpr double kMinDb = -96.0;
constexpr double kMaxDb = 24.0;
constexpr double kMinHz = 20.0;
constexpr double kMaxHz = 20000.0;

struct GraphState
{
    apo::EffectInstance effect;
    apo::StageMonitorSnapshot monitor;
    bool hasEffect = false;
    bool hasMonitor = false;
    HFONT font = nullptr;

    // Stage telemetry arrives in short buffers; these values keep the dot,
    // engagement bar and meter from jumping on every timer tick.
    float inputRms = 0.0f;
    float outputRms = 0.0f;
    float inputPeak = 0.0f;
    float engagement = 0.0f;
    float meterCurrent = 0.0f;
    float meterPeak = 0.0f;
};

struct GraphColors
{
    COLORREF grid;
    COLORREF reference;
    COLORREF zero;
    COLORREF curve;
    COLORREF spectrumFill;
    COLORREF spectrumLine;
    COLORREF stripe;
    COLORREF meterLow;
    COLORREF meterMid;
    COLORREF meterHigh;
    COLORREF meterOff;
    COLORREF engagement;
    COLORREF text;
};

struct GraphLayout
{
    RECT plot{};
    RECT meter{};
    RECT meterLabels{};
    RECT engagement{};
    bool hasMeter = false;
    bool hasEngagement = false;
};

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color,
              int style = PS_SOLID, int width = 1)
{
    HPEN pen = CreatePen(style, width, color);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
    DeleteObject(pen);
}

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

COLORREF Mix(COLORREF left, COLORREF right, double amount)
{
    const auto channel = [amount](int from, int to) -> BYTE {
        const double value = from + (to - from) * amount;
        return static_cast<BYTE>(std::clamp(std::lround(value), 0L, 255L));
    };
    return RGB(channel(GetRValue(left), GetRValue(right)),
               channel(GetGValue(left), GetGValue(right)),
               channel(GetBValue(left), GetBValue(right)));
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

double DbPosition(double db)
{
    return (db - kMinDb) / (kMaxDb - kMinDb);
}

double AmplitudeDb(float amplitude)
{
    const double magnitude =
        std::max(static_cast<double>(std::fabs(amplitude)), 1e-9);
    return std::clamp(20.0 * std::log10(magnitude), kMinDb, kMaxDb);
}

double MaxFrequency(double sampleRate)
{
    if (sampleRate <= 0.0) {
        return kMaxHz;
    }
    return std::clamp(sampleRate * 0.5, kMinHz * 2.0, kMaxHz);
}

double FrequencyPosition(double hz, double sampleRate)
{
    const double maxHz = MaxFrequency(sampleRate);
    const double span = std::log10(maxHz / kMinHz);
    if (span <= 0.0) {
        return 0.0;
    }
    const double clamped = std::clamp(hz, kMinHz, maxHz);
    return std::clamp(std::log10(clamped / kMinHz) / span, 0.0, 1.0);
}

const wchar_t* FrequencyLabel(double maxHz)
{
    if (maxHz >= 19500.0) {
        return L"20k";
    }
    if (maxHz >= 9500.0) {
        return L"10k";
    }
    if (maxHz >= 4500.0) {
        return L"5k";
    }
    return L"10k";
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

void DrawBarCaption(HDC dc, const RECT& bar, const wchar_t* text,
                    COLORREF color)
{
    RECT rect{bar.left - Scale(6), bar.top - Scale(16), bar.right + Scale(6),
              bar.top - Scale(2)};
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect,
              DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
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

void DrawYLabel(HDC dc, int left, int right, const RECT& plot, int y,
                const wchar_t* text, COLORREF color)
{
    const int center = std::clamp(
        y, static_cast<int>(plot.top) + Scale(7),
        static_cast<int>(plot.bottom) - Scale(7));
    RECT rect{left, center - Scale(7), right, center + Scale(7)};
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

GraphLayout ComputeLayout(HWND window, apo::EffectType type)
{
    RECT client{};
    GetClientRect(window, &client);

    const int captionHeight = Scale(18);
    const int bottomMargin = Scale(16);
    int left = client.left + Scale(34);
    int right = client.right - Scale(8);
    const bool hasEngagement = type == apo::EffectType::Dynamics;
    const bool hasMeter =
        type == apo::EffectType::Input || type == apo::EffectType::Gain ||
        type == apo::EffectType::Dynamics ||
        type == apo::EffectType::Equalizer ||
        type == apo::EffectType::Saturation;

    GraphLayout layout;
    layout.hasEngagement = hasEngagement;
    layout.hasMeter = hasMeter;

    if (hasEngagement) {
        left += Scale(20);
    }
    if (hasMeter) {
        // Meter bar, gap, and a right-aligned column of dB labels.
        right -= Scale(42);
    }

    layout.plot = RECT{left, client.top + captionHeight, right,
                       client.bottom - bottomMargin};
    if (hasEngagement) {
        // Keep the bar clear of the left-hand dB labels (0..30) and the plot.
        const int barRight = client.left + Scale(48);
        layout.engagement = RECT{barRight - Scale(12), layout.plot.top,
                                 barRight, layout.plot.bottom};
    }
    if (hasMeter) {
        const int barLeft = right + Scale(6);
        layout.meter = RECT{barLeft, layout.plot.top, barLeft + Scale(12),
                            layout.plot.bottom};
        layout.meterLabels = RECT{barLeft + Scale(15), layout.plot.top,
                                  client.right - Scale(4),
                                  layout.plot.bottom};
    }
    return layout;
}

void DrawMeter(HDC dc, const RECT& bar, const RECT& labels, double currentDb,
               double peakDb, const GraphColors& colors)
{
    FillColor(dc, bar, colors.meterOff);
    FrameColor(dc, bar, colors.reference);

    const double edges[] = {kMinDb, -12.0, -3.0, kMaxDb};
    const COLORREF zoneColors[] = {colors.meterLow, colors.meterMid,
                                   colors.meterHigh};
    for (int zone = 0; zone < 3; ++zone) {
        const int bottom = MapY(bar, DbPosition(edges[zone]));
        const int top = MapY(bar, DbPosition(edges[zone + 1]));
        RECT area{bar.left + 1, top, bar.right - 1, bottom};
        if (area.bottom <= area.top) {
            continue;
        }
        FillColor(dc, area, Mix(colors.meterOff, zoneColors[zone], 0.30));
        if (currentDb > edges[zone]) {
            const int fillTop = std::max(top, MapY(bar, DbPosition(currentDb)));
            RECT filled{area.left, fillTop, area.right, area.bottom};
            FillColor(dc, filled, zoneColors[zone]);
        }
    }

    for (int edge = 1; edge < 3; ++edge) {
        const int y = MapY(bar, DbPosition(edges[edge]));
        DrawLine(dc, bar.left + 1, y, bar.right - 1, y, colors.reference);
    }

    // The filled level follows the short APO hold; the line is the longer
    // peak hold, so a transient between GUI polls is still visible. It uses
    // the exact filled-zone colour so the line and bar always agree.
    if (peakDb > kMinDb) {
        const COLORREF zone = peakDb >= -3.0   ? colors.meterHigh
                              : peakDb >= -12.0 ? colors.meterMid
                                                : colors.meterLow;
        const int peakY = MapY(bar, DbPosition(peakDb));
        DrawLine(dc, bar.left - 2, peakY, bar.right + 2, peakY, zone, PS_SOLID,
                 std::max(3, Scale(3)));
    }

    struct Label {
        double db;
        const wchar_t* text;
    };
    const Label marks[] = {{kMaxDb, L"+24"}, {0.0, L"0"}, {-3.0, L"-3"},
                           {-12.0, L"-12"}, {kMinDb, L"-96"}};
    int previousY = -10000;
    for (const Label& mark : marks) {
        const int y = MapY(bar, DbPosition(mark.db));
        if (y - previousY < Scale(14)) {
            continue;
        }
        previousY = y;
        RECT text{labels.left, y - Scale(7), labels.right, y + Scale(7)};
        SetTextColor(dc, colors.text);
        DrawTextW(dc, mark.text, -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void DrawEngagement(HDC dc, const RECT& bar, double value,
                    const GraphColors& colors)
{
    FillColor(dc, bar, colors.meterOff);
    FrameColor(dc, bar, colors.reference);

    const int fillTop = MapY(bar, std::clamp(value, 0.0, 1.0));
    RECT filled{bar.left + 1, fillTop, bar.right - 1, bar.bottom - 1};
    if (filled.bottom > filled.top) {
        FillColor(dc, filled, colors.engagement);
    }
    const int halfY = MapY(bar, 0.5);
    DrawLine(dc, bar.left + 1, halfY, bar.right - 1, halfY, colors.reference,
             PS_DOT);
}

void PaintSpectrum(HDC dc, const RECT& plot,
                   const apo::StageMonitorSnapshot& monitor,
                   const GraphColors& colors)
{
    if (!monitor.valid || monitor.sampleRate == 0) {
        return;
    }

    const double maxHz = MaxFrequency(monitor.sampleRate);
    POINT points[apo::kStageMonitorBins];
    int count = 0;
    // Start at bin 0 (DC) and clamp it to the 20 Hz left edge. Skipping bin 0
    // left a visible gap before the first FFT bin (about 187 Hz at 48 kHz).
    for (int bin = 0; bin < apo::kStageMonitorBins; ++bin) {
        const double hz =
            static_cast<double>(bin) * monitor.sampleRate / apo::kStageMonitorFftSize;
        if (hz > maxHz) {
            break;
        }
        const double plottedHz = std::max(hz, kMinHz);
        const double magnitude =
            std::max(static_cast<double>(monitor.spectrum[bin]), 1e-9);
        const double db = 20.0 * std::log10(magnitude);
        points[count].x =
            MapX(plot, FrequencyPosition(plottedHz, monitor.sampleRate));
        points[count].y = MapY(plot, DbPosition(db));
        ++count;
    }
    if (count < 2) {
        return;
    }

    POINT polygon[apo::kStageMonitorBins + 2];
    int polygonCount = 0;
    polygon[polygonCount++] = POINT{points[0].x, plot.bottom};
    for (int i = 0; i < count; ++i) {
        polygon[polygonCount++] = points[i];
    }
    polygon[polygonCount++] = POINT{points[count - 1].x, plot.bottom};

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, plot.left, plot.top, plot.right, plot.bottom);
    HBRUSH brush = CreateSolidBrush(colors.spectrumFill);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    Polygon(dc, polygon, polygonCount);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, 1, colors.spectrumLine);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Polyline(dc, points, count);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    RestoreDC(dc, saved);
}

void PaintFrequencyAxes(HDC dc, const RECT& client, const RECT& plot,
                        double sampleRate, const wchar_t* caption,
                        const GraphColors& colors)
{
    DrawCaption(dc, client, plot, caption, colors.text);
    DrawXLeft(dc, plot, L"20", colors.text);
    DrawXCenter(dc, plot, MapX(plot, FrequencyPosition(100.0, sampleRate)),
                L"100", colors.text);
    DrawXCenter(dc, plot, MapX(plot, FrequencyPosition(1000.0, sampleRate)),
                L"1k", colors.text);
    DrawXRight(dc, plot, FrequencyLabel(MaxFrequency(sampleRate)), colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.top, L"+24",
               colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               MapY(plot, DbPosition(0.0)), L"0", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               MapY(plot, DbPosition(-48.0)), L"-48", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.bottom,
               L"-96", colors.text);
}

void PaintDistortion(HDC dc, const RECT& client, const RECT& plot,
                     const apo::DistortionSettings& settings,
                     const GraphState& state, const GraphColors& colors)
{
    DrawGrid(dc, plot, colors.grid, 4, 4);
    DrawLine(dc, plot.left, plot.bottom - 1, plot.right - 1, plot.top,
             colors.reference, PS_DOT);

    const apo::dsp::DistortionCurve curve =
        apo::dsp::MakeDistortionCurve(settings.function, settings.curvature);
    const double drive = apo::dsp::DecibelsToLinear(settings.driveDb);
    const double output = apo::dsp::DecibelsToLinear(settings.outputDb);
    // Drive is a pre-effect gain in the DSP: it scales the signal before the
    // fixed shaping curve. Draw that curve untouched and let Drive move the
    // operating point instead of bending the curve itself.
    auto transfer = [&](double x) {
        return apo::dsp::ShapeDistortion(curve, settings.function, x) * output;
    };

    StrokeCurve(dc, plot, colors.curve, [&](double t) {
        const double x = -1.0 + 2.0 * t;
        return (std::clamp(transfer(x), -1.0, 1.0) + 1.0) * 0.5;
    });

    if (state.monitor.valid && state.inputPeak > 0.0f) {
        // Feed the measured input peak through the pre-effect Drive gain. A
        // higher Drive pushes the mirrored markers apart toward the flat part
        // of the fixed curve, which is what the stage actually does to sound.
        const double magnitude = std::clamp(
            static_cast<double>(state.inputPeak) * drive, 0.0, 1.0);
        const int leftX = MapX(plot, (1.0 - magnitude) * 0.5);
        const int rightX = MapX(plot, (1.0 + magnitude) * 0.5);
        DrawLine(dc, leftX, plot.top, leftX, plot.bottom, colors.stripe,
                 PS_DOT);
        DrawLine(dc, rightX, plot.top, rightX, plot.bottom, colors.stripe,
                 PS_DOT);
        for (double x : {-magnitude, magnitude}) {
            const int dotX = MapX(plot, (x + 1.0) * 0.5);
            const int dotY = MapY(
                plot, (std::clamp(transfer(x), -1.0, 1.0) + 1.0) * 0.5);
            DrawDot(dc, dotX, dotY, colors.stripe);
        }
    }

    DrawCaption(dc, client, plot, L"Transfer curve", colors.text);
    DrawXLeft(dc, plot, L"-1", colors.text);
    DrawXCenter(dc, plot, (plot.left + plot.right) / 2, L"0", colors.text);
    DrawXRight(dc, plot, L"1", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.top, L"1",
               colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               (plot.top + plot.bottom) / 2, L"0", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.bottom,
               L"-1", colors.text);
}

void PaintGain(HDC dc, const RECT& client, const RECT& plot,
               const apo::GainSettings& settings, const GraphState& state,
               const GraphColors& colors)
{
    DrawGrid(dc, plot, colors.grid, 4, 4);
    DrawLine(dc, plot.left, plot.bottom - 1, plot.right - 1, plot.top,
             colors.reference, PS_DOT);

    // Make the 0 dB reference unmissable: the dotted grid alone made the unity
    // line look like just another horizontal rule.
    const int zeroX = MapX(plot, DbPosition(0.0));
    const int zeroY = MapY(plot, DbPosition(0.0));
    DrawLine(dc, zeroX, plot.top, zeroX, plot.bottom, colors.zero, PS_DOT);
    DrawLine(dc, plot.left, zeroY, plot.right, zeroY, colors.zero);

    StrokeCurve(dc, plot, colors.curve, [&](double t) {
        const double inputDb = kMinDb + (kMaxDb - kMinDb) * t;
        return DbPosition(inputDb + settings.gainDb);
    });

    if (state.monitor.valid) {
        const int x = MapX(plot, DbPosition(AmplitudeDb(state.inputRms)));
        const int y = MapY(plot, DbPosition(AmplitudeDb(state.outputRms)));
        DrawLine(dc, x, plot.bottom, x, y, colors.reference, PS_DOT);
        DrawLine(dc, plot.left, y, x, y, colors.reference, PS_DOT);
        DrawDot(dc, x, y, colors.stripe);
    }

    RECT zeroLabel{plot.left + Scale(4), zeroY - Scale(15),
                   plot.left + Scale(46), zeroY - Scale(2)};
    SetTextColor(dc, colors.zero);
    DrawTextW(dc, L"0 dB", -1, &zeroLabel,
              DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    DrawCaption(dc, client, plot, L"Input / output RMS (dBFS)", colors.text);
    DrawXLeft(dc, plot, L"-96", colors.text);
    DrawXCenter(dc, plot, MapX(plot, DbPosition(0.0)), L"0", colors.text);
    DrawXRight(dc, plot, L"+24", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.top, L"+24",
               colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               MapY(plot, DbPosition(0.0)), L"0", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               MapY(plot, DbPosition(-48.0)), L"-48", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.bottom,
               L"-96", colors.text);
}

void PaintDynamics(HDC dc, const RECT& client, const RECT& plot,
                   const apo::DynamicsSettings& settings,
                   const GraphColors& colors)
{
    constexpr double kThresholdMin = -80.0;
    constexpr double kThresholdMax = 0.0;

    DrawGrid(dc, plot, colors.grid, 4, 4);
    DrawLine(dc, plot.left, plot.bottom - 1, plot.right - 1, plot.top,
             colors.reference, PS_DOT);

    const double threshold = std::clamp(settings.thresholdDb, kThresholdMin,
                                        kThresholdMax);
    const double inputTrigger =
        std::clamp(threshold - settings.inputDb, kMinDb, kMaxDb);
    const int thresholdX = MapX(plot, DbPosition(inputTrigger));
    DrawLine(dc, thresholdX, plot.top, thresholdX, plot.bottom,
             colors.reference, PS_DOT);

    StrokeCurve(dc, plot, colors.curve, [&](double t) {
        const double inputDb = kMinDb + (kMaxDb - kMinDb) * t;
        const double detectedDb = inputDb + settings.inputDb;
        const double transferredDb = apo::dsp::DynamicsTransferDb(
            detectedDb, threshold, settings.ratio, settings.mode);
        return DbPosition(transferredDb + settings.outputDb);
    });

    DrawCaption(dc, client, plot, L"Transfer curve (dBFS)", colors.text);
    DrawXLeft(dc, plot, L"-96", colors.text);
    DrawXCenter(dc, plot, (plot.left + plot.right) / 2, L"-36", colors.text);
    DrawXRight(dc, plot, L"+24", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.top, L"+24",
               colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               MapY(plot, DbPosition(0.0)), L"0", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               MapY(plot, DbPosition(-48.0)), L"-48", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.bottom,
               L"-96", colors.text);
}

void PaintEqualizer(HDC dc, const RECT& client, const RECT& plot,
                    const apo::EqualizerSettings& settings,
                    double sampleRate, const GraphColors& colors)
{
    const double maxHz = MaxFrequency(sampleRate);
    const double centerHz = std::clamp(settings.frequencyHz, kMinHz, maxHz);
    const double q = std::max(settings.q, 0.1);

    auto decibelPosition = [](double db) { return DbPosition(db); };

    const double gridHz[] = {100.0, 1000.0, 10000.0};
    for (double hz : gridHz) {
        if (hz >= maxHz) {
            continue;
        }
        const int x = MapX(plot, FrequencyPosition(hz, sampleRate));
        DrawLine(dc, x, plot.top, x, plot.bottom, colors.grid);
    }
    const double gridDb[] = {0.0, -24.0, -48.0, -72.0};
    for (double db : gridDb) {
        const int y = MapY(plot, decibelPosition(db));
        DrawLine(dc, plot.left, y, plot.right, y, colors.grid);
    }

    DrawLine(dc, plot.left, MapY(plot, decibelPosition(0.0)), plot.right,
             MapY(plot, decibelPosition(0.0)), colors.reference, PS_DOT);
    const int centerX =
        MapX(plot, FrequencyPosition(centerHz, sampleRate));
    DrawLine(dc, centerX, plot.top, centerX, plot.bottom, colors.reference,
             PS_DOT);

    StrokeCurve(dc, plot, colors.curve, [&](double t) {
        const double hz = kMinHz * std::pow(maxHz / kMinHz, t);
        const double db =
            apo::dsp::BandpassMagnitudeDb(hz, centerHz, q) + settings.levelDb;
        return decibelPosition(db);
    });

    DrawCaption(dc, client, plot, L"Band response + level", colors.text);
    DrawXLeft(dc, plot, L"20", colors.text);
    DrawXCenter(dc, plot, MapX(plot, FrequencyPosition(100.0, sampleRate)),
                L"100", colors.text);
    DrawXCenter(dc, plot, MapX(plot, FrequencyPosition(1000.0, sampleRate)),
                L"1k", colors.text);
    DrawXRight(dc, plot, FrequencyLabel(maxHz), colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.top, L"+24",
               colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               MapY(plot, DbPosition(0.0)), L"0", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot,
               MapY(plot, DbPosition(-48.0)), L"-48", colors.text);
    DrawYLabel(dc, client.left, client.left + Scale(30), plot, plot.bottom,
               L"-96", colors.text);
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

    const GraphLayout layout = ComputeLayout(window, state.effect.type);
    const RECT& plot = layout.plot;
    if (plot.right - plot.left < Scale(20) ||
        plot.bottom - plot.top < Scale(20)) {
        return;
    }

    FillRect(dc, &plot, theme.editBrush);
    FrameColor(dc, plot, theme.color.frame);

    GraphColors colors{};
    colors.grid = theme.color.graphGrid;
    colors.reference = theme.color.frame;
    colors.zero = Mix(theme.color.text, theme.color.accent, 0.45);
    colors.curve =
        IsWindowEnabled(window) ? theme.color.accent : theme.color.disabledText;
    colors.spectrumFill =
        Mix(theme.color.editBackground, theme.color.accent, 0.22);
    colors.spectrumLine =
        Mix(colors.curve, theme.color.editBackground, 0.40);
    colors.stripe = Mix(theme.color.accent, RGB(255, 255, 255), 0.35);
    colors.meterLow = RGB(72, 190, 110);
    colors.meterMid = RGB(235, 185, 60);
    colors.meterHigh = RGB(225, 40, 40);
    colors.meterOff = Mix(theme.color.editBackground, theme.color.text, 0.10);
    colors.engagement = theme.color.accent;
    colors.text = theme.color.text;

    HFONT font = state.font != nullptr
                     ? state.font
                     : reinterpret_cast<HFONT>(
                           GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    switch (state.effect.type) {
    case apo::EffectType::Input:
        DrawGrid(dc, plot, colors.grid, 4, 4);
        PaintSpectrum(dc, plot, state.monitor, colors);
        PaintFrequencyAxes(dc, client, plot, state.monitor.sampleRate,
                           L"Input spectrum (dBFS)", colors);
        break;
    case apo::EffectType::Gain:
        PaintGain(dc, client, plot, state.effect.gain, state, colors);
        break;
    case apo::EffectType::Saturation:
        PaintDistortion(dc, client, plot, state.effect.distortion, state,
                        colors);
        break;
    case apo::EffectType::Dynamics:
        PaintDynamics(dc, client, plot, state.effect.dynamics, colors);
        break;
    case apo::EffectType::Equalizer:
        PaintSpectrum(dc, plot, state.monitor, colors);
        PaintEqualizer(dc, client, plot, state.effect.equalizer,
                       state.monitor.sampleRate, colors);
        break;
    default:
        break;
    }

    if (layout.hasEngagement) {
        DrawEngagement(dc, layout.engagement, state.engagement, colors);
        DrawBarCaption(dc, layout.engagement, L"ENG", colors.text);
    }
    if (layout.hasMeter) {
        DrawMeter(dc, layout.meter, layout.meterLabels,
                  AmplitudeDb(state.meterCurrent), AmplitudeDb(state.meterPeak),
                  colors);
        DrawBarCaption(dc, layout.meter, L"OUT", colors.text);
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

void ClearGraphState(GraphState& state)
{
    state.monitor = apo::StageMonitorSnapshot{};
    state.hasMonitor = false;
    state.inputRms = 0.0f;
    state.outputRms = 0.0f;
    state.inputPeak = 0.0f;
    state.engagement = 0.0f;
    state.meterCurrent = 0.0f;
    state.meterPeak = 0.0f;
}

} // namespace

bool GraphSupported(apo::EffectType type)
{
    switch (type) {
    case apo::EffectType::Input:
    case apo::EffectType::Gain:
    case apo::EffectType::Saturation:
    case apo::EffectType::Dynamics:
    case apo::EffectType::Equalizer:
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
    if (state->hasEffect &&
        (state->effect.type != effect.type || state->effect.id != effect.id)) {
        ClearGraphState(*state);
    }
    state->effect = effect;
    state->hasEffect = GraphSupported(effect.type);
    InvalidateRect(graph, nullptr, FALSE);
}

void SetGraphMonitor(HWND graph, const apo::StageMonitorSnapshot& monitor)
{
    auto* state = reinterpret_cast<GraphState*>(
        GetWindowLongPtrW(graph, GWLP_USERDATA));
    if (state == nullptr || !state->hasEffect ||
        state->effect.type != monitor.type || !monitor.valid) {
        if (state != nullptr) {
            ClearGraphState(*state);
        }
        InvalidateRect(graph, nullptr, FALSE);
        return;
    }

    constexpr float kSmoothing = 0.30f;
    state->monitor = monitor;
    state->hasMonitor = true;
    state->inputRms += (monitor.inputRms - state->inputRms) * kSmoothing;
    state->outputRms += (monitor.outputRms - state->outputRms) * kSmoothing;
    state->inputPeak += (monitor.inputPeak - state->inputPeak) * kSmoothing;
    state->engagement += (monitor.engagement - state->engagement) * kSmoothing;
    // Both meter parts use APO-side cross-buffer holds, so a transient between
    // GUI polls moves the fill as well. The fill's hold is shorter, leaving the
    // bright line as the longer max marker.
    state->meterCurrent = monitor.outputPeakShortHold;
    state->meterPeak = monitor.outputPeakHold;
    InvalidateRect(graph, nullptr, FALSE);
}

void ClearGraphMonitor(HWND graph)
{
    auto* state = reinterpret_cast<GraphState*>(
        GetWindowLongPtrW(graph, GWLP_USERDATA));
    if (state == nullptr) {
        return;
    }
    ClearGraphState(*state);
    InvalidateRect(graph, nullptr, FALSE);
}

} // namespace ui
