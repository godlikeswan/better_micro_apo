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

void ResizeDeviceColumns()
{
    if (!g_list) {
        return;
    }

    RECT rect{};
    GetClientRect(g_list, &rect);
    int total = rect.right - rect.left - GetSystemMetrics(SM_CXVSCROLL) -
                Scale(2);
    const int stateWidth = Scale(90);
    const int apoWidth = Scale(130);
    const int notesWidth = Scale(170);
    int deviceWidth = total - stateWidth - apoWidth - notesWidth;
    if (deviceWidth < Scale(140)) {
        deviceWidth = Scale(140);
    }

    ListView_SetColumnWidth(g_list, 0, deviceWidth);
    ListView_SetColumnWidth(g_list, 1, stateWidth);
    ListView_SetColumnWidth(g_list, 2, apoWidth);
    ListView_SetColumnWidth(g_list, 3, notesWidth);
}

void ResizeEffectColumns()
{
    if (!g_effectList) {
        return;
    }

    RECT rect{};
    GetClientRect(g_effectList, &rect);
    int width = rect.right - rect.left - GetSystemMetrics(SM_CXVSCROLL) -
                Scale(2);
    const int deleteWidth = Scale(28);
    int nameWidth = width - deleteWidth;
    if (nameWidth < Scale(60)) {
        nameWidth = Scale(60);
    }
    ListView_SetColumnWidth(g_effectList, 0, nameWidth);
    ListView_SetColumnWidth(g_effectList, 1, deleteWidth);
}

void LayoutInstall(int x, int y, int width, int height)
{
    const int gap = Scale(8);
    int filesHeight = Scale(176);
    if (filesHeight > height) {
        filesHeight = height;
    }
    MoveWindow(g_groupFiles, x, y, width, filesHeight, TRUE);

    const int innerX = x + Scale(16);
    const int labelWidth = Scale(110);
    const int statusWidth = Scale(110);
    const int statusX = x + width - Scale(16) - statusWidth;
    const int valueWidth = statusX - (innerX + labelWidth) - Scale(12);
    int rowY = y + Scale(24);

    auto placeRow = [&](HWND label, HWND value, HWND status) {
        MoveWindow(label, innerX, rowY, labelWidth, Scale(20), TRUE);
        MoveWindow(value, innerX + labelWidth, rowY, valueWidth, Scale(20),
                   TRUE);
        MoveWindow(status, statusX, rowY, statusWidth, Scale(20), TRUE);
        rowY += Scale(26);
    };
    placeRow(g_dllLabel, g_dllValue, g_dllStatus);
    placeRow(g_comLabel, g_comValue, g_comStatus);
    placeRow(g_apoLabel, g_apoValue, g_apoStatus);
    MoveWindow(g_filesNote, innerX, rowY, statusX + statusWidth - innerX,
               Scale(20), TRUE);

    const int buttonWidth = Scale(110);
    const int buttonY = y + filesHeight - Scale(46);
    MoveWindow(g_overall, innerX, buttonY + Scale(6), Scale(280), Scale(22),
               TRUE);
    MoveWindow(g_install,
               x + width - Scale(16) - 2 * buttonWidth - gap, buttonY,
               buttonWidth, Scale(30), TRUE);
    MoveWindow(g_uninstall, x + width - Scale(16) - buttonWidth, buttonY,
               buttonWidth, Scale(30), TRUE);
}

void LayoutDevices(int x, int y, int width, int height)
{
    const int gap = Scale(8);
    MoveWindow(g_groupDevices, x, y, width, height, TRUE);

    const int innerX = x + Scale(16);
    const int innerWidth = width - Scale(32);
    const int buttonWidth = Scale(120);
    const int buttonY = y + height - Scale(46);
    const int listHeight = height - Scale(24) - Scale(88);

    MoveWindow(g_list, innerX, y + Scale(24), innerWidth, listHeight, TRUE);
    MoveWindow(g_isolate, innerX, y + height - Scale(78),
               innerWidth - Scale(260), Scale(22), TRUE);
    MoveWindow(g_devicesUnsaved, x + width - Scale(16) - Scale(240),
               y + height - Scale(78), Scale(240), Scale(22), TRUE);
    MoveWindow(g_hint, innerX, y + height - Scale(42),
               innerWidth - 2 * buttonWidth - gap - Scale(16), Scale(20),
               TRUE);
    MoveWindow(g_devicesReset,
               x + width - Scale(16) - 2 * buttonWidth - gap, buttonY,
               buttonWidth, Scale(30), TRUE);
    MoveWindow(g_devicesApply, x + width - Scale(16) - buttonWidth, buttonY,
               buttonWidth, Scale(30), TRUE);

    ResizeDeviceColumns();
}

void LayoutEffectParams(int x, int y, int width)
{
    const int columnGap = Scale(16);
    const int columnWidth = (width - columnGap) / 2;
    const int labelWidth = Scale(110);
    const int sliderWidth = std::max(Scale(50), columnWidth - labelWidth);
    const int controlHeight = Scale(26);
    const int rowStep = Scale(32);

    auto placePair = [&](HWND leftLabel, HWND leftSlider, HWND rightLabel,
                         HWND rightSlider) {
        MoveWindow(leftLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(leftSlider, x + labelWidth, y, sliderWidth, controlHeight,
                   TRUE);
        if (rightLabel != nullptr) {
            const int rightX = x + columnWidth + columnGap;
            MoveWindow(rightLabel, rightX, y, labelWidth, Scale(20), TRUE);
            MoveWindow(rightSlider, rightX + labelWidth, y, sliderWidth,
                       controlHeight, TRUE);
        }
        y += rowStep;
    };

    switch (g_effectType) {
    case apo::EffectType::Noise:
        MoveWindow(g_noiseReductionLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_noiseReductionSlider, x + labelWidth, y, sliderWidth,
                   controlHeight, TRUE);
        MoveWindow(g_noiseCapture, x + columnWidth + columnGap, y,
                   columnWidth, Scale(28), TRUE);
        y += Scale(36);
        MoveWindow(g_noiseNote, x, y, width, Scale(36), TRUE);
        break;
    case apo::EffectType::Gate:
        placePair(g_gateThresholdLabel, g_gateThresholdSlider,
                  g_gateRangeLabel, g_gateRangeSlider);
        placePair(g_gateAttackLabel, g_gateAttackSlider, g_gateHoldLabel,
                  g_gateHoldSlider);
        placePair(g_gateReleaseLabel, g_gateReleaseSlider,
                  g_gateLookaheadLabel, g_gateLookaheadSlider);
        break;
    case apo::EffectType::Dynamic:
        MoveWindow(g_dynamicExtend, x, y, columnWidth, Scale(22), TRUE);
        y += Scale(30);
        placePair(g_dynFrequencyLabel, g_dynFrequencySlider, g_dynQLabel,
                  g_dynQSlider);
        placePair(g_dynThresholdLabel, g_dynThresholdSlider, g_dynRatioLabel,
                  g_dynRatioSlider);
        placePair(g_dynRangeLabel, g_dynRangeSlider, g_dynAttackLabel,
                  g_dynAttackSlider);
        placePair(g_dynReleaseLabel, g_dynReleaseSlider, nullptr, nullptr);
        break;
    case apo::EffectType::Compressor:
        placePair(g_compThresholdLabel, g_compThresholdSlider,
                  g_compRatioLabel, g_compRatioSlider);
        placePair(g_compAttackLabel, g_compAttackSlider, g_compReleaseLabel,
                  g_compReleaseSlider);
        break;
    case apo::EffectType::Gain:
        placePair(g_makeupLabel, g_makeupSlider, nullptr, nullptr);
        break;
    case apo::EffectType::Mono:
        MoveWindow(g_monoNote, x, y, width, Scale(40), TRUE);
        break;
    case apo::EffectType::Distortion:
        MoveWindow(g_distFunctionLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_distFunctionCombo, x + labelWidth, y,
                   Scale(180), Scale(200), TRUE);
        y += Scale(34);
        placePair(g_distDriveLabel, g_distDriveSlider,
                  g_distCurvatureLabel, g_distCurvatureSlider);
        placePair(g_distOutputLabel, g_distOutputSlider, nullptr, nullptr);
        MoveWindow(g_distortionNote, x, y, width, Scale(44), TRUE);
        break;
    case apo::EffectType::Reverb:
        placePair(g_reverbMixLabel, g_reverbMixSlider, g_reverbSizeLabel,
                  g_reverbSizeSlider);
        placePair(g_reverbDampingLabel, g_reverbDampingSlider,
                  g_reverbPredelayLabel, g_reverbPredelaySlider);
        break;
    default:
        break;
    }
}

void LayoutEffects(int x, int y, int width, int height)
{
    const int gap = Scale(8);
    const int labelWidth = Scale(60);
    const int comboWidth = Scale(320);

    MoveWindow(g_effectDeviceLabel, x, y + Scale(3), labelWidth, Scale(20),
               TRUE);
    MoveWindow(g_effectDeviceCombo, x + labelWidth, y, comboWidth,
               Scale(200), TRUE);

    const int presetLabelWidth = Scale(52);
    const int presetComboWidth = Scale(64);
    const int presetX = x + labelWidth + comboWidth + Scale(16);
    MoveWindow(g_effectPresetLabel, presetX, y + Scale(3), presetLabelWidth,
               Scale(20), TRUE);
    MoveWindow(g_effectPresetCombo, presetX + presetLabelWidth, y,
               presetComboWidth, Scale(200), TRUE);

    const int top = y + Scale(34);
    const int bottomHeight = Scale(100);
    const int panelHeight = height - Scale(34) - bottomHeight;
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
    MoveWindow(g_listen, x, bottomY, Scale(230), Scale(22), TRUE);
    MoveWindow(g_meter, x + Scale(240), bottomY, Scale(320), Scale(22),
               TRUE);
    MoveWindow(g_effectsUnsaved, x + width - Scale(240), bottomY,
               Scale(240), Scale(22), TRUE);
    MoveWindow(g_listenNote, x, bottomY + Scale(24),
               width - 2 * Scale(120) - Scale(24), Scale(36), TRUE);

    const int buttonWidth = Scale(120);
    const int buttonY = y + height - Scale(46);
    MoveWindow(g_effectsReset,
               x + width - Scale(16) - 2 * buttonWidth - gap, buttonY,
               buttonWidth, Scale(30), TRUE);
    MoveWindow(g_effectsApply, x + width - Scale(16) - buttonWidth, buttonY,
               buttonWidth, Scale(30), TRUE);

    ResizeEffectColumns();
}

} // namespace

void ResizeColumns()
{
    ResizeDeviceColumns();
    ResizeEffectColumns();
}

void CreateControls(HWND root)
{
    // The tab and its header belong to the root window. Everything below the
    // tab header is created as a child of the page panel, so the tab can never
    // paint over the view controls.
    HWND window = root;

    g_title = ui::Label(window, IDC_TITLE, L"Better Micro APO");
    ui::ApplyFont(g_title, g_titleFont);
    g_refresh = ui::Button(window, IDC_REFRESH, L"Refresh");

    // The tab control must exist before the view controls so the view content
    // is drawn on top of its body area.
    g_tabs = ui::TabControl(window, IDC_TABS);
    const wchar_t* tabNames[] = {L"Install", L"Devices", L"Effects"};
    for (int i = 0; i < 3; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT | TCIF_PARAM;
        item.pszText = const_cast<wchar_t*>(tabNames[i]);
        // Availability lives in the item parameter so the custom tab painter
        // can gray out tabs that are not reachable yet.
        item.lParam = 1;
        TabCtrl_InsertItem(g_tabs, i, &item);
    }

    // The tab control paints its body behind the view controls. This plain
    // panel sits between the two: it gives the controls a stable background
    // and stops the tab from repainting over its siblings.
    g_pageBackground = CreateWindowExW(
        0, kPageClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0,
        window, nullptr, g_instance, nullptr);

    // From here on the view controls are children of the page panel.
    window = g_pageBackground;

    // Keep the tab at the bottom of the sibling z-order so it can never paint
    // its body over the panel or the view controls.
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // Install view.
    g_groupFiles = ui::GroupFrame(window, IDC_GROUP_FILES, L"Files");
    g_dllLabel = ui::Label(window, IDC_DLL_LABEL, L"DLL");
    g_dllValue = ui::Label(window, IDC_DLL_VALUE, apo::kInstalledDll,
                           SS_LEFT | SS_PATHELLIPSIS);
    g_dllStatus = ui::Label(window, IDC_DLL_STATUS, L"", SS_RIGHT);
    g_comLabel = ui::Label(window, IDC_COM_LABEL, L"COM class");
    g_comValue = ui::Label(window, IDC_COM_VALUE, apo::kClsid);
    g_comStatus = ui::Label(window, IDC_COM_STATUS, L"", SS_RIGHT);
    g_apoLabel = ui::Label(window, IDC_APO_LABEL, L"APO registration");
    g_apoValue = ui::Label(window, IDC_APO_VALUE,
                           L"AudioEngine\\AudioProcessingObjects",
                           SS_LEFT | SS_PATHELLIPSIS);
    g_apoStatus = ui::Label(window, IDC_APO_STATUS, L"", SS_RIGHT);
    g_overall = ui::Label(window, IDC_OVERALL, L"");
    ui::ApplyFont(g_overall, g_boldFont);
    g_filesNote = ui::Label(
        window, IDC_FILES_NOTE,
        L"Install and Uninstall stop audio for about a second.");
    g_install = ui::Button(window, IDC_INSTALL, L"Install");
    g_uninstall = ui::Button(window, IDC_UNINSTALL, L"Uninstall");

    // Devices view.
    g_groupDevices =
        ui::GroupFrame(window, IDC_GROUP_DEVICES, L"Devices (PreMix)");
    g_list = ui::ReportList(window, IDC_LIST,
                            LVS_SHOWSELALWAYS | LVS_SINGLESEL);
    ListView_SetExtendedListViewStyle(
        g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                    LVS_EX_INFOTIP);
    ui::ApplyListStateImages(g_list);
    AddColumn(g_list, 0, L"Device", 300);
    AddColumn(g_list, 1, L"State", 90);
    AddColumn(g_list, 2, L"APO", 130);
    AddColumn(g_list, 3, L"Notes", 170);
    g_isolate =
        ui::CheckBox(window, IDC_ISOLATE, L"Ignore other effects");
    g_hint = ui::Label(window, IDC_HINT, L"");
    g_devicesUnsaved = ui::Label(window, IDC_DEVICES_UNSAVED, L"", SS_RIGHT);
    g_devicesApply = ui::Button(window, IDC_DEVICES_APPLY, L"Apply");
    g_devicesReset = ui::Button(window, IDC_DEVICES_RESET, L"Reset");

    // Effects view.
    g_effectDeviceLabel =
        ui::Label(window, IDC_EFFECT_DEVICE_LABEL, L"Device:");
    g_effectDeviceCombo =
        ui::ComboBox(window, IDC_EFFECT_DEVICE_COMBO, CBS_DROPDOWNLIST);
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

    g_noiseReductionLabel =
        ui::Label(window, IDC_NOISE_REDUCTION_LABEL, L"Reduction (dB)");
    g_noiseReductionSlider = ui::Slider(window, IDC_NOISE_REDUCTION_SLIDER,
                                        {0.0, 70.0, 1.0, false, 0});
    g_noiseCapture =
        ui::Button(window, IDC_NOISE_CAPTURE, L"Capture noise profile");
    g_noiseNote = ui::Label(window, IDC_NOISE_NOTE, L"");

    g_listen = ui::CheckBox(window, IDC_LISTEN, L"Listen to this device");
    g_listenNote = ui::Label(
        window, IDC_LISTEN_NOTE,
        L"Plays through the default output.\r\n"
        L"Use headphones to avoid feedback.");
    g_meter = ui::Label(window, IDC_METER, L"Peak: --  RMS: --");

    g_effectsUnsaved = ui::Label(window, IDC_EFFECTS_UNSAVED, L"", SS_RIGHT);
    g_effectsApply = ui::Button(window, IDC_EFFECTS_APPLY, L"Apply");
    g_effectsReset = ui::Button(window, IDC_EFFECTS_RESET, L"Reset");

    // Child windows are inserted at the top of the sibling z-order as they are
    // created, so each group box ended up in front of the controls it frames.
    // WS_CLIPSIBLINGS then clipped those controls away. Push every frame to
    // the bottom so its contents always draw above it.
    const HWND groupBoxes[] = {g_groupFiles, g_groupDevices, g_effectListGroup,
                               g_effectGroup};
    for (HWND group : groupBoxes) {
        SetWindowPos(group, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Shared status bar (outside the tab).
    window = root;
    g_status = ui::Label(window, IDC_STATUS, L"");
    g_logButton = ui::Button(window, IDC_LOG, L"Open log");
}

void Layout(int width, int height)
{
    const int margin = Scale(16);
    int y = margin;

    MoveWindow(g_title, margin, y, Scale(320), Scale(26), TRUE);
    MoveWindow(g_refresh, width - margin - Scale(96), y, Scale(96),
               Scale(28), TRUE);
    y += Scale(40);

    const int statusHeight = Scale(38);
    const int tabsHeight = height - y - statusHeight;
    MoveWindow(g_tabs, margin, y, width - 2 * margin, tabsHeight, TRUE);

    RECT content{0, 0, width - 2 * margin, tabsHeight};
    TabCtrl_AdjustRect(g_tabs, FALSE, &content);
    const int contentX = margin + content.left;
    const int contentY = y + content.top;
    const int contentWidth = content.right - content.left;
    const int contentHeight = content.bottom - content.top;

    MoveWindow(g_pageBackground, contentX, contentY, contentWidth,
               contentHeight, TRUE);

    // The display rectangle ends at the tab frame, so a view that starts at 0,0
    // puts its group frames and labels right against the border. Inset the view
    // content to leave a margin inside the tab body.
    const int contentPadding = Scale(12);
    const int viewWidth = contentWidth - 2 * contentPadding;
    const int viewHeight = contentHeight - 2 * contentPadding;

    // The view controls are children of the page panel, so their coordinates
    // are relative to its client area.
    switch (g_view) {
    case View::Install:
        LayoutInstall(contentPadding, contentPadding, viewWidth, viewHeight);
        break;
    case View::Devices:
        LayoutDevices(contentPadding, contentPadding, viewWidth, viewHeight);
        break;
    case View::Effects:
        LayoutEffects(contentPadding, contentPadding, viewWidth, viewHeight);
        break;
    }

    const int statusY = height - statusHeight;
    MoveWindow(g_status, margin, statusY + Scale(7),
               width - 2 * margin - Scale(120), Scale(20), TRUE);
    MoveWindow(g_logButton, width - margin - Scale(104), statusY, Scale(104),
               Scale(28), TRUE);
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

// Sibling controls do not always repaint when the tab body or a dropdown
// closes over them. Redraw the window and every child after layout changes.
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
