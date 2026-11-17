#include "structure.h"

#include "apo.h"
#include "graph.h"
#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include <commctrl.h>

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
    if (g_latencyTooltip == nullptr || control == nullptr) {
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
    SendMessageW(g_latencyTooltip, TTM_ADDTOOLW, 0,
                 reinterpret_cast<LPARAM>(&info));
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
    ShowWindow(g_gateLookaheadIcon, SW_HIDE);
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

    auto placeReset = [&](int param, int columnX, int rowY) {
        if (param < 0 || param >= ParamResetCount) {
            return;
        }
        HWND button = g_paramResetButtons[param];
        MoveWindow(button, columnX + columnWidth - resetWidth, rowY,
                   resetWidth, controlHeight, TRUE);
        ShowWindow(button, SW_SHOW);
    };

    const int iconSize = Scale(20);
    auto placeIcon = [&](HWND icon, int columnX, int rowY) {
        if (icon == nullptr) {
            return;
        }
        MoveWindow(icon, columnX + labelWidth - iconSize, rowY - Scale(2),
                   iconSize, iconSize, TRUE);
        ShowWindow(icon, SW_SHOW);
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
    case apo::EffectType::Noise:
        MoveWindow(g_noiseReductionLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_noiseReductionSlider, x + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        placeReset(ParamResetNoiseReduction, x, y);
        MoveWindow(g_noiseCapture, x + columnWidth + columnGap, y,
                   columnWidth, Scale(28), TRUE);
        y += Scale(36);
        MoveWindow(g_noiseNote, x, y, width, Scale(36), TRUE);
        break;
    case apo::EffectType::Gate:
        placePair(g_gateThresholdLabel, g_gateThresholdSlider,
                  ParamResetGateThreshold, g_gateRangeLabel, g_gateRangeSlider,
                  ParamResetGateRange);
        placePair(g_gateAttackLabel, g_gateAttackSlider, ParamResetGateAttack,
                  g_gateHoldLabel, g_gateHoldSlider, ParamResetGateHold);
        placePair(g_gateReleaseLabel, g_gateReleaseSlider,
                  ParamResetGateRelease, g_gateLookaheadLabel,
                  g_gateLookaheadSlider, ParamResetGateLookahead);
        placeIcon(g_gateLookaheadIcon, x + columnWidth + columnGap,
                  y - rowStep);
        break;
    case apo::EffectType::Dynamic:
        MoveWindow(g_dynamicExtend, x, y,
                   columnWidth - resetWidth - resetGap, Scale(22), TRUE);
        placeReset(ParamResetDynExpand, x, y);
        y += Scale(30);
        placePair(g_dynFrequencyLabel, g_dynFrequencySlider,
                  ParamResetDynFrequency, g_dynQLabel, g_dynQSlider,
                  ParamResetDynQ);
        placePair(g_dynThresholdLabel, g_dynThresholdSlider,
                  ParamResetDynThreshold, g_dynRatioLabel, g_dynRatioSlider,
                  ParamResetDynRatio);
        placePair(g_dynRangeLabel, g_dynRangeSlider, ParamResetDynRange,
                  g_dynAttackLabel, g_dynAttackSlider, ParamResetDynAttack);
        placePair(g_dynReleaseLabel, g_dynReleaseSlider,
                  ParamResetDynRelease, nullptr, nullptr, 0);
        break;
    case apo::EffectType::Compressor:
        placePair(g_compThresholdLabel, g_compThresholdSlider,
                  ParamResetCompThreshold, g_compRatioLabel, g_compRatioSlider,
                  ParamResetCompRatio);
        placePair(g_compAttackLabel, g_compAttackSlider, ParamResetCompAttack,
                  g_compReleaseLabel, g_compReleaseSlider,
                  ParamResetCompRelease);
        break;
    case apo::EffectType::Gain:
        placePair(g_makeupLabel, g_makeupSlider, ParamResetMakeup, nullptr,
                  nullptr, 0);
        break;
    case apo::EffectType::Mono:
        MoveWindow(g_monoNote, x, y, width, Scale(40), TRUE);
        break;
    case apo::EffectType::Distortion:
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
        MoveWindow(g_distortionNote, x, y, width, Scale(44), TRUE);
        break;
    case apo::EffectType::Reverb:
        placePair(g_reverbMixLabel, g_reverbMixSlider, ParamResetReverbMix,
                  g_reverbSizeLabel, g_reverbSizeSlider,
                  ParamResetReverbSize);
        placePair(g_reverbDampingLabel, g_reverbDampingSlider,
                  ParamResetReverbDamping, g_reverbPredelayLabel,
                  g_reverbPredelaySlider, ParamResetReverbPredelay);
        placeIcon(g_reverbPredelayIcon, x + columnWidth + columnGap,
                  y - rowStep);
        break;
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
    nextX += Scale(96) + Scale(16);
    MoveWindow(g_listen, nextX, y + Scale(1), Scale(230), Scale(24), TRUE);
    nextX += Scale(230) + Scale(16);

    const int presetLabelWidth = Scale(52);
    const int presetComboWidth = Scale(64);
    MoveWindow(g_effectPresetLabel, nextX, y + Scale(3), presetLabelWidth,
               Scale(20), TRUE);
    MoveWindow(g_effectPresetCombo, nextX + presetLabelWidth, y,
               presetComboWidth, Scale(200), TRUE);

    MoveWindow(g_mainHint, x, y + Scale(30), width, Scale(20), TRUE);
    const int top = y + Scale(56);

    const int bottomHeight = Scale(48);
    const int panelHeight = height - (top - y) - bottomHeight;
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

    const int bottomY = top + panelHeight + gap;
    MoveWindow(g_meter, x, bottomY, Scale(320), Scale(22), TRUE);

    ResizeEffectColumns();
}

} // namespace

void ResizeColumns()
{
    ResizeEffectColumns();
}

void CreateControls(HWND root)
{
    g_title = ui::Label(root, IDC_TITLE, L"Better Micro APO");
    ui::ApplyFont(g_title, g_titleFont);
    g_refresh = ui::Button(root, IDC_REFRESH, L"Refresh");

    // Every main-screen control is a child of this panel. Keeping them off the
    // root window gives them a stable background and makes repaint handling a
    // single pass.
    g_pageBackground = CreateWindowExW(
        0, kPageClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0,
        root, nullptr, g_instance, nullptr);
    HWND window = g_pageBackground;

    g_latencyTooltip = CreateWindowExW(
        0, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, root, nullptr, g_instance, nullptr);
    if (g_latencyTooltip != nullptr) {
        ui::ApplyNativeTheme(g_latencyTooltip);
        SendMessageW(g_latencyTooltip, TTM_SETMAXTIPWIDTH, 0, Scale(240));
        SendMessageW(g_latencyTooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 250);
    }

    g_effectDeviceLabel =
        ui::Label(window, IDC_EFFECT_DEVICE_LABEL, L"Device:");
    g_effectDeviceCombo =
        ui::ComboBox(window, IDC_EFFECT_DEVICE_COMBO, CBS_DROPDOWNLIST);
    g_mainEnable = ui::CheckBox(window, IDC_MAIN_ENABLE, L"Enable");
    ui::ApplyFont(g_mainEnable, g_boldFont);
    g_mainHint = ui::Label(window, IDC_MAIN_HINT, L"");
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
    g_monoNote = ui::Label(
        window, IDC_MONO_NOTE,
        L"Copies the processed left channel to every other channel.");
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

    g_distortionNote = ui::Label(
        window, IDC_DISTORTION_NOTE,
        L"Drive pushes the signal into the curve; Output sets the level after "
        L"it.\r\nCurvature bends the curve from nearly linear to fully "
        L"saturated.");

    g_gateThresholdLabel =
        ui::Label(window, IDC_GATE_THRESHOLD_LABEL, L"Threshold (dBFS)");
    g_gateThresholdSlider = ui::Slider(
        window, IDC_GATE_THRESHOLD_SLIDER, {-80.0, 0.0, 1.0, false, 0});
    g_gateRangeLabel =
        ui::Label(window, IDC_GATE_RANGE_LABEL, L"Range (dB)");
    g_gateRangeSlider = ui::Slider(window, IDC_GATE_RANGE_SLIDER,
                                   {0.0, 96.0, 1.0, false, 0});
    g_gateAttackLabel =
        ui::Label(window, IDC_GATE_ATTACK_LABEL, L"Attack (ms)");
    g_gateAttackSlider = ui::Slider(window, IDC_GATE_ATTACK_SLIDER,
                                    {0.1, 1000.0, 0.0, true, 0});
    g_gateHoldLabel = ui::Label(window, IDC_GATE_HOLD_LABEL, L"Hold (ms)");
    g_gateHoldSlider = ui::Slider(window, IDC_GATE_HOLD_SLIDER,
                                  {0.0, 2000.0, 1.0, false, 0});
    g_gateReleaseLabel =
        ui::Label(window, IDC_GATE_RELEASE_LABEL, L"Release (ms)");
    g_gateReleaseSlider = ui::Slider(window, IDC_GATE_RELEASE_SLIDER,
                                     {1.0, 5000.0, 0.0, true, 0});
    g_gateLookaheadLabel =
        ui::Label(window, IDC_GATE_LOOKAHEAD_LABEL, L"Lookahead (ms)");
    g_gateLookaheadSlider = ui::Slider(window, IDC_GATE_LOOKAHEAD_SLIDER,
                                       {0.0, 300.0, 1.0, false, 0});
    g_gateLookaheadIcon = ui::OwnerDrawIcon(window, IDC_GATE_LOOKAHEAD_ICON);
    AddTooltip(g_gateLookaheadIcon,
               L"This parameter delays the audio stream by the set amount.");

    g_compThresholdLabel =
        ui::Label(window, IDC_COMP_THRESHOLD_LABEL, L"Threshold (dBFS)");
    g_compThresholdSlider = ui::Slider(
        window, IDC_COMP_THRESHOLD_SLIDER, {-60.0, 0.0, 1.0, false, 0});
    g_compRatioLabel =
        ui::Label(window, IDC_COMP_RATIO_LABEL, L"Ratio (:1)");
    g_compRatioSlider = ui::Slider(window, IDC_COMP_RATIO_SLIDER,
                                   {1.0, 20.0, 0.1, false, 1});
    g_compAttackLabel =
        ui::Label(window, IDC_COMP_ATTACK_LABEL, L"Attack (ms)");
    g_compAttackSlider = ui::Slider(window, IDC_COMP_ATTACK_SLIDER,
                                    {0.1, 1000.0, 0.0, true, 0});
    g_compReleaseLabel =
        ui::Label(window, IDC_COMP_RELEASE_LABEL, L"Release (ms)");
    g_compReleaseSlider = ui::Slider(window, IDC_COMP_RELEASE_SLIDER,
                                     {1.0, 5000.0, 0.0, true, 0});

    g_makeupLabel = ui::Label(window, IDC_MAKEUP_LABEL, L"Makeup (dB)");
    g_makeupSlider = ui::Slider(window, IDC_MAKEUP_SLIDER,
                                {-24.0, 48.0, 0.5, false, 1});

    g_dynamicExtend =
        ui::CheckBox(window, IDC_DYNAMIC_EXTEND, L"Extend quiet lows");
    g_dynFrequencyLabel =
        ui::Label(window, IDC_DYN_FREQUENCY_LABEL, L"Frequency (Hz)");
    g_dynFrequencySlider = ui::Slider(window, IDC_DYN_FREQUENCY_SLIDER,
                                      {20.0, 2000.0, 0.0, true, 0});
    g_dynQLabel = ui::Label(window, IDC_DYN_Q_LABEL, L"Q");
    g_dynQSlider = ui::Slider(window, IDC_DYN_Q_SLIDER,
                              {0.1, 10.0, 0.0, true, 0});
    g_dynThresholdLabel =
        ui::Label(window, IDC_DYN_THRESHOLD_LABEL, L"Threshold (dBFS)");
    g_dynThresholdSlider = ui::Slider(
        window, IDC_DYN_THRESHOLD_SLIDER, {-80.0, 0.0, 1.0, false, 0});
    g_dynRatioLabel = ui::Label(window, IDC_DYN_RATIO_LABEL, L"Ratio (:1)");
    g_dynRatioSlider = ui::Slider(window, IDC_DYN_RATIO_SLIDER,
                                  {1.0, 20.0, 0.1, false, 1});
    g_dynRangeLabel = ui::Label(window, IDC_DYN_RANGE_LABEL, L"Range (dB)");
    g_dynRangeSlider = ui::Slider(window, IDC_DYN_RANGE_SLIDER,
                                  {0.0, 36.0, 1.0, false, 0});
    g_dynAttackLabel =
        ui::Label(window, IDC_DYN_ATTACK_LABEL, L"Attack (ms)");
    g_dynAttackSlider = ui::Slider(window, IDC_DYN_ATTACK_SLIDER,
                                   {0.1, 1000.0, 0.0, true, 0});
    g_dynReleaseLabel =
        ui::Label(window, IDC_DYN_RELEASE_LABEL, L"Release (ms)");
    g_dynReleaseSlider = ui::Slider(window, IDC_DYN_RELEASE_SLIDER,
                                    {1.0, 5000.0, 0.0, true, 0});

    g_reverbMixLabel = ui::Label(window, IDC_REVERB_MIX_LABEL, L"Mix (%)");
    g_reverbMixSlider = ui::Slider(window, IDC_REVERB_MIX_SLIDER,
                                   {0.0, 100.0, 1.0, false, 0});
    g_reverbSizeLabel = ui::Label(window, IDC_REVERB_SIZE_LABEL, L"Size (%)");
    g_reverbSizeSlider = ui::Slider(window, IDC_REVERB_SIZE_SLIDER,
                                    {0.0, 100.0, 1.0, false, 0});
    g_reverbDampingLabel =
        ui::Label(window, IDC_REVERB_DAMPING_LABEL, L"Damping (%)");
    g_reverbDampingSlider = ui::Slider(window, IDC_REVERB_DAMPING_SLIDER,
                                       {0.0, 100.0, 1.0, false, 0});
    g_reverbPredelayLabel =
        ui::Label(window, IDC_REVERB_PREDELAY_LABEL, L"Predelay (ms)");
    g_reverbPredelaySlider = ui::Slider(window, IDC_REVERB_PREDELAY_SLIDER,
                                        {0.0, 200.0, 1.0, false, 0});
    g_reverbPredelayIcon = ui::OwnerDrawIcon(window, IDC_REVERB_PREDELAY_ICON);
    AddTooltip(g_reverbPredelayIcon,
               L"This parameter delays the audio stream by the set amount.");

    g_noiseReductionLabel =
        ui::Label(window, IDC_NOISE_REDUCTION_LABEL, L"Reduction (dB)");
    g_noiseReductionSlider = ui::Slider(window, IDC_NOISE_REDUCTION_SLIDER,
                                        {0.0, 70.0, 1.0, false, 0});
    g_noiseCapture =
        ui::Button(window, IDC_NOISE_CAPTURE, L"Capture noise profile");
    g_noiseNote = ui::Label(window, IDC_NOISE_NOTE, L"");

    g_listen = ui::CheckBox(window, IDC_LISTEN, L"Listen to this device");
    g_meter = ui::Label(window, IDC_METER, L"Peak: --  RMS: --");

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
}

void Layout(int width, int height)
{
    const int margin = Scale(16);
    int y = margin;

    MoveWindow(g_title, margin, y, Scale(320), Scale(26), TRUE);
    MoveWindow(g_refresh, width - margin - Scale(96), y, Scale(96),
               Scale(28), TRUE);
    y += Scale(40);

    const int statusHeight = Scale(28);
    const int contentWidth = width - 2 * margin;
    const int contentHeight = height - y - statusHeight;
    MoveWindow(g_pageBackground, margin, y, contentWidth, contentHeight, TRUE);

    const int contentPadding = Scale(12);
    const int viewWidth = contentWidth - 2 * contentPadding;
    const int viewHeight = contentHeight - 2 * contentPadding;
    LayoutEffects(contentPadding, contentPadding, viewWidth, viewHeight);

    const int statusY = height - statusHeight;
    MoveWindow(g_status, margin, statusY + (statusHeight - Scale(20)) / 2,
               width - 2 * margin, Scale(20), TRUE);
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
