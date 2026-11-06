#include "apo.h"

#include "apo_internal.h"
#include "apo_monitor.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' "                 \
                        "name='Microsoft.Windows.Common-Controls' "           \
                        "version='6.0.0.0' processorArchitecture='*' "        \
                        "publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

enum : int {
    IDC_TITLE = 100,
    IDC_REFRESH,
    IDC_GROUP_FILES,
    IDC_DLL_LABEL,
    IDC_DLL_VALUE,
    IDC_DLL_STATUS,
    IDC_COM_LABEL,
    IDC_COM_VALUE,
    IDC_COM_STATUS,
    IDC_APO_LABEL,
    IDC_APO_VALUE,
    IDC_APO_STATUS,
    IDC_OVERALL,
    IDC_FILES_NOTE,
    IDC_INSTALL,
    IDC_UNINSTALL,
    IDC_GROUP_DEVICES,
    IDC_LIST,
    IDC_HINT,
    IDC_APPLY,
    IDC_STATUS,
    IDC_LOG,
    IDC_SETTINGS_HEADING,
    IDC_SETTINGS_NAME,
    IDC_GATE,
    IDC_GATE_THRESHOLD_LABEL,
    IDC_GATE_THRESHOLD_EDIT,
    IDC_GATE_RANGE_LABEL,
    IDC_GATE_RANGE_EDIT,
    IDC_GATE_ATTACK_LABEL,
    IDC_GATE_ATTACK_EDIT,
    IDC_GATE_HOLD_LABEL,
    IDC_GATE_HOLD_EDIT,
    IDC_GATE_RELEASE_LABEL,
    IDC_GATE_RELEASE_EDIT,
    IDC_COMPRESSOR,
    IDC_COMP_THRESHOLD_LABEL,
    IDC_COMP_THRESHOLD_EDIT,
    IDC_COMP_RATIO_LABEL,
    IDC_COMP_RATIO_EDIT,
    IDC_COMP_ATTACK_LABEL,
    IDC_COMP_ATTACK_EDIT,
    IDC_COMP_RELEASE_LABEL,
    IDC_COMP_RELEASE_EDIT,
    IDC_MAKEUP_LABEL,
    IDC_MAKEUP_EDIT,
    IDC_MONO,
    IDC_LISTEN,
    IDC_LISTEN_NOTE,
    IDC_METER,
};

constexpr UINT WM_APP_WORK_DONE = WM_APP + 1;
constexpr UINT_PTR kMonitorTimer = 1;

constexpr wchar_t kWindowClass[] = L"BetterMicroApoInstaller";
constexpr wchar_t kInstanceMutex[] = L"Local\\better_micro_apo_gui_instance";

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;

HWND g_title = nullptr;
HWND g_refresh = nullptr;
HWND g_groupFiles = nullptr;
HWND g_dllLabel = nullptr;
HWND g_dllValue = nullptr;
HWND g_dllStatus = nullptr;
HWND g_comLabel = nullptr;
HWND g_comValue = nullptr;
HWND g_comStatus = nullptr;
HWND g_apoLabel = nullptr;
HWND g_apoValue = nullptr;
HWND g_apoStatus = nullptr;
HWND g_overall = nullptr;
HWND g_filesNote = nullptr;
HWND g_install = nullptr;
HWND g_uninstall = nullptr;
HWND g_groupDevices = nullptr;
HWND g_list = nullptr;
HWND g_hint = nullptr;
HWND g_apply = nullptr;
HWND g_status = nullptr;
HWND g_logButton = nullptr;

HWND g_settingsHeading = nullptr;
HWND g_settingsName = nullptr;
HWND g_gate = nullptr;
HWND g_gateThresholdLabel = nullptr;
HWND g_gateThresholdEdit = nullptr;
HWND g_gateRangeLabel = nullptr;
HWND g_gateRangeEdit = nullptr;
HWND g_gateAttackLabel = nullptr;
HWND g_gateAttackEdit = nullptr;
HWND g_gateHoldLabel = nullptr;
HWND g_gateHoldEdit = nullptr;
HWND g_gateReleaseLabel = nullptr;
HWND g_gateReleaseEdit = nullptr;
HWND g_compressor = nullptr;
HWND g_compThresholdLabel = nullptr;
HWND g_compThresholdEdit = nullptr;
HWND g_compRatioLabel = nullptr;
HWND g_compRatioEdit = nullptr;
HWND g_compAttackLabel = nullptr;
HWND g_compAttackEdit = nullptr;
HWND g_compReleaseLabel = nullptr;
HWND g_compReleaseEdit = nullptr;
HWND g_makeupLabel = nullptr;
HWND g_makeupEdit = nullptr;
HWND g_mono = nullptr;
HWND g_listen = nullptr;
HWND g_listenNote = nullptr;
HWND g_meter = nullptr;

DWORD g_clipHoldUntil = 0;
unsigned g_dropTotal = 0;
std::wstring g_meterText;

HFONT g_font = nullptr;
HFONT g_boldFont = nullptr;
HFONT g_titleFont = nullptr;

int g_dpi = 96;
bool g_busy = false;
bool g_loading = false;
bool g_settingsLoading = false;
int g_settingsIndex = -1;
std::vector<apo::Endpoint> g_endpoints;
apo::FilesStatus g_files;
std::thread g_worker;
apo::Result g_result;

int Scale(int value)
{
    return MulDiv(value, g_dpi, 96);
}

void ApplyFont(HWND control, HFONT font)
{
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND CreateStatic(HWND parent, int id, const wchar_t* text, DWORD style)
{
    HWND control = CreateWindowExW(
        0, WC_STATICW, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance,
        nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND CreateButton(HWND parent, int id, const wchar_t* text)
{
    HWND control = CreateWindowExW(
        0, WC_BUTTONW, text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance,
        nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND CreateGroup(HWND parent, int id, const wchar_t* text)
{
    HWND control = CreateWindowExW(
        0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance,
        nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND CreateEdit(HWND parent, int id)
{
    HWND control = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT, 0, 0,
        0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_instance, nullptr);
    ApplyFont(control, g_font);
    return control;
}

HWND CreateCheck(HWND parent, int id, const wchar_t* text)
{
    HWND control = CreateWindowExW(
        0, WC_BUTTONW, text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance,
        nullptr);
    ApplyFont(control, g_font);
    return control;
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

void AddColumn(int index, const wchar_t* text, int width)
{
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.iSubItem = index;
    column.cx = Scale(width);
    column.pszText = const_cast<wchar_t*>(text);
    ListView_InsertColumn(g_list, index, &column);
}

std::wstring FormatNumber(double value)
{
    wchar_t text[32]{};
    swprintf_s(text, std::size(text), L"%g", value);
    return text;
}

void SetEditNumber(HWND edit, double value)
{
    SetWindowTextW(edit, FormatNumber(value).c_str());
}

bool ReadEditNumber(HWND edit, double& value)
{
    wchar_t text[64]{};
    GetWindowTextW(edit, text, static_cast<int>(std::size(text)));

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

void ClearSettingsEditor()
{
    g_settingsIndex = -1;
    g_settingsLoading = true;
    SetWindowTextW(g_settingsName, L"Select a device");
    const HWND edits[] = {
        g_gateThresholdEdit, g_gateRangeEdit, g_gateAttackEdit,
        g_gateHoldEdit, g_gateReleaseEdit, g_compThresholdEdit,
        g_compRatioEdit, g_compAttackEdit, g_compReleaseEdit, g_makeupEdit,
    };
    for (HWND edit : edits) {
        SetWindowTextW(edit, L"");
    }
    SendMessageW(g_gate, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(g_compressor, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(g_mono, BM_SETCHECK, BST_UNCHECKED, 0);
    g_settingsLoading = false;
}

void ShowSettingsForSelected()
{
    const int selected = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (selected < 0 ||
        selected >= static_cast<int>(g_endpoints.size())) {
        ClearSettingsEditor();
        return;
    }

    g_settingsIndex = selected;
    const apo::ApoSettings& settings = g_endpoints[selected].settings;

    g_settingsLoading = true;
    SetWindowTextW(g_settingsName, g_endpoints[selected].name.c_str());
    SetEditNumber(g_gateThresholdEdit, settings.gate.thresholdDb);
    SetEditNumber(g_gateRangeEdit, settings.gate.rangeDb);
    SetEditNumber(g_gateAttackEdit, settings.gate.attackMs);
    SetEditNumber(g_gateHoldEdit, settings.gate.holdMs);
    SetEditNumber(g_gateReleaseEdit, settings.gate.releaseMs);
    SetEditNumber(g_compThresholdEdit, settings.compressor.thresholdDb);
    SetEditNumber(g_compRatioEdit, settings.compressor.ratio);
    SetEditNumber(g_compAttackEdit, settings.compressor.attackMs);
    SetEditNumber(g_compReleaseEdit, settings.compressor.releaseMs);
    SetEditNumber(g_makeupEdit, settings.makeupDb);
    SendMessageW(g_gate, BM_SETCHECK,
                 settings.gate.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_compressor, BM_SETCHECK,
                 settings.compressor.enabled ? BST_CHECKED : BST_UNCHECKED,
                 0);
    SendMessageW(g_mono, BM_SETCHECK,
                 settings.mono ? BST_CHECKED : BST_UNCHECKED, 0);
    g_settingsLoading = false;
}

bool CommitSettingsEditor()
{
    if (g_settingsIndex < 0 ||
        g_settingsIndex >= static_cast<int>(g_endpoints.size())) {
        return true;
    }

    apo::ApoSettings settings = g_endpoints[g_settingsIndex].settings;
    if (!ReadEditNumber(g_gateThresholdEdit, settings.gate.thresholdDb) ||
        !ReadEditNumber(g_gateRangeEdit, settings.gate.rangeDb) ||
        !ReadEditNumber(g_gateAttackEdit, settings.gate.attackMs) ||
        !ReadEditNumber(g_gateHoldEdit, settings.gate.holdMs) ||
        !ReadEditNumber(g_gateReleaseEdit, settings.gate.releaseMs) ||
        !ReadEditNumber(g_compThresholdEdit,
                        settings.compressor.thresholdDb) ||
        !ReadEditNumber(g_compRatioEdit, settings.compressor.ratio) ||
        !ReadEditNumber(g_compAttackEdit, settings.compressor.attackMs) ||
        !ReadEditNumber(g_compReleaseEdit, settings.compressor.releaseMs) ||
        !ReadEditNumber(g_makeupEdit, settings.makeupDb)) {
        MessageBoxW(g_window, L"Enter a valid number in every setting.",
                    L"Better Micro APO", MB_ICONWARNING);
        return false;
    }

    settings.gate.enabled =
        SendMessageW(g_gate, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.compressor.enabled =
        SendMessageW(g_compressor, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.mono =
        SendMessageW(g_mono, BM_GETCHECK, 0, 0) == BST_CHECKED;
    apo::ClampSettings(settings);
    g_endpoints[g_settingsIndex].settings = settings;

    g_settingsLoading = true;
    SetEditNumber(g_gateThresholdEdit, settings.gate.thresholdDb);
    SetEditNumber(g_gateRangeEdit, settings.gate.rangeDb);
    SetEditNumber(g_gateAttackEdit, settings.gate.attackMs);
    SetEditNumber(g_gateHoldEdit, settings.gate.holdMs);
    SetEditNumber(g_gateReleaseEdit, settings.gate.releaseMs);
    SetEditNumber(g_compThresholdEdit, settings.compressor.thresholdDb);
    SetEditNumber(g_compRatioEdit, settings.compressor.ratio);
    SetEditNumber(g_compAttackEdit, settings.compressor.attackMs);
    SetEditNumber(g_compReleaseEdit, settings.compressor.releaseMs);
    SetEditNumber(g_makeupEdit, settings.makeupDb);
    g_settingsLoading = false;
    return true;
}

void EnableSettingsControls(bool enabled)
{
    const HWND controls[] = {
        g_gate,
        g_gateThresholdLabel, g_gateThresholdEdit,
        g_gateRangeLabel, g_gateRangeEdit,
        g_gateAttackLabel, g_gateAttackEdit,
        g_gateHoldLabel, g_gateHoldEdit,
        g_gateReleaseLabel, g_gateReleaseEdit,
        g_compressor,
        g_compThresholdLabel, g_compThresholdEdit,
        g_compRatioLabel, g_compRatioEdit,
        g_compAttackLabel, g_compAttackEdit,
        g_compReleaseLabel, g_compReleaseEdit,
        g_makeupLabel, g_makeupEdit,
        g_mono, g_listen,
    };
    for (HWND control : controls) {
        EnableWindow(control, enabled);
    }
}

void ResetMeter()
{
    g_clipHoldUntil = 0;
    g_dropTotal = 0;
    g_meterText = L"Peak: --";
    if (g_meter != nullptr) {
        SetWindowTextW(g_meter, g_meterText.c_str());
    }
    apo::TakeMonitorLevel();
}

void UpdateMeter()
{
    const apo::MonitorLevel level = apo::TakeMonitorLevel();
    g_dropTotal += level.drops;
    if (level.clipped) {
        g_clipHoldUntil = GetTickCount() + 2000;
    }

    std::wstring text = L"Peak: --";
    if (level.peak > 0.0) {
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"Peak: %.1f dBFS",
                   20.0 * std::log10(level.peak));
        text = buffer;
    }
    if (GetTickCount() < g_clipHoldUntil) {
        text += L"  CLIP";
    }
    if (g_dropTotal != 0) {
        text += L"  drops: " + std::to_wstring(g_dropTotal);
    }

    if (text != g_meterText) {
        g_meterText = text;
        SetWindowTextW(g_meter, g_meterText.c_str());
    }
}

void StopListening()
{
    KillTimer(g_window, kMonitorTimer);
    if (g_listen != nullptr) {
        SendMessageW(g_listen, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    apo::StopMonitor();
    ResetMeter();
}

bool StartListening()
{
    if (g_settingsIndex < 0 ||
        g_settingsIndex >= static_cast<int>(g_endpoints.size())) {
        return false;
    }

    const std::wstring error =
        apo::StartMonitor(g_endpoints[g_settingsIndex].id);
    if (!error.empty()) {
        SendMessageW(g_listen, BM_SETCHECK, BST_UNCHECKED, 0);
        ResetMeter();
        MessageBoxW(g_window, error.c_str(), L"Better Micro APO",
                    MB_ICONWARNING);
        return false;
    }

    ResetMeter();
    SetTimer(g_window, kMonitorTimer, 100, nullptr);
    return true;
}

bool HasChanges()
{
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        const bool checked =
            ListView_GetCheckState(g_list, static_cast<int>(i)) != FALSE;
        if (checked != g_endpoints[i].premixOurs) {
            return true;
        }
        if (g_endpoints[i].settingsDirty) {
            return true;
        }
    }
    return false;
}

void ResizeColumns()
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

void UpdateFilesUi()
{
    SetWindowTextW(g_dllStatus, g_files.dllPresent ? L"present" : L"missing");
    SetWindowTextW(g_comStatus,
                   g_files.comRegistered
                       ? L"registered"
                       : (g_files.registeredPath.empty() ? L"not registered"
                                                        : L"wrong path"));
    SetWindowTextW(g_apoStatus,
                   g_files.apoKeyPresent ? L"present" : L"missing");

    SetWindowTextW(g_overall, g_files.FullyInstalled()
                                  ? L"Installed"
                                  : (g_files.AnyPresent() ? L"Partial"
                                                          : L"Not installed"));

    EnableWindow(g_install, !g_busy && !g_files.FullyInstalled());
    EnableWindow(g_uninstall, !g_busy && g_files.AnyPresent());
    EnableWindow(g_refresh, !g_busy);
    EnableWindow(g_logButton, !g_busy);
}

void UpdateDevicesUi()
{
    const bool anyBound =
        std::any_of(g_endpoints.begin(), g_endpoints.end(),
                    [](const apo::Endpoint& endpoint) {
                        return endpoint.premixOurs;
                    });
    const bool sectionEnabled = g_files.FullyInstalled() || anyBound;

    EnableWindow(g_list, !g_busy && sectionEnabled);
    EnableSettingsControls(!g_busy && sectionEnabled && g_settingsIndex >= 0);
    EnableWindow(g_apply, !g_busy && sectionEnabled && HasChanges());

    if (!sectionEnabled) {
        SetWindowTextW(g_hint, L"Install the APO files first.");
    } else if (g_endpoints.empty()) {
        SetWindowTextW(g_hint, L"No capture devices found.");
    } else {
        SetWindowTextW(g_hint,
                       L"Select a device to edit its settings. Apply stops "
                       L"audio for about a second.");
    }
}

void MarkSettingsDirty()
{
    if (g_settingsLoading || g_settingsIndex < 0 ||
        g_settingsIndex >= static_cast<int>(g_endpoints.size())) {
        return;
    }
    g_endpoints[g_settingsIndex].settingsDirty = true;
    UpdateDevicesUi();
}

void Refresh()
{
    StopListening();

    std::wstring selectedId;
    if (g_settingsIndex >= 0 &&
        g_settingsIndex < static_cast<int>(g_endpoints.size())) {
        selectedId = g_endpoints[g_settingsIndex].id;
    }

    g_settingsIndex = -1;
    g_files = apo::GetFilesStatus();
    g_endpoints = apo::EnumEndpoints();

    g_loading = true;
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        const apo::Endpoint& endpoint = g_endpoints[i];

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(endpoint.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        const int row = ListView_InsertItem(g_list, &item);

        std::wstring state = apo::DeviceStateText(endpoint.state);
        ListView_SetItemText(g_list, row, 1, state.data());

        std::wstring apoText = endpoint.premixOurs ? L"Installed" : L"-";
        ListView_SetItemText(g_list, row, 2, apoText.data());

        std::wstring notes;
        if (endpoint.disableSysFx) {
            notes = L"System effects disabled";
        }
        if (endpoint.streamOurs) {
            if (!notes.empty()) {
                notes += L"; ";
            }
            notes += L"also in Stream";
        }
        ListView_SetItemText(g_list, row, 3, notes.data());

        ListView_SetCheckState(g_list, row, endpoint.premixOurs);
    }

    if (!selectedId.empty()) {
        for (size_t i = 0; i < g_endpoints.size(); ++i) {
            if (_wcsicmp(g_endpoints[i].id.c_str(), selectedId.c_str()) ==
                0) {
                ListView_SetItemState(
                    g_list, static_cast<int>(i),
                    LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(g_list, static_cast<int>(i), FALSE);
                break;
            }
        }
    }
    g_loading = false;

    ShowSettingsForSelected();
    UpdateFilesUi();
    UpdateDevicesUi();
    ResizeColumns();
}

void Layout(int width, int height)
{
    const int margin = Scale(16);
    const int gap = Scale(8);
    int y = margin;

    MoveWindow(g_title, margin, y, Scale(320), Scale(26), TRUE);
    MoveWindow(g_refresh, width - margin - Scale(96), y, Scale(96),
               Scale(28), TRUE);
    y += Scale(40);

    const int filesHeight = Scale(176);
    MoveWindow(g_groupFiles, margin, y, width - 2 * margin, filesHeight, TRUE);

    const int innerX = margin + Scale(16);
    const int labelWidth = Scale(110);
    const int statusWidth = Scale(110);
    const int statusX = width - margin - Scale(16) - statusWidth;
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
               width - margin - Scale(16) - 2 * buttonWidth - gap, buttonY,
               buttonWidth, Scale(30), TRUE);
    MoveWindow(g_uninstall, width - margin - Scale(16) - buttonWidth, buttonY,
               buttonWidth, Scale(30), TRUE);

    y += filesHeight + Scale(12);
    int devicesHeight = height - y - Scale(52);
    if (devicesHeight < Scale(440)) {
        devicesHeight = Scale(440);
    }
    MoveWindow(g_groupDevices, margin, y, width - 2 * margin, devicesHeight,
               TRUE);

    const int listX = margin + Scale(16);
    const int settingsWidth = Scale(360);
    const int listWidth =
        width - 2 * margin - Scale(32) - settingsWidth - gap;
    const int listHeight = devicesHeight - Scale(76);
    MoveWindow(g_list, listX, y + Scale(24), listWidth, listHeight, TRUE);

    const int settingsX = listX + listWidth + gap;
    int settingsY = y + Scale(24);
    MoveWindow(g_settingsHeading, settingsX, settingsY, settingsWidth,
               Scale(20), TRUE);
    settingsY += Scale(24);
    MoveWindow(g_settingsName, settingsX, settingsY, settingsWidth,
               Scale(20), TRUE);
    settingsY += Scale(26);

    const int columnGap = Scale(12);
    const int columnWidth = (settingsWidth - columnGap) / 2;
    const int settingLabelWidth = Scale(100);
    const int settingEditWidth = columnWidth - settingLabelWidth;

    auto placeSettingPair = [&](HWND leftLabel, HWND leftEdit,
                                HWND rightLabel, HWND rightEdit) {
        MoveWindow(leftLabel, settingsX, settingsY, settingLabelWidth,
                   Scale(20), TRUE);
        MoveWindow(leftEdit, settingsX + settingLabelWidth, settingsY,
                   settingEditWidth, Scale(22), TRUE);
        if (rightLabel != nullptr) {
            const int rightX = settingsX + columnWidth + columnGap;
            MoveWindow(rightLabel, rightX, settingsY, settingLabelWidth,
                       Scale(20), TRUE);
            MoveWindow(rightEdit, rightX + settingLabelWidth, settingsY,
                       settingEditWidth, Scale(22), TRUE);
        }
        settingsY += Scale(28);
    };

    MoveWindow(g_gate, settingsX, settingsY, settingsWidth, Scale(20), TRUE);
    settingsY += Scale(28);
    placeSettingPair(g_gateThresholdLabel, g_gateThresholdEdit,
                     g_gateRangeLabel, g_gateRangeEdit);
    placeSettingPair(g_gateAttackLabel, g_gateAttackEdit, g_gateHoldLabel,
                     g_gateHoldEdit);
    placeSettingPair(g_gateReleaseLabel, g_gateReleaseEdit, nullptr, nullptr);

    MoveWindow(g_compressor, settingsX, settingsY, settingsWidth, Scale(20),
               TRUE);
    settingsY += Scale(28);
    placeSettingPair(g_compThresholdLabel, g_compThresholdEdit,
                     g_compRatioLabel, g_compRatioEdit);
    placeSettingPair(g_compAttackLabel, g_compAttackEdit, g_compReleaseLabel,
                     g_compReleaseEdit);
    placeSettingPair(g_makeupLabel, g_makeupEdit, nullptr, nullptr);

    MoveWindow(g_mono, settingsX, settingsY, settingsWidth, Scale(20), TRUE);
    settingsY += Scale(24);
    MoveWindow(g_listen, settingsX, settingsY, settingsWidth, Scale(20), TRUE);
    settingsY += Scale(24);
    MoveWindow(g_listenNote, settingsX, settingsY, settingsWidth, Scale(36),
               TRUE);
    settingsY += Scale(40);
    MoveWindow(g_meter, settingsX, settingsY, settingsWidth, Scale(20), TRUE);

    MoveWindow(g_hint, listX, y + devicesHeight - Scale(42),
               listWidth - Scale(130), Scale(20), TRUE);
    MoveWindow(g_apply, width - margin - Scale(16) - Scale(120),
               y + devicesHeight - Scale(46), Scale(120), Scale(30), TRUE);

    const int statusY = height - Scale(38);
    MoveWindow(g_status, margin, statusY + Scale(7),
               width - 2 * margin - Scale(120), Scale(20), TRUE);
    MoveWindow(g_logButton, width - margin - Scale(104), statusY, Scale(104),
               Scale(28), TRUE);

    ResizeColumns();
}

void StartWork(std::function<apo::Result()> work, const wchar_t* status)
{
    if (g_busy) {
        return;
    }

    g_busy = true;
    SetWindowTextW(g_status, status);
    UpdateFilesUi();
    UpdateDevicesUi();

    g_worker = std::thread([work = std::move(work)] {
        g_result = work();
        PostMessageW(g_window, WM_APP_WORK_DONE, 0, 0);
    });
}

void DoInstall()
{
    if (g_busy) {
        return;
    }

    StopListening();
    StartWork([] { return apo::InstallFiles(); }, L"Installing...");
}

void DoUninstall()
{
    if (g_busy) {
        return;
    }

    StopListening();
    StartWork([] { return apo::UninstallFiles(); }, L"Uninstalling...");
}

void DoApply()
{
    if (g_busy) {
        return;
    }
    if (!CommitSettingsEditor()) {
        return;
    }

    std::vector<apo::Change> changes;
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        const bool checked =
            ListView_GetCheckState(g_list, static_cast<int>(i)) != FALSE;
        const bool bindingChanged = checked != g_endpoints[i].premixOurs;
        if (!bindingChanged && !g_endpoints[i].settingsDirty) {
            continue;
        }

        apo::Change change;
        change.id = g_endpoints[i].id;
        change.enable = checked;
        change.settingsChanged = g_endpoints[i].settingsDirty;
        change.settings = g_endpoints[i].settings;
        changes.push_back(std::move(change));
    }
    if (changes.empty()) {
        return;
    }

    StopListening();
    StartWork([changes] { return apo::ApplyChanges(changes); },
              L"Applying...");
}

void DoOpenLog()
{
    if (apo::FileExists(apo::kLogFile)) {
        ShellExecuteW(g_window, L"open", apo::kLogFile, nullptr, nullptr,
                      SW_SHOWNORMAL);
    } else {
        ShellExecuteW(g_window, L"open", apo::kDataDir, nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        CreateFonts();

        g_title = CreateStatic(window, IDC_TITLE, L"Better Micro APO",
                               SS_LEFT);
        ApplyFont(g_title, g_titleFont);
        g_refresh = CreateButton(window, IDC_REFRESH, L"Refresh");

        g_groupFiles = CreateGroup(window, IDC_GROUP_FILES, L"Files");
        g_dllLabel = CreateStatic(window, IDC_DLL_LABEL, L"DLL", SS_LEFT);
        g_dllValue = CreateStatic(window, IDC_DLL_VALUE, apo::kInstalledDll,
                                  SS_LEFT | SS_PATHELLIPSIS);
        g_dllStatus = CreateStatic(window, IDC_DLL_STATUS, L"", SS_RIGHT);
        g_comLabel = CreateStatic(window, IDC_COM_LABEL, L"COM class", SS_LEFT);
        g_comValue = CreateStatic(window, IDC_COM_VALUE, apo::kClsid, SS_LEFT);
        g_comStatus = CreateStatic(window, IDC_COM_STATUS, L"", SS_RIGHT);
        g_apoLabel =
            CreateStatic(window, IDC_APO_LABEL, L"APO registration", SS_LEFT);
        g_apoValue = CreateStatic(window, IDC_APO_VALUE,
                                  L"AudioEngine\\AudioProcessingObjects",
                                  SS_LEFT | SS_PATHELLIPSIS);
        g_apoStatus = CreateStatic(window, IDC_APO_STATUS, L"", SS_RIGHT);
        g_overall = CreateStatic(window, IDC_OVERALL, L"", SS_LEFT);
        ApplyFont(g_overall, g_boldFont);
        g_filesNote = CreateStatic(
            window, IDC_FILES_NOTE,
            L"Install and Uninstall stop audio for about a second.", SS_LEFT);
        g_install = CreateButton(window, IDC_INSTALL, L"Install");
        g_uninstall = CreateButton(window, IDC_UNINSTALL, L"Uninstall");

        g_groupDevices =
            CreateGroup(window, IDC_GROUP_DEVICES, L"Devices (PreMix)");
        g_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
            g_instance, nullptr);
        ListView_SetExtendedListViewStyle(
            g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                        LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        ApplyFont(g_list, g_font);
        AddColumn(0, L"Device", 300);
        AddColumn(1, L"State", 90);
        AddColumn(2, L"APO", 130);
        AddColumn(3, L"Notes", 170);
        g_hint = CreateStatic(window, IDC_HINT, L"", SS_LEFT);
        g_apply = CreateButton(window, IDC_APPLY, L"Apply");

        g_settingsHeading = CreateStatic(window, IDC_SETTINGS_HEADING,
                                         L"Device settings", SS_LEFT);
        ApplyFont(g_settingsHeading, g_boldFont);
        g_settingsName = CreateStatic(window, IDC_SETTINGS_NAME,
                                      L"Select a device",
                                      SS_LEFT | SS_PATHELLIPSIS);
        g_gate = CreateCheck(window, IDC_GATE, L"Gate");
        g_gateThresholdLabel = CreateStatic(
            window, IDC_GATE_THRESHOLD_LABEL, L"Threshold (dBFS)", SS_LEFT);
        g_gateThresholdEdit = CreateEdit(window, IDC_GATE_THRESHOLD_EDIT);
        g_gateRangeLabel =
            CreateStatic(window, IDC_GATE_RANGE_LABEL, L"Range (dB)", SS_LEFT);
        g_gateRangeEdit = CreateEdit(window, IDC_GATE_RANGE_EDIT);
        g_gateAttackLabel =
            CreateStatic(window, IDC_GATE_ATTACK_LABEL, L"Attack (ms)", SS_LEFT);
        g_gateAttackEdit = CreateEdit(window, IDC_GATE_ATTACK_EDIT);
        g_gateHoldLabel =
            CreateStatic(window, IDC_GATE_HOLD_LABEL, L"Hold (ms)", SS_LEFT);
        g_gateHoldEdit = CreateEdit(window, IDC_GATE_HOLD_EDIT);
        g_gateReleaseLabel = CreateStatic(window, IDC_GATE_RELEASE_LABEL,
                                          L"Release (ms)", SS_LEFT);
        g_gateReleaseEdit = CreateEdit(window, IDC_GATE_RELEASE_EDIT);
        g_compressor = CreateCheck(window, IDC_COMPRESSOR, L"Compressor");
        g_compThresholdLabel = CreateStatic(
            window, IDC_COMP_THRESHOLD_LABEL, L"Threshold (dBFS)", SS_LEFT);
        g_compThresholdEdit = CreateEdit(window, IDC_COMP_THRESHOLD_EDIT);
        g_compRatioLabel =
            CreateStatic(window, IDC_COMP_RATIO_LABEL, L"Ratio (:1)", SS_LEFT);
        g_compRatioEdit = CreateEdit(window, IDC_COMP_RATIO_EDIT);
        g_compAttackLabel = CreateStatic(window, IDC_COMP_ATTACK_LABEL,
                                         L"Attack (ms)", SS_LEFT);
        g_compAttackEdit = CreateEdit(window, IDC_COMP_ATTACK_EDIT);
        g_compReleaseLabel = CreateStatic(window, IDC_COMP_RELEASE_LABEL,
                                          L"Release (ms)", SS_LEFT);
        g_compReleaseEdit = CreateEdit(window, IDC_COMP_RELEASE_EDIT);
        g_makeupLabel =
            CreateStatic(window, IDC_MAKEUP_LABEL, L"Makeup (dB)", SS_LEFT);
        g_makeupEdit = CreateEdit(window, IDC_MAKEUP_EDIT);
        g_mono = CreateCheck(window, IDC_MONO, L"Mono (left to all channels)");
        g_listen = CreateCheck(window, IDC_LISTEN, L"Listen to this device");
        g_listenNote = CreateStatic(
            window, IDC_LISTEN_NOTE,
            L"Plays through the default output.\r\n"
            L"Use headphones to avoid feedback.",
            SS_LEFT);
        g_meter = CreateStatic(window, IDC_METER, L"Peak: --", SS_LEFT);

        g_status = CreateStatic(window, IDC_STATUS, L"", SS_LEFT);
        g_logButton = CreateButton(window, IDC_LOG, L"Open log");

        Refresh();
        return 0;
    }

    case WM_SIZE:
        if (g_list) {
            Layout(LOWORD(lParam), HIWORD(lParam));
        }
        break;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(900);
        info->ptMinTrackSize.y = Scale(760);
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_REFRESH:
            Refresh();
            break;
        case IDC_INSTALL:
            DoInstall();
            break;
        case IDC_UNINSTALL:
            DoUninstall();
            break;
        case IDC_APPLY:
            DoApply();
            break;
        case IDC_LOG:
            DoOpenLog();
            break;
        case IDC_GATE_THRESHOLD_EDIT:
        case IDC_GATE_RANGE_EDIT:
        case IDC_GATE_ATTACK_EDIT:
        case IDC_GATE_HOLD_EDIT:
        case IDC_GATE_RELEASE_EDIT:
        case IDC_COMP_THRESHOLD_EDIT:
        case IDC_COMP_RATIO_EDIT:
        case IDC_COMP_ATTACK_EDIT:
        case IDC_COMP_RELEASE_EDIT:
        case IDC_MAKEUP_EDIT:
            if (HIWORD(wParam) == EN_CHANGE) {
                MarkSettingsDirty();
            }
            break;
        case IDC_GATE:
        case IDC_COMPRESSOR:
        case IDC_MONO:
            if (HIWORD(wParam) == BN_CLICKED) {
                MarkSettingsDirty();
            }
            break;
        case IDC_LISTEN:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (SendMessageW(g_listen, BM_GETCHECK, 0, 0) ==
                    BST_CHECKED) {
                    StartListening();
                } else {
                    StopListening();
                }
            }
            break;
        }
        break;

    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom != IDC_LIST) {
            break;
        }

        switch (header->code) {
        case LVN_ITEMCHANGING: {
            auto* item = reinterpret_cast<NMLISTVIEW*>(lParam);
            if ((item->uChanged & LVIF_STATE) != 0) {
                const bool wasChecked =
                    ((item->uOldState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                const bool nowChecked =
                    ((item->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                if (nowChecked && !wasChecked) {
                    const size_t index =
                        static_cast<size_t>(item->lParam);
                    if (index < g_endpoints.size() &&
                        !apo::IsDeviceActive(g_endpoints[index].state)) {
                        MessageBeep(MB_ICONWARNING);
                        return TRUE;
                    }
                }
            }
            break;
        }

        case LVN_ITEMCHANGED: {
            if (g_loading) {
                break;
            }

            auto* item = reinterpret_cast<NMLISTVIEW*>(lParam);
            const bool selectionChanged =
                (item->uChanged & LVIF_STATE) != 0 &&
                ((item->uOldState ^ item->uNewState) & LVIS_SELECTED) != 0;
            if (selectionChanged && (item->uNewState & LVIS_SELECTED) != 0) {
                if (!CommitSettingsEditor()) {
                    // Keep editing the previous device.
                    g_loading = true;
                    if (g_settingsIndex >= 0 &&
                        g_settingsIndex <
                            static_cast<int>(g_endpoints.size())) {
                        ListView_SetItemState(
                            g_list, g_settingsIndex,
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
                    }
                    ListView_SetItemState(g_list, item->iItem, 0,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                    g_loading = false;
                    UpdateDevicesUi();
                    break;
                }
                StopListening();
                ShowSettingsForSelected();
            }
            UpdateDevicesUi();
            break;
        }

        case LVN_GETINFOTIP: {
            auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
            const size_t index = static_cast<size_t>(tip->iItem);
            if (index < g_endpoints.size()) {
                std::wstring text = g_endpoints[index].id;
                if (g_endpoints[index].streamOurs) {
                    text += L"\r\nAlso bound in the Stream slot";
                }
                wcsncpy_s(tip->pszText, tip->cchTextMax, text.c_str(),
                          _TRUNCATE);
            }
            break;
        }

        case NM_CUSTOMDRAW: {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            switch (draw->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                const size_t index =
                    static_cast<size_t>(draw->nmcd.lItemlParam);
                if (index < g_endpoints.size() &&
                    !apo::IsDeviceActive(g_endpoints[index].state)) {
                    draw->clrText = GetSysColor(COLOR_GRAYTEXT);
                }
                return CDRF_DODEFAULT;
            }
            }
            break;
        }
        }
        break;
    }

    case WM_APP_WORK_DONE:
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_busy = false;
        Refresh();
        SetWindowTextW(g_status, g_result.summary.c_str());
        if (!g_result.ok) {
            std::wstring details = g_result.details;
            if (details.size() > 4000) {
                details.resize(4000);
                details += L"\r\n...";
            }
            MessageBoxW(window, details.c_str(), L"Operation failed",
                        MB_ICONERROR);
        }
        break;

    case WM_TIMER:
        if (wParam == kMonitorTimer) {
            if (!apo::IsMonitorActive()) {
                KillTimer(window, kMonitorTimer);
                SendMessageW(g_listen, BM_SETCHECK, BST_UNCHECKED, 0);
                SetWindowTextW(g_status, L"Monitoring stopped.");
                ResetMeter();
            } else {
                UpdateMeter();
            }
        }
        break;

    case WM_CLOSE:
        if (g_busy) {
            MessageBeep(MB_ICONWARNING);
            break;
        }
        DestroyWindow(window);
        break;

    case WM_DESTROY:
        KillTimer(window, kMonitorTimer);
        apo::StopMonitor();
        if (g_worker.joinable()) {
            g_worker.join();
        }
        if (g_font) {
            DeleteObject(g_font);
        }
        if (g_boldFont) {
            DeleteObject(g_boldFont);
        }
        if (g_titleFont) {
            DeleteObject(g_titleFont);
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return 0;
}

int DumpStatus(const std::wstring& path)
{
    const apo::FilesStatus files = apo::GetFilesStatus();
    const std::vector<apo::Endpoint> endpoints = apo::EnumEndpoints();

    std::wstring text;
    text += L"elevated = ";
    text += apo::IsElevated() ? L"yes\r\n" : L"no\r\n";
    text += L"dll_present = ";
    text += files.dllPresent ? L"yes\r\n" : L"no\r\n";
    text += L"com_registered = ";
    text += files.comRegistered ? L"yes\r\n" : L"no\r\n";
    text += L"com_path = " + files.registeredPath + L"\r\n";
    text += L"apo_key = ";
    text += files.apoKeyPresent ? L"yes\r\n" : L"no\r\n";
    text += L"overall = ";
    text += files.FullyInstalled()
                ? L"installed\r\n"
                : (files.AnyPresent() ? L"partial\r\n" : L"none\r\n");
    text += L"endpoints = " + std::to_wstring(endpoints.size()) + L"\r\n";

    for (const apo::Endpoint& endpoint : endpoints) {
        text += L"endpoint " + endpoint.id + L" | name=\"" + endpoint.name +
                L"\" | state=" + std::to_wstring(endpoint.state) +
                L" | active=" +
                (apo::IsDeviceActive(endpoint.state) ? L"yes" : L"no") +
                L" | premix=" +
                (endpoint.premixOurs ? L"installed" : L"no") +
                L" | stream=" +
                (endpoint.streamOurs ? L"installed" : L"no") +
                L" | sysfx=" +
                (endpoint.disableSysFx ? L"disabled" : L"enabled") +
                L" | gate=" +
                (endpoint.settings.gate.enabled ? L"on" : L"off") +
                L" | gate_threshold=" +
                FormatNumber(endpoint.settings.gate.thresholdDb) +
                L" | gate_range=" +
                FormatNumber(endpoint.settings.gate.rangeDb) +
                L" | gate_attack=" +
                FormatNumber(endpoint.settings.gate.attackMs) +
                L" | gate_hold=" +
                FormatNumber(endpoint.settings.gate.holdMs) +
                L" | gate_release=" +
                FormatNumber(endpoint.settings.gate.releaseMs) +
                L" | compressor=" +
                (endpoint.settings.compressor.enabled ? L"on" : L"off") +
                L" | threshold=" +
                FormatNumber(endpoint.settings.compressor.thresholdDb) +
                L" | ratio=" +
                FormatNumber(endpoint.settings.compressor.ratio) +
                L" | attack=" +
                FormatNumber(endpoint.settings.compressor.attackMs) +
                L" | release=" +
                FormatNumber(endpoint.settings.compressor.releaseMs) +
                L" | makeup=" + FormatNumber(endpoint.settings.makeupDb) +
                L" | mono=" + (endpoint.settings.mono ? L"on" : L"off") +
                L"\r\n";
    }

    return apo::detail::WriteTextFile(path, text) ? 0 : 1;
}

} // namespace

bool ActivateExistingInstance()
{
    for (int attempt = 0; attempt < 40; ++attempt) {
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing) {
            if (IsIconic(existing)) {
                ShowWindow(existing, SW_RESTORE);
            } else {
                ShowWindow(existing, SW_SHOW);
            }
            if (!SetForegroundWindow(existing)) {
                FLASHWINFO flash{sizeof(flash), existing, FLASHW_ALL, 3, 0};
                FlashWindowEx(&flash);
            }
            return true;
        }
        Sleep(50);
    }
    return false;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    g_instance = instance;

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (g_dpi <= 0) {
        g_dpi = 96;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--dump") == 0) {
                wchar_t temp[MAX_PATH]{};
                GetTempPathW(MAX_PATH, temp);
                const std::wstring path =
                    std::wstring(temp) +
                    L"better_micro_apo_gui_status.txt";
                const int code = DumpStatus(path);
                LocalFree(argv);
                return code;
            }
            if (_wcsnicmp(argv[i], L"--dump=", 7) == 0) {
                const int code = DumpStatus(argv[i] + 7);
                LocalFree(argv);
                return code;
            }
        }
        LocalFree(argv);
    }

    // Keep the handle open for the life of the process so a second launch
    // reliably sees the mutex. --dump above deliberately bypasses this.
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);
    if (instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        return 0;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    HWND window = CreateWindowExW(
        0, windowClass.lpszClassName, L"Better Micro APO",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, Scale(1020),
        Scale(820), nullptr, nullptr, instance, nullptr);
    if (!window) {
        return 1;
    }

    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
