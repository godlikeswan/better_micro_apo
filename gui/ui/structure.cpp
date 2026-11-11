#include "structure.h"

#include "apo.h"
#include "theme.h"
#include "ui_state.h"
#include "widgets.h"

#include <commctrl.h>

#include <algorithm>
#include <iterator>

namespace {

using ui::Scale;

const wchar_t* EffectLabel(EffectKind kind)
{
    switch (kind) {
    case EffectKind::Noise:
        return L"Noise reduction";
    case EffectKind::Gate:
        return L"Gate";
    case EffectKind::Dynamic:
        return L"Dynamic";
    case EffectKind::Compressor:
        return L"Compressor";
    case EffectKind::Gain:
        return L"Gain";
    case EffectKind::Mono:
        return L"Mono";
    default:
        return L"";
    }
}

void AddColumn(HWND list, int index, const wchar_t* text, int width)
{
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
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
    if (width < Scale(80)) {
        width = Scale(80);
    }
    ListView_SetColumnWidth(g_effectList, 0, width);
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
    const int editWidth = std::max(Scale(50), columnWidth - labelWidth);

    auto placePair = [&](HWND leftLabel, HWND leftEdit, HWND rightLabel,
                         HWND rightEdit) {
        MoveWindow(leftLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(leftEdit, x + labelWidth, y, editWidth, Scale(22), TRUE);
        if (rightLabel != nullptr) {
            const int rightX = x + columnWidth + columnGap;
            MoveWindow(rightLabel, rightX, y, labelWidth, Scale(20), TRUE);
            MoveWindow(rightEdit, rightX + labelWidth, y, editWidth,
                       Scale(22), TRUE);
        }
        y += Scale(30);
    };

    switch (g_effectKind) {
    case EffectKind::Noise:
        MoveWindow(g_noiseReductionLabel, x, y, labelWidth, Scale(20), TRUE);
        MoveWindow(g_noiseReductionEdit, x + labelWidth, y, editWidth,
                   Scale(22), TRUE);
        MoveWindow(g_noiseCapture, x + columnWidth + columnGap, y,
                   columnWidth, Scale(28), TRUE);
        y += Scale(34);
        MoveWindow(g_noiseNote, x, y, width, Scale(36), TRUE);
        break;
    case EffectKind::Gate:
        placePair(g_gateThresholdLabel, g_gateThresholdEdit,
                  g_gateRangeLabel, g_gateRangeEdit);
        placePair(g_gateAttackLabel, g_gateAttackEdit, g_gateHoldLabel,
                  g_gateHoldEdit);
        placePair(g_gateReleaseLabel, g_gateReleaseEdit,
                  g_gateLookaheadLabel, g_gateLookaheadEdit);
        break;
    case EffectKind::Dynamic:
        MoveWindow(g_dynamicExtend, x, y, columnWidth, Scale(22), TRUE);
        y += Scale(30);
        placePair(g_dynFrequencyLabel, g_dynFrequencyEdit, g_dynQLabel,
                  g_dynQEdit);
        placePair(g_dynThresholdLabel, g_dynThresholdEdit, g_dynRatioLabel,
                  g_dynRatioEdit);
        placePair(g_dynRangeLabel, g_dynRangeEdit, g_dynAttackLabel,
                  g_dynAttackEdit);
        placePair(g_dynReleaseLabel, g_dynReleaseEdit, nullptr, nullptr);
        break;
    case EffectKind::Compressor:
        placePair(g_compThresholdLabel, g_compThresholdEdit,
                  g_compRatioLabel, g_compRatioEdit);
        placePair(g_compAttackLabel, g_compAttackEdit, g_compReleaseLabel,
                  g_compReleaseEdit);
        break;
    case EffectKind::Gain:
        placePair(g_makeupLabel, g_makeupEdit, nullptr, nullptr);
        break;
    case EffectKind::Mono:
        MoveWindow(g_monoNote, x, y, width, Scale(40), TRUE);
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

    const int top = y + Scale(34);
    const int bottomHeight = Scale(100);
    const int panelHeight = height - Scale(34) - bottomHeight;
    const int listWidth = Scale(220);

    MoveWindow(g_effectListGroup, x, top, listWidth, panelHeight, TRUE);
    MoveWindow(g_effectList, x + Scale(12), top + Scale(24),
               listWidth - Scale(24), panelHeight - Scale(36), TRUE);

    const int paramX = x + listWidth + gap;
    const int paramWidth = width - listWidth - gap;
    MoveWindow(g_effectGroup, paramX, top, paramWidth, panelHeight, TRUE);

    const int innerX = paramX + Scale(14);
    const int innerY = top + Scale(24);
    const int innerWidth = paramWidth - Scale(28);
    MoveWindow(g_effectHeading, innerX, innerY, innerWidth, Scale(22), TRUE);
    LayoutEffectParams(innerX, innerY + Scale(28), innerWidth);

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

const wchar_t* EffectName(EffectKind kind)
{
    return EffectLabel(kind);
}

void ResizeColumns()
{
    ResizeDeviceColumns();
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

    g_effectListGroup = ui::GroupFrame(window, IDC_EFFECT_LIST_GROUP, L"Effects");
    g_effectList = ui::ReportList(
        window, IDC_EFFECT_LIST,
        LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER);
    ListView_SetExtendedListViewStyle(
        g_effectList,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ui::ApplyListStateImages(g_effectList);
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.cx = Scale(180);
        column.pszText = const_cast<wchar_t*>(L"Effect");
        ListView_InsertColumn(g_effectList, 0, &column);
    }
    for (int i = 0; i < static_cast<int>(EffectKind::Count); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText =
            const_cast<wchar_t*>(EffectName(static_cast<EffectKind>(i)));
        ListView_InsertItem(g_effectList, &item);
    }

    g_effectGroup =
        ui::GroupFrame(window, IDC_EFFECT_GROUP, L"Effect parameters");
    g_effectHeading = ui::Label(window, IDC_EFFECT_HEADING, L"Noise reduction");
    ui::ApplyFont(g_effectHeading, g_boldFont);
    g_monoNote = ui::Label(
        window, IDC_MONO_NOTE,
        L"Copies the processed left channel to every other channel.");

    g_gateThresholdLabel =
        ui::Label(window, IDC_GATE_THRESHOLD_LABEL, L"Threshold (dBFS)");
    g_gateThresholdEdit = ui::NumberField(window, IDC_GATE_THRESHOLD_EDIT);
    g_gateRangeLabel =
        ui::Label(window, IDC_GATE_RANGE_LABEL, L"Range (dB)");
    g_gateRangeEdit = ui::NumberField(window, IDC_GATE_RANGE_EDIT);
    g_gateAttackLabel =
        ui::Label(window, IDC_GATE_ATTACK_LABEL, L"Attack (ms)");
    g_gateAttackEdit = ui::NumberField(window, IDC_GATE_ATTACK_EDIT);
    g_gateHoldLabel = ui::Label(window, IDC_GATE_HOLD_LABEL, L"Hold (ms)");
    g_gateHoldEdit = ui::NumberField(window, IDC_GATE_HOLD_EDIT);
    g_gateReleaseLabel =
        ui::Label(window, IDC_GATE_RELEASE_LABEL, L"Release (ms)");
    g_gateReleaseEdit = ui::NumberField(window, IDC_GATE_RELEASE_EDIT);
    g_gateLookaheadLabel =
        ui::Label(window, IDC_GATE_LOOKAHEAD_LABEL, L"Lookahead (ms)");
    g_gateLookaheadEdit = ui::NumberField(window, IDC_GATE_LOOKAHEAD_EDIT);

    g_compThresholdLabel =
        ui::Label(window, IDC_COMP_THRESHOLD_LABEL, L"Threshold (dBFS)");
    g_compThresholdEdit = ui::NumberField(window, IDC_COMP_THRESHOLD_EDIT);
    g_compRatioLabel =
        ui::Label(window, IDC_COMP_RATIO_LABEL, L"Ratio (:1)");
    g_compRatioEdit = ui::NumberField(window, IDC_COMP_RATIO_EDIT);
    g_compAttackLabel =
        ui::Label(window, IDC_COMP_ATTACK_LABEL, L"Attack (ms)");
    g_compAttackEdit = ui::NumberField(window, IDC_COMP_ATTACK_EDIT);
    g_compReleaseLabel =
        ui::Label(window, IDC_COMP_RELEASE_LABEL, L"Release (ms)");
    g_compReleaseEdit = ui::NumberField(window, IDC_COMP_RELEASE_EDIT);

    g_makeupLabel = ui::Label(window, IDC_MAKEUP_LABEL, L"Makeup (dB)");
    g_makeupEdit = ui::NumberField(window, IDC_MAKEUP_EDIT);

    g_dynamicExtend =
        ui::CheckBox(window, IDC_DYNAMIC_EXTEND, L"Extend quiet lows");
    g_dynFrequencyLabel =
        ui::Label(window, IDC_DYN_FREQUENCY_LABEL, L"Frequency (Hz)");
    g_dynFrequencyEdit = ui::NumberField(window, IDC_DYN_FREQUENCY_EDIT);
    g_dynQLabel = ui::Label(window, IDC_DYN_Q_LABEL, L"Q");
    g_dynQEdit = ui::NumberField(window, IDC_DYN_Q_EDIT);
    g_dynThresholdLabel =
        ui::Label(window, IDC_DYN_THRESHOLD_LABEL, L"Threshold (dBFS)");
    g_dynThresholdEdit = ui::NumberField(window, IDC_DYN_THRESHOLD_EDIT);
    g_dynRatioLabel = ui::Label(window, IDC_DYN_RATIO_LABEL, L"Ratio (:1)");
    g_dynRatioEdit = ui::NumberField(window, IDC_DYN_RATIO_EDIT);
    g_dynRangeLabel = ui::Label(window, IDC_DYN_RANGE_LABEL, L"Range (dB)");
    g_dynRangeEdit = ui::NumberField(window, IDC_DYN_RANGE_EDIT);
    g_dynAttackLabel =
        ui::Label(window, IDC_DYN_ATTACK_LABEL, L"Attack (ms)");
    g_dynAttackEdit = ui::NumberField(window, IDC_DYN_ATTACK_EDIT);
    g_dynReleaseLabel =
        ui::Label(window, IDC_DYN_RELEASE_LABEL, L"Release (ms)");
    g_dynReleaseEdit = ui::NumberField(window, IDC_DYN_RELEASE_EDIT);

    g_noiseReductionLabel =
        ui::Label(window, IDC_NOISE_REDUCTION_LABEL, L"Reduction (dB)");
    g_noiseReductionEdit = ui::NumberField(window, IDC_NOISE_REDUCTION_EDIT);
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
