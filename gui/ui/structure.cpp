#include "structure.h"

#include "apo.h"
#include "graph.h"
#include "looper_progress.h"
#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>
#include <string>

namespace {

using ui::Scale;

void AddColumn(HWND list, int index, const wchar_t* text, int width,
               int format = LVCFMT_LEFT)
{
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    column.fmt = format;
    column.iSubItem = index;
    column.cx = Scale(width);
    column.pszText = const_cast<wchar_t*>(text);
    ListView_InsertColumn(list, index, &column);
}

void AddTooltip(HWND control, const wchar_t* text)
{
    if (g_infoTooltip == nullptr || control == nullptr) {
        return;
    }

    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    // Tooltips must subclass the tool window itself: static controls do not
    // forward mouse movement to their parent.
    info.hwnd = control;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(g_infoTooltip, TTM_ADDTOOLW, 0,
                 reinterpret_cast<LPARAM>(&info));
}

enum class InfoIconKind : DWORD_PTR {
    Latency = 0,
    Interrupt = 1,
};

constexpr UINT_PTR kInfoIconSubclassId = 5;

void DrawStopwatch(HDC dc, const RECT& client)
{
    const int centerX = (client.left + client.right) / 2;
    const int centerY = (client.top + client.bottom) / 2 + Scale(1);
    const int size = static_cast<int>(
        std::min(client.right - client.left, client.bottom - client.top));
    const int radius = std::max(Scale(3), size / 2 - Scale(3));
    const int stem = Scale(3);

    Ellipse(dc, centerX - radius, centerY - radius, centerX + radius,
            centerY + radius);
    MoveToEx(dc, centerX, centerY - radius, nullptr);
    LineTo(dc, centerX, centerY - radius - stem);
    MoveToEx(dc, centerX - stem, centerY - radius - stem, nullptr);
    LineTo(dc, centerX + stem, centerY - radius - stem);
    MoveToEx(dc, centerX, centerY, nullptr);
    LineTo(dc, centerX + radius / 2, centerY - radius / 2);
}

void DrawInterrupt(HDC dc, const RECT& client)
{
    const int centerX = (client.left + client.right) / 2;
    const int centerY = (client.top + client.bottom) / 2;
    const int size = static_cast<int>(
        std::min(client.right - client.left, client.bottom - client.top));
    const int radius = std::max(Scale(4), size / 2 - Scale(3));

    MoveToEx(dc, centerX, centerY - radius, nullptr);
    LineTo(dc, centerX + radius, centerY + radius);
    LineTo(dc, centerX - radius, centerY + radius);
    LineTo(dc, centerX, centerY - radius);

    MoveToEx(dc, centerX, centerY - radius / 2, nullptr);
    LineTo(dc, centerX, centerY + radius / 4);
    const int dotY = centerY + radius / 2;
    MoveToEx(dc, centerX, dotY, nullptr);
    LineTo(dc, centerX, dotY + Scale(1));
}

LRESULT CALLBACK InfoIconProc(HWND window, UINT message, WPARAM wParam,
                              LPARAM lParam, UINT_PTR, DWORD_PTR ref)
{
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message != WM_PAINT) {
        return DefSubclassProc(window, message, wParam, lParam);
    }

    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);

    const ui::Theme& theme = ui::GetTheme();
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, theme.backgroundBrush);

    const COLORREF color = IsWindowEnabled(window) ? theme.color.text
                                                   : theme.color.disabledText;
    HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    if (ref == static_cast<DWORD_PTR>(InfoIconKind::Interrupt)) {
        DrawInterrupt(dc, client);
    } else {
        DrawStopwatch(dc, client);
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    EndPaint(window, &paint);
    return 0;
}

HWND InfoIcon(HWND parent, int id, InfoIconKind kind)
{
    HWND control = CreateWindowExW(
        0, WC_STATICW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_CENTER | SS_NOTIFY,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SetWindowSubclass(control, InfoIconProc, kInfoIconSubclassId,
                      static_cast<DWORD_PTR>(kind));
    return control;
}

void PlaceInfoIcon(HWND icon, HWND textControl, int x, int y, int iconSize,
                   int rowHeight, int maxRight, int textOffset = 0)
{
    if (icon == nullptr) {
        return;
    }

    // Put the marker just past the control's text, clamped before maxRight.
    int textWidth = 0;
    if (textControl != nullptr) {
        const int length = GetWindowTextLengthW(textControl);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        if (length > 0) {
            GetWindowTextW(textControl, text.data(), length + 1);
            text.resize(static_cast<size_t>(length));
        } else {
            text.clear();
        }
        HDC dc = GetDC(textControl);
        if (dc != nullptr) {
            HFONT font = reinterpret_cast<HFONT>(
                SendMessageW(textControl, WM_GETFONT, 0, 0));
            HGDIOBJ oldFont =
                font != nullptr ? SelectObject(dc, font) : nullptr;
            SIZE extent{};
            GetTextExtentPoint32W(dc, text.c_str(),
                                  static_cast<int>(text.size()), &extent);
            textWidth = extent.cx;
            if (oldFont != nullptr) {
                SelectObject(dc, oldFont);
            }
            ReleaseDC(textControl, dc);
        }
    }

    const int iconX = std::max(
        x, std::min(x + textOffset + textWidth + Scale(4),
                    maxRight - iconSize));
    const int iconY = y + (rowHeight - iconSize) / 2;
    MoveWindow(icon, iconX, iconY, iconSize, iconSize, TRUE);
    ShowWindow(icon, SW_SHOW);
    // The icon overlays the label, so keep it above that sibling.
    SetWindowPos(icon, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

constexpr UINT_PTR kLooperGestureSubclassId = 6;
constexpr UINT_PTR kLooperLongPressTimer = 1;
constexpr UINT_PTR kLooperSingleTapTimer = 2;

struct LooperGestureState
{
    bool active = false;
    bool longFired = false;
    bool consumed = false;
    bool tapPending = false;
    DWORD downTick = 0;
    DWORD lastTapTick = 0;
    // Every new press invalidates the previous press's timers. A delayed
    // WM_TIMER can then never deliver a stale Tap after a DoubleTap or hold.
    unsigned generation = 0;
    unsigned timerGeneration = 0;
};

LooperGestureState g_looperGestures[apo::kLooperTrackCount];

LRESULT CALLBACK LooperGestureProc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam, UINT_PTR, DWORD_PTR ref)
{
    const int track = static_cast<int>(ref);
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return DefSubclassProc(window, message, wParam, lParam);
    }
    LooperGestureState& state = g_looperGestures[track];
    const auto post = [track](LooperGesture gesture) {
        PostMessageW(g_window, kLooperGestureMessage,
                     static_cast<WPARAM>(track),
                     static_cast<LPARAM>(gesture));
    };

    switch (message) {
    case WM_LBUTTONDOWN: {
        const DWORD now = GetTickCount();
        // Decide on the second press, not its release, so a pending single tap
        // cannot fire first.
        if (state.tapPending && state.lastTapTick != 0 &&
            now - state.lastTapTick <= kLooperDoubleTapMs)
        {
            KillTimer(window, kLooperSingleTapTimer);
            KillTimer(window, kLooperLongPressTimer);
            ++state.generation;
            state.timerGeneration = state.generation;
            state.active = true;
            state.consumed = true;
            state.longFired = false;
            state.tapPending = false;
            state.lastTapTick = 0;
            state.downTick = now;
            SetCapture(window);
            post(LooperGesture::DoubleTap);
            return 0;
        }

        KillTimer(window, kLooperSingleTapTimer);
        KillTimer(window, kLooperLongPressTimer);
        ++state.generation;
        state.timerGeneration = state.generation;
        state.active = true;
        state.consumed = false;
        state.longFired = false;
        state.tapPending = false;
        state.lastTapTick = 0;
        state.downTick = now;
        SetTimer(window, kLooperLongPressTimer, kLooperLongPressMs, nullptr);
        SetCapture(window);
        return 0;
    }

    case WM_TIMER:
        if (wParam == kLooperLongPressTimer && state.active &&
            !state.consumed && !state.longFired &&
            state.timerGeneration == state.generation)
        {
            ++state.generation;
            state.timerGeneration = state.generation;
            state.longFired = true;
            state.tapPending = false;
            state.lastTapTick = 0;
            KillTimer(window, kLooperLongPressTimer);
            KillTimer(window, kLooperSingleTapTimer);
            post(LooperGesture::LongPress);
            return 0;
        }
        if (wParam == kLooperSingleTapTimer && state.tapPending &&
            state.timerGeneration == state.generation)
        {
            ++state.generation;
            state.timerGeneration = state.generation;
            state.tapPending = false;
            state.lastTapTick = 0;
            post(LooperGesture::Tap);
            return 0;
        }
        break;

    case WM_CANCELMODE:
        KillTimer(window, kLooperLongPressTimer);
        KillTimer(window, kLooperSingleTapTimer);
        ++state.generation;
        state.timerGeneration = state.generation;
        state.active = false;
        state.consumed = false;
        state.longFired = false;
        state.tapPending = false;
        state.lastTapTick = 0;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        break;

    case WM_LBUTTONUP: {
        if (!state.active) {
            break;
        }
        state.active = false;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        KillTimer(window, kLooperLongPressTimer);

        if (state.consumed) {
            state.consumed = false;
            state.tapPending = false;
            state.lastTapTick = 0;
            ++state.generation;
            return 0;
        }
        if (state.longFired) {
            state.longFired = false;
            state.tapPending = false;
            state.lastTapTick = 0;
            ++state.generation;
            return 0;
        }

        const DWORD now = GetTickCount();
        const DWORD held = now - state.downTick;
        state.timerGeneration = state.generation;
        if (held >= kLooperLongPressMs) {
            ++state.generation;
            state.lastTapTick = 0;
            post(LooperGesture::LongPress);
            return 0;
        }
        if (held < kLooperTapMaxMs) {
            state.tapPending = true;
            state.lastTapTick = now;
            // Delay the single tap just long enough for a second press to win.
            SetTimer(window, kLooperSingleTapTimer, kLooperDoubleTapMs,
                     nullptr);
        } else {
            state.tapPending = false;
            state.lastTapTick = 0;
        }
        return 0;
    }

    default:
        break;
    }

    return DefSubclassProc(window, message, wParam, lParam);
}

void CreateParamResetButtons(HWND parent)
{
    for (int param = 0; param < ParamResetCount; ++param) {
        HWND button = ui::IconButton(parent, IDC_PARAM_RESET, L"\u21BA");
        LONG style = GetWindowLongW(button, GWL_STYLE);
        style &= ~WS_TABSTOP;
        SetWindowLongW(button, GWL_STYLE, style);
        SetWindowLongPtrW(button, GWLP_USERDATA,
                          static_cast<LONG_PTR>(param));
        ShowWindow(button, SW_HIDE);
        g_paramResetButtons[param] = button;
    }
}

void ResizeEffectColumns()
{
    if (!g_effectList) {
        return;
    }

    RECT rect{};
    GetClientRect(g_effectList, &rect);
    const int width = rect.right - rect.left;

    // Make the delete column absorb any remainder so it ends exactly at the
    // right edge. The glyph stays centered inside that column.
    int nameWidth = width - Scale(28);
    if (nameWidth < Scale(60)) {
        nameWidth = Scale(60);
    }
    const int deleteWidth = width - nameWidth;
    ListView_SetColumnWidth(g_effectList, 0, nameWidth);
    ListView_SetColumnWidth(g_effectList, 1, deleteWidth);
}

void LayoutEffectParams(int x, int y, int width)
{
    for (HWND button : g_paramResetButtons) {
        ShowWindow(button, SW_HIDE);
    }
    ShowWindow(g_dynamicsLookaheadIcon, SW_HIDE);
    ShowWindow(g_eqLookaheadIcon, SW_HIDE);
    ShowWindow(g_reverbPredelayIcon, SW_HIDE);
    if (g_effectSelection < 0) {
        return;
    }

    const int columnGap = Scale(16);
    const int columnWidth = (width - columnGap) / 2;
    const int labelWidth = Scale(128);
    const int resetWidth = Scale(24);
    const int resetGap = Scale(4);
    const int sliderWidth =
        std::max(Scale(50), columnWidth - labelWidth - resetWidth - resetGap);
    const int controlHeight = Scale(26);
    const int rowStep = Scale(32);

    auto placeResetAt = [&](int param, int columnX, int rowY, int columnW) {
        if (param < 0 || param >= ParamResetCount) {
            return;
        }
        HWND button = g_paramResetButtons[param];
        MoveWindow(button, columnX + columnW - resetWidth, rowY, resetWidth,
                   controlHeight, TRUE);
        ShowWindow(button, SW_SHOW);
    };
    auto placeReset = [&](int param, int columnX, int rowY) {
        placeResetAt(param, columnX, rowY, columnWidth);
    };

    const int iconSize = Scale(16);
    auto placeIcon = [&](HWND icon, HWND label, int columnX, int rowY) {
        PlaceInfoIcon(icon, label, columnX, rowY, iconSize, Scale(20),
                      columnX + labelWidth);
    };

    auto placePair = [&](HWND leftLabel, HWND leftSlider, int leftReset,
                         HWND rightLabel, HWND rightSlider, int rightReset) {
        MoveWindow(leftLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(leftSlider, x + labelWidth, y, sliderWidth, controlHeight,
                   TRUE);
        placeReset(leftReset, x, y);
        if (rightLabel != nullptr) {
            const int rightX = x + columnWidth + columnGap;
            MoveWindow(rightLabel, rightX, y, labelWidth, Scale(20), TRUE);
            MoveWindow(rightSlider, rightX + labelWidth, y, sliderWidth,
                       controlHeight, TRUE);
            placeReset(rightReset, rightX, y);
        }
        y += rowStep;
    };

    switch (g_effectType) {
    case apo::EffectType::Noise: {
        const int rightX = x + columnWidth + columnGap;
        MoveWindow(g_noiseReductionLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_noiseReductionSlider, x + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        placeReset(ParamResetNoiseReduction, x, y);
        MoveWindow(g_noiseFftSizeLabel, rightX, y, labelWidth, Scale(20),
                   TRUE);
        MoveWindow(g_noiseFftSizeCombo, rightX + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetNoiseFftSize, rightX, y);
        y += Scale(36);
        MoveWindow(g_noiseCapture, x, y, columnWidth, Scale(28), TRUE);
        MoveWindow(g_noiseNote, rightX, y, columnWidth, Scale(36), TRUE);
        break;
    }
    case apo::EffectType::Gate:
    case apo::EffectType::Compressor: {
        const int rightX = x + columnWidth + columnGap;
        placePair(g_dynamicsInputLabel, g_dynamicsInputSlider,
                  ParamResetDynamicsInput, g_dynamicsThresholdLabel,
                  g_dynamicsThresholdSlider, ParamResetDynamicsThreshold);
        placePair(g_dynamicsRatioLabel, g_dynamicsRatioSlider,
                  ParamResetDynamicsRatio, g_dynamicsAttackLabel,
                  g_dynamicsAttackSlider, ParamResetDynamicsAttack);
        placePair(g_dynamicsReleaseLabel, g_dynamicsReleaseSlider,
                  ParamResetDynamicsRelease, g_dynamicsOutputLabel,
                  g_dynamicsOutputSlider, ParamResetDynamicsOutput);
        placePair(g_dynamicsHoldLabel, g_dynamicsHoldSlider,
                  ParamResetDynamicsHold, g_dynamicsLookaheadLabel,
                  g_dynamicsLookaheadSlider, ParamResetDynamicsLookahead);
        placeIcon(g_dynamicsLookaheadIcon, g_dynamicsLookaheadLabel, rightX,
                  y - rowStep);
        break;
    }
    case apo::EffectType::Equalizer: {
        const int rightX = x + columnWidth + columnGap;
        placePair(g_eqFrequencyLabel, g_eqFrequencySlider,
                  ParamResetDynFrequency, g_eqQLabel, g_eqQSlider,
                  ParamResetDynQ);

        MoveWindow(g_eqLevelLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_eqLevelSlider, x + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        placeReset(ParamResetDynLevel, x, y);
        MoveWindow(g_eqDynamicLabel, rightX, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_eqDynamicCombo, rightX + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetDynMode, rightX, y);
        y += rowStep;

        placePair(g_eqThresholdLabel, g_eqThresholdSlider,
                  ParamResetDynThreshold, g_eqRatioLabel, g_eqRatioSlider,
                  ParamResetDynRatio);
        placePair(g_eqRangeLabel, g_eqRangeSlider, ParamResetDynRange,
                  g_eqAttackLabel, g_eqAttackSlider, ParamResetDynAttack);
        placePair(g_eqReleaseLabel, g_eqReleaseSlider, ParamResetDynRelease,
                  g_eqHoldLabel, g_eqHoldSlider, ParamResetDynHold);
        placePair(g_eqLookaheadLabel, g_eqLookaheadSlider,
                  ParamResetDynLookahead, nullptr, nullptr, 0);
        placeIcon(g_eqLookaheadIcon, g_eqLookaheadLabel, x, y - rowStep);
        break;
    }
    case apo::EffectType::Input:
        placePair(g_inputGainLabel, g_inputGainSlider, ParamResetInputGain,
                  nullptr, nullptr, 0);
        MoveWindow(g_inputTreatMono, x, y, width, Scale(24), TRUE);
        break;
    case apo::EffectType::Gain:
        placePair(g_gainLabel, g_gainSlider, ParamResetGain, nullptr, nullptr,
                  0);
        break;
    case apo::EffectType::Saturation:
        MoveWindow(g_distFunctionLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_distFunctionCombo, x + labelWidth, y, Scale(180),
                   Scale(200), TRUE);
        placeReset(ParamResetDistFunction, x, y);
        y += Scale(34);
        placePair(g_distDriveLabel, g_distDriveSlider, ParamResetDistDrive,
                  g_distCurvatureLabel, g_distCurvatureSlider,
                  ParamResetDistCurvature);
        placePair(g_distOutputLabel, g_distOutputSlider, ParamResetDistOutput,
                  nullptr, nullptr, 0);
        break;
    case apo::EffectType::Reverb: {
        const int rightX = x + columnWidth + columnGap;
        MoveWindow(g_reverbAlgorithmLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_reverbAlgorithmCombo, x + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetReverbAlgorithm, x, y);
        MoveWindow(g_reverbLevelLabel, rightX, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_reverbLevelSlider, rightX + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        placeReset(ParamResetReverbLevel, rightX, y);
        y += rowStep;

        placePair(g_reverbMaxLengthLabel, g_reverbMaxLengthSlider,
                  ParamResetReverbMaxLength, g_reverbSizeLabel,
                  g_reverbSizeSlider, ParamResetReverbSize);
        placePair(g_reverbDampingLabel, g_reverbDampingSlider,
                  ParamResetReverbDamping, g_reverbWidthLabel,
                  g_reverbWidthSlider, ParamResetReverbWidth);
        placePair(g_reverbDensityLabel, g_reverbDensitySlider,
                  ParamResetReverbDensity, g_reverbPredelayLabel,
                  g_reverbPredelaySlider, ParamResetReverbPredelay);
        placeIcon(g_reverbPredelayIcon, g_reverbPredelayLabel, rightX,
                  y - rowStep);
        break;
    }
    case apo::EffectType::Echo: {
        const int rightX = x + columnWidth + columnGap;
        MoveWindow(g_echoModeLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_echoModeCombo, x + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetEchoMode, x, y);
        MoveWindow(g_echoTimeLabel, rightX, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_echoTimeSlider, rightX + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        placeReset(ParamResetEchoTime, rightX, y);
        y += rowStep;
        placePair(g_echoFeedbackLabel, g_echoFeedbackSlider,
                  ParamResetEchoFeedback, g_echoDampingLabel,
                  g_echoDampingSlider, ParamResetEchoDamping);
        placePair(g_echoLevelLabel, g_echoLevelSlider, ParamResetEchoLevel,
                  nullptr, nullptr, 0);
        break;
    }
    case apo::EffectType::Chorus: {
        placePair(g_chorusVoicesLabel, g_chorusVoicesSlider,
                  ParamResetChorusVoices, g_chorusRateLabel,
                  g_chorusRateSlider, ParamResetChorusRate);
        placePair(g_chorusDepthLabel, g_chorusDepthSlider,
                  ParamResetChorusDepth, g_chorusWidthLabel,
                  g_chorusWidthSlider, ParamResetChorusWidth);
        placePair(g_chorusLevelLabel, g_chorusLevelSlider,
                  ParamResetChorusLevel, nullptr, nullptr, 0);
        break;
    }
    case apo::EffectType::Tremolo: {
        placePair(g_tremoloRateLabel, g_tremoloRateSlider,
                  ParamResetTremoloRate, g_tremoloDepthLabel,
                  g_tremoloDepthSlider, ParamResetTremoloDepth);
        const int rightX = x + columnWidth + columnGap;
        MoveWindow(g_tremoloWaveformLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_tremoloWaveformCombo, x + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetTremoloWaveform, x, y);
        MoveWindow(g_tremoloSpreadLabel, rightX, y, labelWidth, Scale(20),
                   TRUE);
        MoveWindow(g_tremoloSpreadSlider, rightX + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        placeReset(ParamResetTremoloSpread, rightX, y);
        break;
    }
    case apo::EffectType::Vibrato: {
        placePair(g_vibratoRateLabel, g_vibratoRateSlider,
                  ParamResetVibratoRate, g_vibratoDepthLabel,
                  g_vibratoDepthSlider, ParamResetVibratoDepth);
        const int rightX = x + columnWidth + columnGap;
        MoveWindow(g_vibratoWaveformLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_vibratoWaveformCombo, x + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetVibratoWaveform, x, y);
        MoveWindow(g_vibratoSpreadLabel, rightX, y, labelWidth, Scale(20),
                   TRUE);
        MoveWindow(g_vibratoSpreadSlider, rightX + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        placeReset(ParamResetVibratoSpread, rightX, y);
        break;
    }
    case apo::EffectType::PitchShifter: {
        const int rightX = x + columnWidth + columnGap;
        MoveWindow(g_pitchAlgorithmLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_pitchAlgorithmCombo, x + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetPitchAlgorithm, x, y);
        MoveWindow(g_pitchWindowLabel, rightX, y, labelWidth, Scale(20),
                   TRUE);
        MoveWindow(g_pitchWindowSlider, rightX + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        placeReset(ParamResetPitchWindow, rightX, y);
        y += rowStep;
        placePair(g_pitchShiftLabel, g_pitchShiftSlider,
                  ParamResetPitchShift, g_pitchLevelLabel,
                  g_pitchLevelSlider, ParamResetPitchLevel);
        y += rowStep;
        placePair(g_pitchDryLevelLabel, g_pitchDryLevelSlider,
                  ParamResetPitchDryLevel, nullptr, nullptr, 0);
        break;
    }
    case apo::EffectType::Looper: {
        const int gap = Scale(6);
        const int looperRowStep = Scale(44);
        const int buttonWidth = Scale(52);
        const int stateWidth = Scale(76);
        const int volumeLabelWidth = Scale(34);
        const int extra = std::max(0, width - Scale(572));
        const int volumeSliderWidth = Scale(90) + extra / 3;
        const int freeWidth = Scale(96) + extra / 3;
        const int stopWidth = Scale(116) + extra / 3;
        const int volumeAreaWidth =
            volumeLabelWidth + volumeSliderWidth + resetWidth + resetGap;
        const int freeAreaWidth = freeWidth + resetWidth + resetGap;
        const int stopAreaWidth = stopWidth + resetWidth + resetGap;

        int cursor = x;
        MoveWindow(g_looperHeaderTrack, cursor, y, buttonWidth, Scale(20),
                   TRUE);
        cursor += buttonWidth + gap;
        MoveWindow(g_looperHeaderState, cursor, y, stateWidth, Scale(20), TRUE);
        cursor += stateWidth + gap;
        MoveWindow(g_looperHeaderVolume, cursor, y, volumeAreaWidth, Scale(20),
                   TRUE);
        cursor += volumeAreaWidth + gap;
        MoveWindow(g_looperHeaderFree, cursor, y, freeAreaWidth, Scale(20),
                   TRUE);
        cursor += freeAreaWidth + gap;
        MoveWindow(g_looperHeaderStop, cursor, y, stopAreaWidth, Scale(20),
                   TRUE);

        int rowY = y + Scale(24);
        for (int track = 0; track < apo::kLooperTrackCount; ++track) {
            cursor = x;
            MoveWindow(g_looperButtons[track], cursor, rowY, buttonWidth,
                       controlHeight, TRUE);
            cursor += buttonWidth + gap;
            MoveWindow(g_looperStateLabels[track], cursor, rowY, stateWidth,
                       controlHeight, TRUE);
            cursor += stateWidth + gap;
            MoveWindow(g_looperVolumeSliders[track], cursor, rowY,
                       volumeSliderWidth, controlHeight, TRUE);
            placeResetAt(ParamResetLooperVolume1 + track * 3, cursor, rowY,
                         volumeAreaWidth);
            cursor += volumeAreaWidth + gap;
            MoveWindow(g_looperFreeLength[track], cursor, rowY, freeWidth,
                       controlHeight, TRUE);
            placeResetAt(ParamResetLooperFree1 + track * 3, cursor, rowY,
                         freeAreaWidth);
            cursor += freeAreaWidth + gap;
            MoveWindow(g_looperStopAtEnd[track], cursor, rowY, stopWidth,
                       controlHeight, TRUE);
            placeResetAt(ParamResetLooperStop1 + track * 3, cursor, rowY,
                         stopAreaWidth);
            rowY += looperRowStep;
        }
        MoveWindow(g_looperProgress, x, y + Scale(24), width,
                   looperRowStep * apo::kLooperTrackCount, TRUE);
        MoveWindow(g_looperNote, x, rowY, width, Scale(40), TRUE);
        break;
    }
    case apo::EffectType::Tuner: {
        const int rightX = x + columnWidth + columnGap;
        MoveWindow(g_tunerAlgorithmLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_tunerAlgorithmCombo, x + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetTunerAlgorithm, x, y);
        MoveWindow(g_tunerSampleSizeLabel, rightX, y, labelWidth, Scale(20),
                   TRUE);
        MoveWindow(g_tunerSampleSizeCombo, rightX + labelWidth, y, sliderWidth,
                   Scale(200), TRUE);
        placeReset(ParamResetTunerSampleSize, rightX, y);
        y += rowStep;
        placePair(g_tunerSmoothingLabel, g_tunerSmoothingSlider,
                  ParamResetTunerSmoothing, g_tunerReferenceLabel,
                  g_tunerReferenceSlider, ParamResetTunerReference);
        break;
    }
    default:
        break;
    }
}

void LayoutEffects(int x, int y, int width, int height)
{
    const int gap = Scale(8);
    const int labelWidth = Scale(60);
    const int comboWidth = Scale(300);

    MoveWindow(g_effectDeviceLabel, x, y + Scale(3), labelWidth, Scale(20),
               TRUE);
    MoveWindow(g_effectDeviceCombo, x + labelWidth, y, comboWidth,
               Scale(200), TRUE);

    // Enable is the primary action, so it sits directly after the device
    // selector. Listening is the next action, immediately after Enable.
    int nextX = x + labelWidth + comboWidth + Scale(16);
    MoveWindow(g_mainEnable, nextX, y + Scale(1), Scale(96), Scale(24), TRUE);
    // Checkbox text starts after the box, so account for that offset.
    PlaceInfoIcon(g_mainEnableIcon, g_mainEnable, nextX, y + Scale(1),
                  Scale(16), Scale(24), nextX + Scale(96), Scale(20));
    nextX += Scale(96) + Scale(16);
    MoveWindow(g_listen, nextX, y + Scale(1), Scale(230), Scale(24), TRUE);
    nextX += Scale(230) + Scale(16);

    const int presetLabelWidth = Scale(52);
    const int presetComboWidth = Scale(64);
    MoveWindow(g_effectPresetLabel, nextX, y + Scale(3), presetLabelWidth,
               Scale(20), TRUE);
    MoveWindow(g_effectPresetCombo, nextX + presetLabelWidth, y,
               presetComboWidth, Scale(200), TRUE);

    const int top = y + Scale(34);

    const int panelHeight = height - (top - y);
    const int listWidth = Scale(220);

    MoveWindow(g_effectListGroup, x, top, listWidth, panelHeight, TRUE);
    const int addHeight = Scale(30);
    const int addGap = Scale(8);
    MoveWindow(g_effectList, x + Scale(12), top + Scale(24),
               listWidth - Scale(24),
               panelHeight - Scale(24) - addHeight - addGap - Scale(12), TRUE);
    MoveWindow(g_effectAdd, x + Scale(12),
               top + panelHeight - Scale(12) - addHeight,
               listWidth - Scale(24), addHeight, TRUE);

    const int paramX = x + listWidth + gap;
    const int paramWidth = width - listWidth - gap;
    MoveWindow(g_effectGroup, paramX, top, paramWidth, panelHeight, TRUE);

    const int innerX = paramX + Scale(14);
    const int innerY = top + Scale(24);
    const int innerWidth = paramWidth - Scale(28);
    MoveWindow(g_effectHeading, innerX, innerY, innerWidth, Scale(22), TRUE);

    // The graph sits directly below the title and spans the panel width. Only
    // effect types with a curve reserve that space; the rest keep the full
    // height for their controls.
    int paramsY = innerY + Scale(28);
    if (ui::GraphSupported(g_effectType)) {
        const int graphTop = paramsY;
        const int available = panelHeight - (graphTop - top) - Scale(12);
        int graphHeight =
            std::clamp(available * 2 / 5, Scale(110), Scale(180));
        graphHeight = std::min(graphHeight, std::max(available, Scale(60)));
        MoveWindow(g_effectGraph, innerX, graphTop, innerWidth, graphHeight,
                   TRUE);
        paramsY = graphTop + graphHeight + Scale(10);
    }
    LayoutEffectParams(innerX, paramsY, innerWidth);

    ResizeEffectColumns();
}

} // namespace

void ResizeColumns()
{
    ResizeEffectColumns();
}

void CreateControls(HWND root)
{
    // Every main-screen control is a child of this panel. Keeping them off the
    // root window gives them a stable background and makes repaint handling a
    // single pass.
    g_pageBackground = CreateWindowExW(
        0, kPageClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0,
        root, nullptr, g_instance, nullptr);
    HWND window = g_pageBackground;

    g_infoTooltip = CreateWindowExW(
        0, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, root, nullptr, g_instance, nullptr);
    if (g_infoTooltip != nullptr) {
        ui::ApplyNativeTheme(g_infoTooltip);
        SendMessageW(g_infoTooltip, TTM_SETMAXTIPWIDTH, 0, Scale(240));
        SendMessageW(g_infoTooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 250);
    }

    g_effectDeviceLabel =
        ui::Label(window, IDC_EFFECT_DEVICE_LABEL, L"Device:");
    g_effectDeviceCombo =
        ui::ComboBox(window, IDC_EFFECT_DEVICE_COMBO, CBS_DROPDOWNLIST);
    g_mainEnable = ui::CheckBox(window, IDC_MAIN_ENABLE, L"Enable");
    ui::ApplyFont(g_mainEnable, g_boldFont);
    g_mainEnableIcon =
        InfoIcon(window, IDC_MAIN_ENABLE_ICON, InfoIconKind::Interrupt);
    AddTooltip(g_mainEnableIcon,
               L"Turning this on or off briefly interrupts audio.");
    g_effectPresetLabel =
        ui::Label(window, IDC_EFFECT_PRESET_LABEL, L"Preset:");
    g_effectPresetCombo =
        ui::ComboBox(window, IDC_EFFECT_PRESET_COMBO, CBS_DROPDOWNLIST);
    for (int i = 0; i < apo::kPresetCount; ++i) {
        const std::wstring text = std::to_wstring(i + 1);
        SendMessageW(g_effectPresetCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(g_effectPresetCombo, CB_SETCURSEL, 0, 0);

    g_effectListGroup = ui::GroupFrame(window, IDC_EFFECT_LIST_GROUP, L"Effects");
    g_effectList = ui::ReportList(
        window, IDC_EFFECT_LIST,
        LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER);
    ListView_SetExtendedListViewStyle(
        g_effectList,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ui::ApplyListStateImages(g_effectList);
    AddColumn(g_effectList, 0, L"Effect", 180);
    AddColumn(g_effectList, 1, L"", 28, LVCFMT_CENTER);
    g_effectAdd = ui::Button(window, IDC_EFFECT_ADD, L"Add effect");
    ui::ApplyFont(g_effectAdd, g_boldFont);

    g_effectGroup =
        ui::GroupFrame(window, IDC_EFFECT_GROUP, L"Effect parameters");
    g_effectHeading = ui::Label(window, IDC_EFFECT_HEADING, L"Noise reduction");
    ui::ApplyFont(g_effectHeading, g_boldFont);
    g_effectGraph = ui::Graph(window, IDC_EFFECT_GRAPH);
    g_inputTreatMono =
        ui::CheckBox(window, IDC_INPUT_TREAT_MONO, L"Treat as mono");
    AddTooltip(g_inputTreatMono,
               L"Copy the first channel to every other channel before the "
               L"other effects run.");
    g_distFunctionLabel =
        ui::Label(window, IDC_DIST_FUNCTION_LABEL, L"Function");
    g_distFunctionCombo =
        ui::ComboBox(window, IDC_DIST_FUNCTION_COMBO, CBS_DROPDOWNLIST);
    for (int i = 0; i < apo::kDistortionFunctionCount; ++i) {
        SendMessageW(g_distFunctionCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         apo::DistortionFunctionName(i)));
    }
    SendMessageW(g_distFunctionCombo, CB_SETCURSEL, apo::kDistortionTanh, 0);

    g_distDriveLabel = ui::Label(window, IDC_DIST_DRIVE_LABEL, L"Drive (dB)");
    g_distDriveSlider = ui::Slider(window, IDC_DIST_DRIVE_SLIDER,
                                   {0.0, 36.0, 0.5, false, 1});
    g_distCurvatureLabel =
        ui::Label(window, IDC_DIST_CURVATURE_LABEL, L"Curvature (%)");
    g_distCurvatureSlider = ui::Slider(window, IDC_DIST_CURVATURE_SLIDER,
                                       {0.0, 100.0, 1.0, false, 0});
    g_distOutputLabel =
        ui::Label(window, IDC_DIST_OUTPUT_LABEL, L"Output (dB)");
    g_distOutputSlider = ui::Slider(window, IDC_DIST_OUTPUT_SLIDER,
                                    {-24.0, 12.0, 0.5, false, 1});

    g_dynamicsInputLabel =
        ui::Label(window, IDC_DYN_INPUT_LABEL, L"Input (dB)");
    g_dynamicsInputSlider = ui::Slider(window, IDC_DYN_INPUT_SLIDER,
                                       {-24.0, 30.0, 0.5, false, 1});
    g_dynamicsThresholdLabel =
        ui::Label(window, IDC_DYN_THRESHOLD_LABEL, L"Threshold (dBFS)");
    g_dynamicsThresholdSlider = ui::Slider(
        window, IDC_DYN_THRESHOLD_SLIDER,
        {apo::kMinThresholdDb, apo::kMaxThresholdDb, 1.0, false, 0});
    g_dynamicsRatioLabel = ui::Label(window, IDC_DYN_RATIO_LABEL, L"Ratio");
    g_dynamicsRatioSlider = ui::Slider(
        window, IDC_DYN_RATIO_SLIDER,
        {apo::kDynamicsMinRatio, apo::kDynamicsMaxFiniteRatio, 0.0, true, 0,
         true});
    g_dynamicsAttackLabel =
        ui::Label(window, IDC_DYN_ATTACK_LABEL, L"Attack (ms)");
    g_dynamicsAttackSlider = ui::Slider(window, IDC_DYN_ATTACK_SLIDER,
                                        {0.1, 1000.0, 0.0, true, 0});
    g_dynamicsReleaseLabel =
        ui::Label(window, IDC_DYN_RELEASE_LABEL, L"Release (ms)");
    g_dynamicsReleaseSlider = ui::Slider(window, IDC_DYN_RELEASE_SLIDER,
                                         {1.0, 5000.0, 0.0, true, 0});
    g_dynamicsHoldLabel =
        ui::Label(window, IDC_DYN_HOLD_LABEL, L"Hold (ms)");
    g_dynamicsHoldSlider = ui::Slider(window, IDC_DYN_HOLD_SLIDER,
                                      {0.0, 2000.0, 1.0, false, 0});
    g_dynamicsOutputLabel =
        ui::Label(window, IDC_DYN_OUTPUT_LABEL, L"Output (dB)");
    g_dynamicsOutputSlider = ui::Slider(window, IDC_DYN_OUTPUT_SLIDER,
                                        {-24.0, 24.0, 0.5, false, 1});
    g_dynamicsLookaheadLabel =
        ui::Label(window, IDC_DYN_LOOKAHEAD_LABEL, L"Lookahead (ms)");
    g_dynamicsLookaheadSlider = ui::Slider(window, IDC_DYN_LOOKAHEAD_SLIDER,
                                           {0.0, 300.0, 1.0, false, 0});
    g_dynamicsLookaheadIcon =
        InfoIcon(window, IDC_DYN_LOOKAHEAD_ICON, InfoIconKind::Latency);
    AddTooltip(g_dynamicsLookaheadIcon,
               L"Adds delay to the audio stream.");

    g_inputGainLabel = ui::Label(window, IDC_INPUT_GAIN_LABEL,
                                 L"Input gain (dB)");
    g_inputGainSlider = ui::Slider(window, IDC_INPUT_GAIN_SLIDER,
                                   {apo::kInputMinGainDb,
                                    apo::kInputMaxGainDb, 0.5, false, 1});

    g_gainLabel = ui::Label(window, IDC_GAIN_LABEL, L"Gain (dB)");
    g_gainSlider = ui::Slider(window, IDC_GAIN_SLIDER,
                              {-24.0, 50.0, 0.5, false, 1});

    g_eqDynamicLabel = ui::Label(window, IDC_EQ_DYNAMIC_LABEL, L"Dynamic");
    g_eqDynamicCombo =
        ui::ComboBox(window, IDC_EQ_DYNAMIC_COMBO, CBS_DROPDOWNLIST);
    for (int mode = 0;
         mode < static_cast<int>(apo::EqualizerDynamicsMode::Count); ++mode) {
        SendMessageW(
            g_eqDynamicCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(apo::EqualizerDynamicsModeName(
                static_cast<apo::EqualizerDynamicsMode>(mode))));
    }
    SendMessageW(g_eqDynamicCombo, CB_SETCURSEL,
                 static_cast<int>(apo::EqualizerDynamicsMode::None), 0);

    g_eqFrequencyLabel =
        ui::Label(window, IDC_EQ_FREQUENCY_LABEL, L"Frequency (Hz)");
    g_eqFrequencySlider = ui::Slider(window, IDC_EQ_FREQUENCY_SLIDER,
                                      {apo::kEqMinFrequencyHz,
                                       apo::kEqMaxFrequencyHz, 0.0, true, 0});
    g_eqQLabel = ui::Label(window, IDC_EQ_Q_LABEL, L"Q");
    g_eqQSlider = ui::Slider(window, IDC_EQ_Q_SLIDER,
                              {apo::kEqMinQ, apo::kEqMaxQ, 0.0, true, 0});
    g_eqThresholdLabel =
        ui::Label(window, IDC_EQ_THRESHOLD_LABEL, L"Threshold (dBFS)");
    g_eqThresholdSlider = ui::Slider(
        window, IDC_EQ_THRESHOLD_SLIDER,
        {apo::kMinThresholdDb, apo::kMaxThresholdDb, 1.0, false, 0});
    g_eqRatioLabel = ui::Label(window, IDC_EQ_RATIO_LABEL, L"Ratio (:1)");
    g_eqRatioSlider =
        ui::Slider(window, IDC_EQ_RATIO_SLIDER,
                   {1.0, apo::kDynamicsMaxFiniteRatio, 0.0, true, 0, true});
    g_eqRangeLabel = ui::Label(window, IDC_EQ_RANGE_LABEL, L"Range (dB)");
    g_eqRangeSlider = ui::Slider(window, IDC_EQ_RANGE_SLIDER,
                                  {0.0, apo::kEqMaxRangeDb, 1.0, false, 0});
    g_eqLevelLabel = ui::Label(window, IDC_EQ_LEVEL_LABEL, L"Level (dB)");
    g_eqLevelSlider = ui::Slider(window, IDC_EQ_LEVEL_SLIDER,
                                  {apo::kEqMinLevelDb, apo::kEqMaxLevelDb,
                                   0.5, false, 1});
    g_eqAttackLabel =
        ui::Label(window, IDC_EQ_ATTACK_LABEL, L"Attack (ms)");
    g_eqAttackSlider = ui::Slider(window, IDC_EQ_ATTACK_SLIDER,
                                   {0.1, 1000.0, 0.0, true, 0});
    g_eqReleaseLabel =
        ui::Label(window, IDC_EQ_RELEASE_LABEL, L"Release (ms)");
    g_eqReleaseSlider = ui::Slider(window, IDC_EQ_RELEASE_SLIDER,
                                    {1.0, 5000.0, 0.0, true, 0});
    g_eqHoldLabel = ui::Label(window, IDC_EQ_HOLD_LABEL, L"Hold (ms)");
    g_eqHoldSlider = ui::Slider(window, IDC_EQ_HOLD_SLIDER,
                                 {0.0, apo::kEqMaxHoldMs, 1.0, false, 0});
    g_eqLookaheadLabel =
        ui::Label(window, IDC_EQ_LOOKAHEAD_LABEL, L"Lookahead (ms)");
    g_eqLookaheadSlider = ui::Slider(window, IDC_EQ_LOOKAHEAD_SLIDER,
                                      {0.0, apo::kDynamicsMaxLookaheadMs, 1.0,
                                       false, 0});
    g_eqLookaheadIcon =
        InfoIcon(window, IDC_EQ_LOOKAHEAD_ICON, InfoIconKind::Latency);
    AddTooltip(g_eqLookaheadIcon, L"Adds delay to the audio stream.");

    g_reverbAlgorithmLabel =
        ui::Label(window, IDC_REVERB_ALGORITHM_LABEL, L"Algorithm");
    g_reverbAlgorithmCombo =
        ui::ComboBox(window, IDC_REVERB_ALGORITHM_COMBO, CBS_DROPDOWNLIST);
    for (int algorithm = 0;
         algorithm < static_cast<int>(apo::ReverbAlgorithm::Count);
         ++algorithm) {
        SendMessageW(g_reverbAlgorithmCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(apo::ReverbAlgorithmName(
                         static_cast<apo::ReverbAlgorithm>(algorithm))));
    }
    SendMessageW(g_reverbAlgorithmCombo, CB_SETCURSEL,
                 static_cast<int>(apo::ReverbAlgorithm::CombAllpass), 0);

    g_reverbLevelLabel =
        ui::Label(window, IDC_REVERB_LEVEL_LABEL, L"Level (%)");
    g_reverbLevelSlider = ui::Slider(window, IDC_REVERB_LEVEL_SLIDER,
                                     {0.0, 100.0, 1.0, false, 0});
    g_reverbMaxLengthLabel =
        ui::Label(window, IDC_REVERB_MAX_LENGTH_LABEL, L"Max length (s)");
    g_reverbMaxLengthSlider = ui::Slider(
        window, IDC_REVERB_MAX_LENGTH_SLIDER,
        {apo::kReverbMinMaxLengthMs / 1000.0,
         apo::kReverbMaxMaxLengthMs / 1000.0, 0.1, false, 1});
    g_reverbSizeLabel = ui::Label(window, IDC_REVERB_SIZE_LABEL, L"Size (%)");
    g_reverbSizeSlider = ui::Slider(window, IDC_REVERB_SIZE_SLIDER,
                                    {0.0, 100.0, 1.0, false, 0});
    g_reverbDampingLabel =
        ui::Label(window, IDC_REVERB_DAMPING_LABEL, L"Damping (%)");
    g_reverbDampingSlider = ui::Slider(window, IDC_REVERB_DAMPING_SLIDER,
                                       {0.0, 100.0, 1.0, false, 0});
    g_reverbWidthLabel =
        ui::Label(window, IDC_REVERB_WIDTH_LABEL, L"Width (%)");
    g_reverbWidthSlider = ui::Slider(window, IDC_REVERB_WIDTH_SLIDER,
                                     {0.0, 100.0, 1.0, false, 0});
    g_reverbDensityLabel =
        ui::Label(window, IDC_REVERB_DENSITY_LABEL, L"Density (%)");
    g_reverbDensitySlider = ui::Slider(window, IDC_REVERB_DENSITY_SLIDER,
                                       {0.0, 100.0, 1.0, false, 0});
    g_reverbPredelayLabel =
        ui::Label(window, IDC_REVERB_PREDELAY_LABEL, L"Predelay (ms)");
    g_reverbPredelaySlider = ui::Slider(window, IDC_REVERB_PREDELAY_SLIDER,
                                        {0.0, 200.0, 1.0, false, 0});
    g_reverbPredelayIcon =
        InfoIcon(window, IDC_REVERB_PREDELAY_ICON, InfoIconKind::Latency);
    AddTooltip(g_reverbPredelayIcon,
               L"Adds delay to the audio stream.");

    g_echoModeLabel = ui::Label(window, IDC_ECHO_MODE_LABEL, L"Mode");
    g_echoModeCombo =
        ui::ComboBox(window, IDC_ECHO_MODE_COMBO, CBS_DROPDOWNLIST);
    for (int mode = 0; mode < static_cast<int>(apo::EchoMode::Count);
         ++mode) {
        SendMessageW(g_echoModeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         apo::EchoModeName(static_cast<apo::EchoMode>(mode))));
    }
    SendMessageW(g_echoModeCombo, CB_SETCURSEL,
                 static_cast<int>(apo::EchoMode::Stereo), 0);

    g_echoTimeLabel =
        ui::Label(window, IDC_ECHO_TIME_LABEL, L"Time (ms)");
    g_echoTimeSlider = ui::Slider(
        window, IDC_ECHO_TIME_SLIDER,
        {apo::kEchoMinTimeMs, apo::kEchoMaxTimeMs, 1.0, false, 0});
    g_echoFeedbackLabel =
        ui::Label(window, IDC_ECHO_FEEDBACK_LABEL, L"Feedback (%)");
    g_echoFeedbackSlider = ui::Slider(
        window, IDC_ECHO_FEEDBACK_SLIDER,
        {0.0, apo::kEchoMaxFeedback, 1.0, false, 0});
    g_echoDampingLabel =
        ui::Label(window, IDC_ECHO_DAMPING_LABEL, L"Damping (%)");
    g_echoDampingSlider = ui::Slider(window, IDC_ECHO_DAMPING_SLIDER,
                                     {0.0, 100.0, 1.0, false, 0});
    g_echoLevelLabel = ui::Label(window, IDC_ECHO_LEVEL_LABEL, L"Level (%)");
    g_echoLevelSlider = ui::Slider(window, IDC_ECHO_LEVEL_SLIDER,
                                   {0.0, 100.0, 1.0, false, 0});

    g_chorusVoicesLabel =
        ui::Label(window, IDC_CHORUS_VOICES_LABEL, L"Voices");
    g_chorusVoicesSlider = ui::Slider(
        window, IDC_CHORUS_VOICES_SLIDER,
        {apo::kChorusMinVoices, apo::kChorusMaxVoices, 1.0, false, 0});
    g_chorusRateLabel =
        ui::Label(window, IDC_CHORUS_RATE_LABEL, L"Rate (Hz)");
    g_chorusRateSlider = ui::Slider(
        window, IDC_CHORUS_RATE_SLIDER,
        {apo::kChorusMinRateHz, apo::kChorusMaxRateHz, 0.01, false, 2});
    g_chorusDepthLabel =
        ui::Label(window, IDC_CHORUS_DEPTH_LABEL, L"Depth (ms)");
    g_chorusDepthSlider = ui::Slider(
        window, IDC_CHORUS_DEPTH_SLIDER,
        {0.0, apo::kChorusMaxDepthMs, 0.1, false, 1});
    g_chorusWidthLabel =
        ui::Label(window, IDC_CHORUS_WIDTH_LABEL, L"Width (%)");
    g_chorusWidthSlider = ui::Slider(window, IDC_CHORUS_WIDTH_SLIDER,
                                     {0.0, 100.0, 1.0, false, 0});
    g_chorusLevelLabel =
        ui::Label(window, IDC_CHORUS_LEVEL_LABEL, L"Level (%)");
    g_chorusLevelSlider = ui::Slider(window, IDC_CHORUS_LEVEL_SLIDER,
                                     {0.0, 100.0, 1.0, false, 0});

    g_pitchAlgorithmLabel =
        ui::Label(window, IDC_PITCH_ALGORITHM_LABEL, L"Algorithm");
    g_pitchAlgorithmCombo =
        ui::ComboBox(window, IDC_PITCH_ALGORITHM_COMBO, CBS_DROPDOWNLIST);
    for (int algorithm = 0;
         algorithm < static_cast<int>(apo::PitchAlgorithm::Count);
         ++algorithm) {
        SendMessageW(g_pitchAlgorithmCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(apo::PitchAlgorithmName(
                         static_cast<apo::PitchAlgorithm>(algorithm))));
    }
    SendMessageW(g_pitchAlgorithmCombo, CB_SETCURSEL,
                 static_cast<int>(apo::PitchAlgorithm::Granular), 0);

    g_pitchShiftLabel =
        ui::Label(window, IDC_PITCH_SHIFT_LABEL, L"Shift (semitones)");
    g_pitchShiftSlider = ui::Slider(
        window, IDC_PITCH_SHIFT_SLIDER,
        {apo::kPitchMinSemitones, apo::kPitchMaxSemitones, 0.1, false, 1});
    g_pitchWindowLabel =
        ui::Label(window, IDC_PITCH_WINDOW_LABEL, L"Window (samples)");
    g_pitchWindowSlider = ui::Slider(
        window, IDC_PITCH_WINDOW_SLIDER,
        {apo::kPitchMinWindowSamples, apo::kPitchMaxWindowSamples, 1.0, false,
         0});
    g_pitchDryLevelLabel =
        ui::Label(window, IDC_PITCH_DRY_LEVEL_LABEL, L"Dry level (%)");
    g_pitchDryLevelSlider =
        ui::Slider(window, IDC_PITCH_DRY_LEVEL_SLIDER,
                   {0.0, 100.0, 1.0, false, 0});

    g_pitchLevelLabel =
        ui::Label(window, IDC_PITCH_LEVEL_LABEL, L"Level (%)");
    g_pitchLevelSlider = ui::Slider(window, IDC_PITCH_LEVEL_SLIDER,
                                    {0.0, 100.0, 1.0, false, 0});

    g_tremoloRateLabel =
        ui::Label(window, IDC_TREMOLO_RATE_LABEL, L"Rate (Hz)");
    g_tremoloRateSlider = ui::Slider(
        window, IDC_TREMOLO_RATE_SLIDER,
        {apo::kModulationMinRateHz, apo::kModulationMaxRateHz, 0.1, false, 1});
    g_tremoloDepthLabel =
        ui::Label(window, IDC_TREMOLO_DEPTH_LABEL, L"Depth (%)");
    g_tremoloDepthSlider = ui::Slider(window, IDC_TREMOLO_DEPTH_SLIDER,
                                      {0.0, 100.0, 1.0, false, 0});
    g_tremoloWaveformLabel =
        ui::Label(window, IDC_TREMOLO_WAVEFORM_LABEL, L"Waveform");
    g_tremoloWaveformCombo =
        ui::ComboBox(window, IDC_TREMOLO_WAVEFORM_COMBO, CBS_DROPDOWNLIST);
    g_vibratoWaveformLabel =
        ui::Label(window, IDC_VIBRATO_WAVEFORM_LABEL, L"Waveform");
    g_vibratoWaveformCombo =
        ui::ComboBox(window, IDC_VIBRATO_WAVEFORM_COMBO, CBS_DROPDOWNLIST);
    for (int waveform = 0;
         waveform < static_cast<int>(apo::ModulationWaveform::Count);
         ++waveform) {
        const LPARAM name = reinterpret_cast<LPARAM>(
            apo::ModulationWaveformName(
                static_cast<apo::ModulationWaveform>(waveform)));
        SendMessageW(g_tremoloWaveformCombo, CB_ADDSTRING, 0, name);
        SendMessageW(g_vibratoWaveformCombo, CB_ADDSTRING, 0, name);
    }
    SendMessageW(g_tremoloWaveformCombo, CB_SETCURSEL,
                 static_cast<int>(apo::ModulationWaveform::Sine), 0);
    SendMessageW(g_vibratoWaveformCombo, CB_SETCURSEL,
                 static_cast<int>(apo::ModulationWaveform::Sine), 0);
    g_tremoloSpreadLabel =
        ui::Label(window, IDC_TREMOLO_SPREAD_LABEL, L"Stereo spread (%)");
    g_tremoloSpreadSlider = ui::Slider(
        window, IDC_TREMOLO_SPREAD_SLIDER,
        {0.0, apo::kModulationMaxSpread, 1.0, false, 0});

    g_vibratoRateLabel =
        ui::Label(window, IDC_VIBRATO_RATE_LABEL, L"Rate (Hz)");
    g_vibratoRateSlider = ui::Slider(
        window, IDC_VIBRATO_RATE_SLIDER,
        {apo::kModulationMinRateHz, apo::kModulationMaxRateHz, 0.1, false, 1});
    g_vibratoDepthLabel =
        ui::Label(window, IDC_VIBRATO_DEPTH_LABEL, L"Depth (cents)");
    g_vibratoDepthSlider = ui::Slider(
        window, IDC_VIBRATO_DEPTH_SLIDER,
        {0.0, apo::kVibratoMaxDepthCents, 1.0, false, 0});
    g_vibratoSpreadLabel =
        ui::Label(window, IDC_VIBRATO_SPREAD_LABEL, L"Stereo spread (%)");
    g_vibratoSpreadSlider = ui::Slider(
        window, IDC_VIBRATO_SPREAD_SLIDER,
        {0.0, apo::kModulationMaxSpread, 1.0, false, 0});

    g_noiseReductionLabel =
        ui::Label(window, IDC_NOISE_REDUCTION_LABEL, L"Reduction (dB)");
    g_noiseReductionSlider = ui::Slider(window, IDC_NOISE_REDUCTION_SLIDER,
                                        {0.0, 50.0, 1.0, false, 0});
    g_noiseFftSizeLabel =
        ui::Label(window, IDC_NOISE_FFT_SIZE_LABEL, L"FFT size");
    g_noiseFftSizeCombo =
        ui::ComboBox(window, IDC_NOISE_FFT_SIZE_COMBO, CBS_DROPDOWNLIST);
    for (int size : apo::kNoiseFftSizes) {
        const std::wstring text = std::to_wstring(size);
        SendMessageW(g_noiseFftSizeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(g_noiseFftSizeCombo, CB_SETCURSEL,
                 apo::NoiseFftSizeIndex(apo::kNoiseDefaultFftSize), 0);
    g_noiseCapture =
        ui::Button(window, IDC_NOISE_CAPTURE, L"Capture noise profile");
    g_noiseNote = ui::Label(window, IDC_NOISE_NOTE, L"");

    g_tunerAlgorithmLabel =
        ui::Label(window, IDC_TUNER_ALGORITHM_LABEL, L"Algorithm");
    g_tunerAlgorithmCombo =
        ui::ComboBox(window, IDC_TUNER_ALGORITHM_COMBO, CBS_DROPDOWNLIST);
    for (int algorithm = 0;
         algorithm < static_cast<int>(apo::TunerAlgorithm::Count); ++algorithm) {
        SendMessageW(g_tunerAlgorithmCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(apo::TunerAlgorithmName(
                         static_cast<apo::TunerAlgorithm>(algorithm))));
    }
    SendMessageW(g_tunerAlgorithmCombo, CB_SETCURSEL,
                 static_cast<int>(apo::TunerAlgorithm::Autocorrelation), 0);

    g_tunerSampleSizeLabel =
        ui::Label(window, IDC_TUNER_SAMPLE_SIZE_LABEL, L"Sample size");
    g_tunerSampleSizeCombo =
        ui::ComboBox(window, IDC_TUNER_SAMPLE_SIZE_COMBO, CBS_DROPDOWNLIST);
    for (int size : apo::kTunerSampleSizes) {
        const std::wstring text =
            std::to_wstring(size) + L" samples";
        SendMessageW(g_tunerSampleSizeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(g_tunerSampleSizeCombo, CB_SETCURSEL,
                 apo::TunerSampleSizeIndex(apo::kTunerDefaultSampleSize), 0);

    g_tunerSmoothingLabel =
        ui::Label(window, IDC_TUNER_SMOOTHING_LABEL, L"Smoothing (%)");
    g_tunerSmoothingSlider = ui::Slider(
        window, IDC_TUNER_SMOOTHING_SLIDER,
        {apo::kTunerMinSmoothing, apo::kTunerMaxSmoothing, 1.0, false, 0});
    g_tunerReferenceLabel =
        ui::Label(window, IDC_TUNER_REFERENCE_LABEL, L"Reference A4 (Hz)");
    g_tunerReferenceSlider = ui::Slider(
        window, IDC_TUNER_REFERENCE_SLIDER,
        {apo::kTunerMinReferenceHz, apo::kTunerMaxReferenceHz, 0.1, false, 1});

    g_looperHeaderTrack =
        ui::Label(window, IDC_LOOPER_HEADER_TRACK, L"Key");
    g_looperHeaderState =
        ui::Label(window, IDC_LOOPER_HEADER_STATE, L"State");
    g_looperHeaderVolume =
        ui::Label(window, IDC_LOOPER_HEADER_VOLUME, L"Volume");
    g_looperHeaderFree =
        ui::Label(window, IDC_LOOPER_HEADER_FREE, L"Free length");
    g_looperHeaderStop =
        ui::Label(window, IDC_LOOPER_HEADER_STOP, L"Stop at loop end");
    for (int track = 0; track < apo::kLooperTrackCount; ++track) {
        const wchar_t* key = L"?";
        if (track == 0) {
            key = L"1";
        } else if (track == 1) {
            key = L"2";
        } else if (track == 2) {
            key = L"3";
        } else if (track == 3) {
            key = L"4";
        } else if (track == 4) {
            key = L"Space";
        }

        HWND button =
            ui::Button(window, IDC_LOOPER_BUTTON_1 + track, key);
        SetWindowSubclass(button, LooperGestureProc,
                          kLooperGestureSubclassId,
                          static_cast<DWORD_PTR>(track));
        g_looperButtons[track] = button;
        g_looperStateLabels[track] = ui::Label(
            window, IDC_LOOPER_STATE_1 + track, L"Empty", SS_CENTER);
        g_looperVolumeSliders[track] = ui::Slider(
            window, IDC_LOOPER_VOLUME_1 + track,
            {0.0, 100.0, 1.0, false, 0});
        g_looperFreeLength[track] = ui::CheckBox(
            window, IDC_LOOPER_FREE_LENGTH_1 + track, L"Free length");
        g_looperStopAtEnd[track] = ui::CheckBox(
            window, IDC_LOOPER_STOP_END_1 + track, L"Stop at loop end");
    }
    g_looperNote = ui::Label(
        window, IDC_LOOPER_NOTE,
        L"Tap: record / finish / overdub. Recording wraps and layers. "
        L"Double tap: stop. Long press: undo / clear.");
    g_looperProgress = ui::LooperProgress(window, IDC_LOOPER_PROGRESS);

    AddTooltip(g_looperFreeLength[0],
               L"Keep the exact recorded length; otherwise this track snaps "
               L"to a multiple of the first synced take.");
    AddTooltip(g_looperStopAtEnd[0],
               L"Defer a double-tap stop until this loop reaches its end.");
    AddTooltip(g_looperNote,
               L"Tap under ~400 ms. Press twice within ~250 ms for a double "
               L"tap, or hold about 600 ms. Keys 1-4 and Space start or finish "
               L"a take on the first press; a later double tap stops, and "
               L"holding a provisional start discards it. Takes are saved per device and restored after audio restarts and GUI reopen.");
    AddTooltip(g_looperProgress,
               L"Each line's width is proportional to its loop length; the "
               L"longest take fills the row. The filled part is the current "
               L"playhead: red records, amber overdubs, green plays, and a "
               L"dim outline is empty or stopped.");

    g_listen = ui::CheckBox(window, IDC_LISTEN, L"Listen to this device");

    CreateParamResetButtons(window);

    // Child windows are inserted at the top of the sibling z-order as they are
    // created, so each group box ended up in front of the controls it frames.
    // WS_CLIPSIBLINGS then clipped those controls away. Push every frame to
    // the bottom so its contents always draw above it.
    const HWND groupBoxes[] = {g_effectListGroup, g_effectGroup};
    for (HWND group : groupBoxes) {
        SetWindowPos(group, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    g_status = ui::Label(root, IDC_STATUS, L"");
    // Status and live levels share the bottom row of the root window, so the
    // meter is a sibling of the status instead of a child of the page panel.
    g_meter = ui::Label(root, IDC_METER, L"Peak: --  RMS: --", SS_RIGHT);

    RefreshInfoIcons();
}

void RefreshInfoIcons()
{
    const HWND icons[] = {g_mainEnableIcon, g_dynamicsLookaheadIcon,
                          g_reverbPredelayIcon};
    for (HWND icon : icons) {
        if (icon != nullptr) {
            InvalidateRect(icon, nullptr, TRUE);
        }
    }
}

void DestroyInfoIcons()
{
    const HWND icons[] = {g_mainEnableIcon, g_dynamicsLookaheadIcon,
                          g_reverbPredelayIcon};
    for (HWND icon : icons) {
        if (icon != nullptr) {
            RemoveWindowSubclass(icon, InfoIconProc, kInfoIconSubclassId);
        }
    }
    for (int track = 0; track < apo::kLooperTrackCount; ++track) {
        HWND button = g_looperButtons[track];
        if (button != nullptr) {
            KillTimer(button, kLooperLongPressTimer);
            KillTimer(button, kLooperSingleTapTimer);
            RemoveWindowSubclass(button, LooperGestureProc,
                                 kLooperGestureSubclassId);
        }
        g_looperGestures[track] = LooperGestureState{};
    }
}

void Layout(int width, int height)
{
    const int margin = Scale(8);
    const int padding = Scale(8);
    const int bottomHeight = Scale(24);

    const int contentWidth = width - 2 * margin;
    const int contentHeight = height - margin - bottomHeight;
    MoveWindow(g_pageBackground, margin, margin, contentWidth, contentHeight,
               TRUE);

    const int viewWidth = contentWidth - 2 * padding;
    const int viewHeight = contentHeight - 2 * padding;
    LayoutEffects(padding, padding, viewWidth, viewHeight);

    // One bottom row: status on the left, live levels on the right. Use the
    // same total inset as the view controls and center the text in the strip.
    const int rowHeight = Scale(20);
    const int rowY = height - bottomHeight + (bottomHeight - rowHeight) / 2;
    const int rowInset = margin + padding;
    const int meterWidth = Scale(320);
    const int meterX = width - rowInset - meterWidth;
    const int statusWidth = std::max(0, meterX - rowInset - padding);
    MoveWindow(g_status, rowInset, rowY, statusWidth, rowHeight, TRUE);
    MoveWindow(g_meter, meterX, rowY, meterWidth, rowHeight, TRUE);
}

void Relayout()
{
    if (g_window == nullptr) {
        return;
    }
    RECT rect{};
    GetClientRect(g_window, &rect);
    Layout(rect.right, rect.bottom);
    RepaintAll();
}

// Sibling controls do not always repaint when a dropdown closes over them.
// Redraw the window and every child after layout changes.
void RepaintAll()
{
    if (g_window == nullptr) {
        return;
    }
    // Every control paints its own background, so a separate erase pass only
    // adds a visible flash. Invalidate and let the paint handlers fill on the
    // next normal paint cycle instead of forcing an immediate repaint.
    RedrawWindow(g_window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN);
    if (g_pageBackground != nullptr) {
        RedrawWindow(g_pageBackground, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}
