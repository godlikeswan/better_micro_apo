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
#include <deque>
#include <functional>
#include <initializer_list>
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
    IDC_TABS,

    // Install view.
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

    // Devices view.
    IDC_GROUP_DEVICES,
    IDC_LIST,
    IDC_ISOLATE,
    IDC_HINT,
    IDC_DEVICES_UNSAVED,
    IDC_DEVICES_APPLY,
    IDC_DEVICES_RESET,

    // Effects view.
    IDC_EFFECT_DEVICE_LABEL,
    IDC_EFFECT_DEVICE_COMBO,
    IDC_EFFECT_LIST_GROUP,
    IDC_EFFECT_LIST,
    IDC_EFFECT_GROUP,
    IDC_EFFECT_HEADING,
    IDC_EFFECTS_UNSAVED,
    IDC_EFFECTS_APPLY,
    IDC_EFFECTS_RESET,
    IDC_MONO_NOTE,

    // Effect parameters.
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
    IDC_GATE_LOOKAHEAD_LABEL,
    IDC_GATE_LOOKAHEAD_EDIT,
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
    IDC_DYNAMIC_EXTEND,
    IDC_DYN_FREQUENCY_LABEL,
    IDC_DYN_FREQUENCY_EDIT,
    IDC_DYN_Q_LABEL,
    IDC_DYN_Q_EDIT,
    IDC_DYN_THRESHOLD_LABEL,
    IDC_DYN_THRESHOLD_EDIT,
    IDC_DYN_RATIO_LABEL,
    IDC_DYN_RATIO_EDIT,
    IDC_DYN_RANGE_LABEL,
    IDC_DYN_RANGE_EDIT,
    IDC_DYN_ATTACK_LABEL,
    IDC_DYN_ATTACK_EDIT,
    IDC_DYN_RELEASE_LABEL,
    IDC_DYN_RELEASE_EDIT,
    IDC_NOISE_REDUCTION_LABEL,
    IDC_NOISE_REDUCTION_EDIT,
    IDC_NOISE_CAPTURE,
    IDC_NOISE_NOTE,
    IDC_LISTEN,
    IDC_LISTEN_NOTE,
    IDC_METER,

    // Shared status bar.
    IDC_STATUS,
    IDC_LOG,
};

constexpr UINT WM_APP_WORK_DONE = WM_APP + 1;
constexpr UINT_PTR kMonitorTimer = 1;
constexpr DWORD kMeterWindowMs = 500;

constexpr wchar_t kWindowClass[] = L"BetterMicroApoInstaller";
constexpr wchar_t kInstanceMutex[] = L"Local\\better_micro_apo_gui_instance";

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;

// Always-visible chrome.
HWND g_title = nullptr;
HWND g_refresh = nullptr;
HWND g_tabs = nullptr;
HWND g_status = nullptr;
HWND g_logButton = nullptr;

// Install view.
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

// Devices view.
HWND g_groupDevices = nullptr;
HWND g_list = nullptr;
HWND g_isolate = nullptr;
HWND g_hint = nullptr;
HWND g_devicesUnsaved = nullptr;
HWND g_devicesApply = nullptr;
HWND g_devicesReset = nullptr;

// Effects view.
HWND g_effectDeviceLabel = nullptr;
HWND g_effectDeviceCombo = nullptr;
HWND g_effectListGroup = nullptr;
HWND g_effectList = nullptr;
HWND g_effectGroup = nullptr;
HWND g_effectHeading = nullptr;
HWND g_effectsUnsaved = nullptr;
HWND g_effectsApply = nullptr;
HWND g_effectsReset = nullptr;
HWND g_monoNote = nullptr;

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
HWND g_gateLookaheadLabel = nullptr;
HWND g_gateLookaheadEdit = nullptr;
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
HWND g_dynamicExtend = nullptr;
HWND g_dynFrequencyLabel = nullptr;
HWND g_dynFrequencyEdit = nullptr;
HWND g_dynQLabel = nullptr;
HWND g_dynQEdit = nullptr;
HWND g_dynThresholdLabel = nullptr;
HWND g_dynThresholdEdit = nullptr;
HWND g_dynRatioLabel = nullptr;
HWND g_dynRatioEdit = nullptr;
HWND g_dynRangeLabel = nullptr;
HWND g_dynRangeEdit = nullptr;
HWND g_dynAttackLabel = nullptr;
HWND g_dynAttackEdit = nullptr;
HWND g_dynReleaseLabel = nullptr;
HWND g_dynReleaseEdit = nullptr;
HWND g_noiseReductionLabel = nullptr;
HWND g_noiseReductionEdit = nullptr;
HWND g_noiseCapture = nullptr;
HWND g_noiseNote = nullptr;
HWND g_listen = nullptr;
HWND g_listenNote = nullptr;
HWND g_meter = nullptr;

DWORD g_clipHoldUntil = 0;
unsigned g_dropTotal = 0;

struct MeterEntry {
    DWORD tick = 0;
    double peak = 0.0;
    double sumSquares = 0.0;
    unsigned long long samples = 0;
};

std::deque<MeterEntry> g_meterHistory;
std::wstring g_meterText;

HFONT g_font = nullptr;
HFONT g_boldFont = nullptr;
HFONT g_titleFont = nullptr;

int g_dpi = 96;
bool g_busy = false;
bool g_loading = false;         // Guard for the device list.
bool g_effectLoading = false;   // Guard for the effect list/editor.
bool g_comboLoading = false;    // Guard for the device combo.
bool g_tabLoading = false;      // Guard for programmatic tab changes.

std::vector<apo::Endpoint> g_endpoints;
apo::FilesStatus g_files;
std::thread g_worker;
apo::Result g_result;

int g_deviceIndex = -1;   // Selected row in the Devices list.
int g_effectIndex = -1;   // Selected endpoint in the Effects combo.

constexpr unsigned kNoiseCaptureMs = 3000;

// The effect list rows, in display order. Each row maps to one optional stage.
enum class EffectKind : int {
    Noise = 0,
    Gate,
    Dynamic,
    Compressor,
    Gain,
    Mono,
    Count,
};

EffectKind g_effectKind = EffectKind::Noise;

enum class View : int {
    Install = 0,
    Devices,
    Effects,
};

View g_view = View::Install;

enum class WorkKind {
    Generic,
    CaptureNoise,
};

WorkKind g_workKind = WorkKind::Generic;
int g_captureIndex = -1;
apo::ApoSettings g_captureSettings;
bool g_captureSucceeded = false;

// A noise capture restarts audio and saves the effect settings. Keep any
// pending isolation edit separate so the capture does not mark it as applied.
bool g_captureIsolationDirty = false;
bool g_captureIsolatePending = false;
bool g_captureIsolateSaved = false;

// Apply briefly stops the audio service, which also stops the monitor stream.
// Remember the listening state so it can be restarted once Apply finishes.
bool g_resumeListen = false;
std::wstring g_resumeListenId;

// Devices Apply refreshes the endpoint list. Keep effect edits that are still
// unsaved so switching views and applying device bindings does not discard
// them.
bool g_preserveDirtyEffects = false;

// Forward declarations for helpers that are used before their definitions.
void UpdateViewAvailability();
void SetActiveView(View view);
void UpdateDevicesUi();
void UpdateEffectsUi();
void Relayout();
void ResizeColumns();

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

// ---------------------------------------------------------------------------
// Effect list model
// ---------------------------------------------------------------------------

const wchar_t* EffectName(EffectKind kind)
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

bool EffectEnabled(const apo::ApoSettings& settings, EffectKind kind)
{
    switch (kind) {
    case EffectKind::Noise:
        return settings.noise.enabled;
    case EffectKind::Gate:
        return settings.gate.enabled;
    case EffectKind::Dynamic:
        return settings.dynamics.enabled;
    case EffectKind::Compressor:
        return settings.compressor.enabled;
    case EffectKind::Gain:
        return settings.makeupEnabled;
    case EffectKind::Mono:
        return settings.mono;
    default:
        return false;
    }
}

void SetEffectEnabled(apo::ApoSettings& settings, EffectKind kind, bool enabled)
{
    switch (kind) {
    case EffectKind::Noise:
        settings.noise.enabled = enabled;
        break;
    case EffectKind::Gate:
        settings.gate.enabled = enabled;
        break;
    case EffectKind::Dynamic:
        settings.dynamics.enabled = enabled;
        break;
    case EffectKind::Compressor:
        settings.compressor.enabled = enabled;
        break;
    case EffectKind::Gain:
        settings.makeupEnabled = enabled;
        break;
    case EffectKind::Mono:
        settings.mono = enabled;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// View and dirty-state helpers
// ---------------------------------------------------------------------------

bool AnyBound()
{
    return std::any_of(g_endpoints.begin(), g_endpoints.end(),
                       [](const apo::Endpoint& endpoint) {
                           return endpoint.Enabled();
                       });
}

bool DevicesAvailable()
{
    return g_files.FullyInstalled() || AnyBound();
}

bool EffectsAvailable()
{
    return g_files.FullyInstalled() && AnyBound();
}

bool EffectsDirty()
{
    return std::any_of(g_endpoints.begin(), g_endpoints.end(),
                       [](const apo::Endpoint& endpoint) {
                           return endpoint.settingsDirty;
                       });
}

bool DevicesDirty()
{
    if (g_list == nullptr) {
        return false;
    }
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        const bool checked =
            ListView_GetCheckState(g_list, static_cast<int>(i)) != FALSE;
        if (checked != g_endpoints[i].Enabled()) {
            return true;
        }
        if (g_endpoints[i].isolationDirty ||
            g_endpoints[i].isolatePendingCleanup) {
            return true;
        }
    }
    return false;
}

bool ValidEffectIndex()
{
    return g_effectIndex >= 0 &&
           g_effectIndex < static_cast<int>(g_endpoints.size()) &&
           g_endpoints[g_effectIndex].Enabled();
}

View DefaultView()
{
    if (EffectsAvailable()) {
        return View::Effects;
    }
    if (DevicesAvailable()) {
        return View::Devices;
    }
    return View::Install;
}

int ViewToTab(View view)
{
    return static_cast<int>(view);
}

View TabToView(int tab)
{
    switch (tab) {
    case 1:
        return View::Devices;
    case 2:
        return View::Effects;
    default:
        return View::Install;
    }
}

bool TabAvailable(int tab)
{
    switch (tab) {
    case 1:
        return DevicesAvailable();
    case 2:
        return EffectsAvailable();
    default:
        return true;
    }
}

// ---------------------------------------------------------------------------
// Effect editor
// ---------------------------------------------------------------------------

void ShowEffectControls(EffectKind kind, bool show)
{
    auto showAll = [show](std::initializer_list<HWND> controls) {
        for (HWND control : controls) {
            if (control != nullptr) {
                ShowWindow(control, show ? SW_SHOW : SW_HIDE);
            }
        }
    };

    switch (kind) {
    case EffectKind::Noise:
        showAll({g_noiseReductionLabel, g_noiseReductionEdit, g_noiseCapture,
                 g_noiseNote});
        break;
    case EffectKind::Gate:
        showAll({g_gateThresholdLabel, g_gateThresholdEdit, g_gateRangeLabel,
                 g_gateRangeEdit, g_gateAttackLabel, g_gateAttackEdit,
                 g_gateHoldLabel, g_gateHoldEdit, g_gateReleaseLabel,
                 g_gateReleaseEdit, g_gateLookaheadLabel,
                 g_gateLookaheadEdit});
        break;
    case EffectKind::Dynamic:
        showAll({g_dynamicExtend, g_dynFrequencyLabel, g_dynFrequencyEdit,
                 g_dynQLabel, g_dynQEdit, g_dynThresholdLabel,
                 g_dynThresholdEdit, g_dynRatioLabel, g_dynRatioEdit,
                 g_dynRangeLabel, g_dynRangeEdit, g_dynAttackLabel,
                 g_dynAttackEdit, g_dynReleaseLabel, g_dynReleaseEdit});
        break;
    case EffectKind::Compressor:
        showAll({g_compThresholdLabel, g_compThresholdEdit, g_compRatioLabel,
                 g_compRatioEdit, g_compAttackLabel, g_compAttackEdit,
                 g_compReleaseLabel, g_compReleaseEdit});
        break;
    case EffectKind::Gain:
        showAll({g_makeupLabel, g_makeupEdit});
        break;
    case EffectKind::Mono:
        showAll({g_monoNote});
        break;
    default:
        break;
    }
}

void UpdateViewVisibility()
{
    auto showAll = [](std::initializer_list<HWND> controls, bool show) {
        for (HWND control : controls) {
            if (control != nullptr) {
                ShowWindow(control, show ? SW_SHOW : SW_HIDE);
            }
        }
    };

    const bool install = g_view == View::Install;
    const bool devices = g_view == View::Devices;
    const bool effects = g_view == View::Effects;

    showAll({g_groupFiles, g_dllLabel, g_dllValue, g_dllStatus, g_comLabel,
             g_comValue, g_comStatus, g_apoLabel, g_apoValue, g_apoStatus,
             g_overall, g_filesNote, g_install, g_uninstall},
            install);

    showAll({g_groupDevices, g_list, g_isolate, g_hint, g_devicesUnsaved,
             g_devicesApply, g_devicesReset},
            devices);

    showAll({g_effectDeviceLabel, g_effectDeviceCombo, g_effectListGroup,
             g_effectList, g_effectGroup, g_effectHeading, g_listen,
             g_listenNote, g_meter, g_effectsUnsaved, g_effectsApply,
             g_effectsReset},
            effects);

    for (int i = 0; i < static_cast<int>(EffectKind::Count); ++i) {
        ShowEffectControls(static_cast<EffectKind>(i), false);
    }
    if (effects) {
        ShowEffectControls(g_effectKind, true);
    }
}

void EnableEffectControls(bool enabled)
{
    const HWND controls[] = {
        g_gateThresholdLabel, g_gateThresholdEdit,
        g_gateRangeLabel, g_gateRangeEdit,
        g_gateAttackLabel, g_gateAttackEdit,
        g_gateHoldLabel, g_gateHoldEdit,
        g_gateReleaseLabel, g_gateReleaseEdit,
        g_gateLookaheadLabel, g_gateLookaheadEdit,
        g_compThresholdLabel, g_compThresholdEdit,
        g_compRatioLabel, g_compRatioEdit,
        g_compAttackLabel, g_compAttackEdit,
        g_compReleaseLabel, g_compReleaseEdit,
        g_makeupLabel, g_makeupEdit,
        g_dynamicExtend,
        g_dynFrequencyLabel, g_dynFrequencyEdit,
        g_dynQLabel, g_dynQEdit,
        g_dynThresholdLabel, g_dynThresholdEdit,
        g_dynRatioLabel, g_dynRatioEdit,
        g_dynRangeLabel, g_dynRangeEdit,
        g_dynAttackLabel, g_dynAttackEdit,
        g_dynReleaseLabel, g_dynReleaseEdit,
        g_noiseReductionLabel, g_noiseReductionEdit, g_noiseCapture,
    };
    for (HWND control : controls) {
        EnableWindow(control, enabled);
    }
}

void ClearEffectEditor()
{
    g_effectIndex = -1;
    g_effectLoading = true;
    const HWND edits[] = {
        g_gateThresholdEdit, g_gateRangeEdit, g_gateAttackEdit,
        g_gateHoldEdit, g_gateReleaseEdit, g_gateLookaheadEdit,
        g_compThresholdEdit, g_compRatioEdit, g_compAttackEdit,
        g_compReleaseEdit, g_makeupEdit, g_noiseReductionEdit,
        g_dynFrequencyEdit, g_dynQEdit, g_dynThresholdEdit, g_dynRatioEdit,
        g_dynRangeEdit, g_dynAttackEdit, g_dynReleaseEdit,
    };
    for (HWND edit : edits) {
        SetWindowTextW(edit, L"");
    }
    SendMessageW(g_dynamicExtend, BM_SETCHECK, BST_UNCHECKED, 0);
    for (int i = 0; i < static_cast<int>(EffectKind::Count); ++i) {
        ListView_SetCheckState(g_effectList, i, FALSE);
    }
    SetWindowTextW(g_effectHeading, L"Select an effect");
    SetWindowTextW(g_noiseNote, L"");
    g_effectLoading = false;
}

void ShowEffectForSelected()
{
    if (!ValidEffectIndex()) {
        ClearEffectEditor();
        return;
    }

    const apo::ApoSettings& settings = g_endpoints[g_effectIndex].settings;

    g_effectLoading = true;
    SetEditNumber(g_noiseReductionEdit, settings.noise.reductionDb);
    SetEditNumber(g_gateThresholdEdit, settings.gate.thresholdDb);
    SetEditNumber(g_gateRangeEdit, settings.gate.rangeDb);
    SetEditNumber(g_gateAttackEdit, settings.gate.attackMs);
    SetEditNumber(g_gateHoldEdit, settings.gate.holdMs);
    SetEditNumber(g_gateReleaseEdit, settings.gate.releaseMs);
    SetEditNumber(g_gateLookaheadEdit, settings.gate.lookaheadMs);
    SetEditNumber(g_compThresholdEdit, settings.compressor.thresholdDb);
    SetEditNumber(g_compRatioEdit, settings.compressor.ratio);
    SetEditNumber(g_compAttackEdit, settings.compressor.attackMs);
    SetEditNumber(g_compReleaseEdit, settings.compressor.releaseMs);
    SetEditNumber(g_makeupEdit, settings.makeupDb);
    SetEditNumber(g_dynFrequencyEdit, settings.dynamics.frequencyHz);
    SetEditNumber(g_dynQEdit, settings.dynamics.q);
    SetEditNumber(g_dynThresholdEdit, settings.dynamics.thresholdDb);
    SetEditNumber(g_dynRatioEdit, settings.dynamics.ratio);
    SetEditNumber(g_dynRangeEdit, settings.dynamics.rangeDb);
    SetEditNumber(g_dynAttackEdit, settings.dynamics.attackMs);
    SetEditNumber(g_dynReleaseEdit, settings.dynamics.releaseMs);
    SendMessageW(g_dynamicExtend, BM_SETCHECK,
                 settings.dynamics.expand ? BST_CHECKED : BST_UNCHECKED, 0);

    for (int i = 0; i < static_cast<int>(EffectKind::Count); ++i) {
        ListView_SetCheckState(
            g_effectList, i,
            EffectEnabled(settings, static_cast<EffectKind>(i)) ? TRUE
                                                               : FALSE);
    }
    ListView_SetItemState(g_effectList, static_cast<int>(g_effectKind),
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);

    SetWindowTextW(g_effectHeading, EffectName(g_effectKind));

    const bool hasProfile = apo::FileExists(
        apo::NoiseProfilePath(g_endpoints[g_effectIndex].id));
    SetWindowTextW(
        g_noiseNote,
        hasProfile
            ? L"Profile captured. Stay quiet for ~3 s to recapture.\r\n"
              L"Audio stops briefly."
            : L"Stay quiet for ~3 s while capturing.\r\n"
              L"Audio stops briefly.");
    g_effectLoading = false;
}

bool CommitEffectEditor()
{
    if (!ValidEffectIndex()) {
        return true;
    }

    apo::ApoSettings settings = g_endpoints[g_effectIndex].settings;
    if (!ReadEditNumber(g_noiseReductionEdit, settings.noise.reductionDb) ||
        !ReadEditNumber(g_gateThresholdEdit, settings.gate.thresholdDb) ||
        !ReadEditNumber(g_gateRangeEdit, settings.gate.rangeDb) ||
        !ReadEditNumber(g_gateAttackEdit, settings.gate.attackMs) ||
        !ReadEditNumber(g_gateHoldEdit, settings.gate.holdMs) ||
        !ReadEditNumber(g_gateReleaseEdit, settings.gate.releaseMs) ||
        !ReadEditNumber(g_gateLookaheadEdit, settings.gate.lookaheadMs) ||
        !ReadEditNumber(g_compThresholdEdit,
                        settings.compressor.thresholdDb) ||
        !ReadEditNumber(g_compRatioEdit, settings.compressor.ratio) ||
        !ReadEditNumber(g_compAttackEdit, settings.compressor.attackMs) ||
        !ReadEditNumber(g_compReleaseEdit, settings.compressor.releaseMs) ||
        !ReadEditNumber(g_makeupEdit, settings.makeupDb) ||
        !ReadEditNumber(g_dynFrequencyEdit, settings.dynamics.frequencyHz) ||
        !ReadEditNumber(g_dynQEdit, settings.dynamics.q) ||
        !ReadEditNumber(g_dynThresholdEdit,
                        settings.dynamics.thresholdDb) ||
        !ReadEditNumber(g_dynRatioEdit, settings.dynamics.ratio) ||
        !ReadEditNumber(g_dynRangeEdit, settings.dynamics.rangeDb) ||
        !ReadEditNumber(g_dynAttackEdit, settings.dynamics.attackMs) ||
        !ReadEditNumber(g_dynReleaseEdit, settings.dynamics.releaseMs)) {
        MessageBoxW(g_window, L"Enter a valid number in every setting.",
                    L"Better Micro APO", MB_ICONWARNING);
        return false;
    }

    settings.dynamics.expand =
        SendMessageW(g_dynamicExtend, BM_GETCHECK, 0, 0) == BST_CHECKED;
    apo::ClampSettings(settings);
    g_endpoints[g_effectIndex].settings = settings;

    // Show the clamped values so the user sees what will be saved.
    g_effectLoading = true;
    SetEditNumber(g_noiseReductionEdit, settings.noise.reductionDb);
    SetEditNumber(g_gateThresholdEdit, settings.gate.thresholdDb);
    SetEditNumber(g_gateRangeEdit, settings.gate.rangeDb);
    SetEditNumber(g_gateAttackEdit, settings.gate.attackMs);
    SetEditNumber(g_gateHoldEdit, settings.gate.holdMs);
    SetEditNumber(g_gateReleaseEdit, settings.gate.releaseMs);
    SetEditNumber(g_gateLookaheadEdit, settings.gate.lookaheadMs);
    SetEditNumber(g_compThresholdEdit, settings.compressor.thresholdDb);
    SetEditNumber(g_compRatioEdit, settings.compressor.ratio);
    SetEditNumber(g_compAttackEdit, settings.compressor.attackMs);
    SetEditNumber(g_compReleaseEdit, settings.compressor.releaseMs);
    SetEditNumber(g_makeupEdit, settings.makeupDb);
    SetEditNumber(g_dynFrequencyEdit, settings.dynamics.frequencyHz);
    SetEditNumber(g_dynQEdit, settings.dynamics.q);
    SetEditNumber(g_dynThresholdEdit, settings.dynamics.thresholdDb);
    SetEditNumber(g_dynRatioEdit, settings.dynamics.ratio);
    SetEditNumber(g_dynRangeEdit, settings.dynamics.rangeDb);
    SetEditNumber(g_dynAttackEdit, settings.dynamics.attackMs);
    SetEditNumber(g_dynReleaseEdit, settings.dynamics.releaseMs);
    g_effectLoading = false;
    return true;
}

void MarkSettingsDirty()
{
    if (g_effectLoading || !ValidEffectIndex()) {
        return;
    }
    g_endpoints[g_effectIndex].settingsDirty = true;
    UpdateEffectsUi();
}

// ---------------------------------------------------------------------------
// Meter and audition
// ---------------------------------------------------------------------------

void ResetMeter()
{
    g_clipHoldUntil = 0;
    g_dropTotal = 0;
    g_meterHistory.clear();
    g_meterText = L"Peak: --  RMS: --";
    if (g_meter != nullptr) {
        SetWindowTextW(g_meter, g_meterText.c_str());
    }
    apo::TakeMonitorLevel();
}

void UpdateMeter()
{
    const DWORD now = GetTickCount();
    const apo::MonitorLevel level = apo::TakeMonitorLevel();
    g_dropTotal += level.drops;
    if (level.clipped) {
        g_clipHoldUntil = now + 2000;
    }

    // Keep the last 500 ms of 100 ms blocks. The displayed peak is the maximum
    // over that window and the RMS is computed from the summed energy, which
    // makes both readings much steadier than the raw per-timer values.
    g_meterHistory.push_back(
        MeterEntry{now, level.peak, level.sumSquares, level.samples});
    while (!g_meterHistory.empty() &&
           now - g_meterHistory.front().tick > kMeterWindowMs) {
        g_meterHistory.pop_front();
    }

    double peak = 0.0;
    double sumSquares = 0.0;
    unsigned long long samples = 0;
    for (const MeterEntry& entry : g_meterHistory) {
        peak = std::max(peak, entry.peak);
        sumSquares += entry.sumSquares;
        samples += entry.samples;
    }

    wchar_t buffer[96]{};
    if (peak > 0.0) {
        swprintf_s(buffer, L"Peak: %.1f dBFS", 20.0 * std::log10(peak));
    } else {
        swprintf_s(buffer, L"Peak: --");
    }
    std::wstring text = buffer;

    if (samples != 0) {
        const double rms =
            std::sqrt(sumSquares / static_cast<double>(samples));
        if (rms > 0.0) {
            swprintf_s(buffer, L"  RMS: %.1f dBFS", 20.0 * std::log10(rms));
        } else {
            swprintf_s(buffer, L"  RMS: -inf dBFS");
        }
    } else {
        swprintf_s(buffer, L"  RMS: --");
    }
    text += buffer;

    if (now < g_clipHoldUntil) {
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

bool IsListening()
{
    return g_listen != nullptr &&
           SendMessageW(g_listen, BM_GETCHECK, 0, 0) == BST_CHECKED &&
           apo::IsMonitorActive();
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
    if (!ValidEffectIndex()) {
        return false;
    }

    const std::wstring error =
        apo::StartMonitor(g_endpoints[g_effectIndex].id);
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

// ---------------------------------------------------------------------------
// UI state updates
// ---------------------------------------------------------------------------

void SetApplyHighlight(HWND button, bool dirty)
{
    LONG style = GetWindowLongW(button, GWL_STYLE);
    style &= ~(BS_PUSHBUTTON | BS_DEFPUSHBUTTON);
    style |= dirty ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON;
    SetWindowLongW(button, GWL_STYLE, style);
    SetWindowTextW(button, dirty ? L"Apply changes" : L"Apply");
    SendMessageW(button, WM_SETFONT,
                 reinterpret_cast<WPARAM>(dirty ? g_boldFont : g_font), TRUE);
    InvalidateRect(button, nullptr, TRUE);
}

void UpdateFilesUi()
{
    const bool stale = g_files.DllStale();
    SetWindowTextW(g_dllStatus,
                   !g_files.dllPresent
                       ? L"missing"
                       : (stale ? L"outdated" : L"present"));
    SetWindowTextW(g_comStatus,
                   g_files.comRegistered
                       ? L"registered"
                       : (g_files.registeredPath.empty() ? L"not registered"
                                                        : L"wrong path"));
    SetWindowTextW(g_apoStatus,
                   g_files.apoKeyPresent ? L"present" : L"missing");

    SetWindowTextW(
        g_overall,
        g_files.FullyInstalled()
            ? (stale ? L"Installed (DLL outdated)" : L"Installed")
            : (g_files.AnyPresent() ? L"Partial" : L"Not installed"));

    SetWindowTextW(g_install, stale ? L"Update" : L"Install");
    SetWindowTextW(
        g_filesNote,
        stale ? L"The installed DLL is older than this app. Click Update to "
                L"replace it; audio stops briefly."
              : L"Install and Uninstall stop audio for about a second.");

    EnableWindow(g_install, !g_busy && (!g_files.FullyInstalled() || stale));
    EnableWindow(g_uninstall, !g_busy && g_files.AnyPresent());
    EnableWindow(g_refresh, !g_busy);
    EnableWindow(g_logButton, !g_busy);

    if (g_tabs != nullptr) {
        InvalidateRect(g_tabs, nullptr, TRUE);
    }
}

void UpdateDevicesUi()
{
    const bool available = DevicesAvailable();
    const bool hasDevice =
        g_deviceIndex >= 0 &&
        g_deviceIndex < static_cast<int>(g_endpoints.size());
    const bool dirty = DevicesDirty();

    EnableWindow(g_list, !g_busy && available);
    EnableWindow(g_isolate,
                 !g_busy && available && hasDevice &&
                     (g_endpoints[g_deviceIndex].compositePresent ||
                      g_endpoints[g_deviceIndex].settings.isolateEffects));
    EnableWindow(g_devicesApply, !g_busy && available && dirty);
    EnableWindow(g_devicesReset, !g_busy && available && dirty);
    SetApplyHighlight(g_devicesApply, dirty);
    SetWindowTextW(g_devicesUnsaved, dirty ? L"Unsaved changes" : L"");

    if (!available) {
        SetWindowTextW(g_hint, L"Install the APO files first.");
    } else if (g_endpoints.empty()) {
        SetWindowTextW(g_hint, L"No capture devices found.");
    } else {
        SetWindowTextW(g_hint,
                       L"Check the devices to apply the effect. Apply stops "
                       L"audio briefly.");
    }
}

void UpdateEffectsUi()
{
    const bool available = EffectsAvailable();
    const bool hasDevice = ValidEffectIndex();
    const bool dirty = EffectsDirty();

    // Lock the device selector while edits are pending so a half-edited
    // device cannot be left behind. Apply or Reset unlocks it.
    EnableWindow(g_effectDeviceCombo, !g_busy && available && !dirty);
    EnableWindow(g_effectList, !g_busy && available && hasDevice);
    EnableEffectControls(!g_busy && available && hasDevice);
    EnableWindow(g_listen, !g_busy && hasDevice);
    EnableWindow(g_effectsApply, !g_busy && available && dirty);
    EnableWindow(g_effectsReset, !g_busy && available && dirty);
    SetApplyHighlight(g_effectsApply, dirty);
    SetWindowTextW(g_effectsUnsaved, dirty ? L"Unsaved changes" : L"");
}

void ShowDeviceForSelected()
{
    g_loading = true;
    const bool checked =
        g_deviceIndex >= 0 &&
        g_deviceIndex < static_cast<int>(g_endpoints.size()) &&
        g_endpoints[g_deviceIndex].settings.isolateEffects;
    SendMessageW(g_isolate, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED,
                 0);
    g_loading = false;
}

void UpdateViewAvailability()
{
    if (g_view == View::Effects && !EffectsAvailable()) {
        SetActiveView(DevicesAvailable() ? View::Devices : View::Install);
        return;
    }
    if (g_view == View::Devices && !DevicesAvailable()) {
        SetActiveView(View::Install);
        return;
    }
    if (g_tabs != nullptr) {
        InvalidateRect(g_tabs, nullptr, TRUE);
    }
}

void SetActiveView(View view)
{
    if (view == View::Effects && !EffectsAvailable()) {
        view = DevicesAvailable() ? View::Devices : View::Install;
    }
    if (view == View::Devices && !DevicesAvailable()) {
        view = View::Install;
    }

    g_view = view;
    g_tabLoading = true;
    TabCtrl_SetCurSel(g_tabs, ViewToTab(view));
    g_tabLoading = false;

    UpdateViewVisibility();
    Relayout();
    UpdateFilesUi();
    UpdateDevicesUi();
    UpdateEffectsUi();
}

void OnIsolateClicked()
{
    if (g_deviceIndex < 0 ||
        g_deviceIndex >= static_cast<int>(g_endpoints.size())) {
        return;
    }

    const bool checked =
        SendMessageW(g_isolate, BM_GETCHECK, 0, 0) == BST_CHECKED;
    apo::Endpoint& endpoint = g_endpoints[g_deviceIndex];
    endpoint.settings.isolateEffects = checked;
    endpoint.isolationDirty = checked != endpoint.isolateSaved;
    UpdateDevicesUi();
}

void ResetDevices()
{
    if (g_busy) {
        return;
    }

    // Revert the checkboxes to the bindings that are currently persisted.
    g_loading = true;
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        ListView_SetCheckState(g_list, static_cast<int>(i),
                               g_endpoints[i].Enabled() ? TRUE : FALSE);
    }
    g_loading = false;

    // Restore only the isolation option; effect edits made in the Effects
    // view must survive a Devices reset.
    for (apo::Endpoint& endpoint : g_endpoints) {
        if (!endpoint.isolationDirty) {
            continue;
        }
        apo::ApoSettings persisted;
        apo::LoadSettings(endpoint.id, persisted);
        endpoint.settings.isolateEffects = persisted.isolateEffects;
        endpoint.isolationDirty = false;
    }

    ShowDeviceForSelected();
    UpdateDevicesUi();
}

void ResetEffects()
{
    if (g_busy) {
        return;
    }

    // Discard unsaved effect edits by reloading the persisted settings. Keep
    // the Devices view's isolation edit, which lives in the same file.
    for (apo::Endpoint& endpoint : g_endpoints) {
        if (!endpoint.settingsDirty) {
            continue;
        }
        apo::ApoSettings persisted;
        apo::LoadSettings(endpoint.id, persisted);
        const bool isolateEdit = endpoint.settings.isolateEffects;
        endpoint.settings = persisted;
        endpoint.settings.isolateEffects = isolateEdit;
        endpoint.settingsDirty = false;
    }

    ShowEffectForSelected();
    UpdateEffectsUi();
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void RebuildDeviceList(const std::wstring& selectedId)
{
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

        std::wstring apoText = endpoint.Enabled() ? L"Installed" : L"-";
        ListView_SetItemText(g_list, row, 2, apoText.data());

        std::wstring notes;
        if (endpoint.disableSysFx) {
            notes = L"System effects disabled";
        }
        if (endpoint.compositePresent) {
            if (!notes.empty()) {
                notes += L"; ";
            }
            notes += L"effect pack";
            if (endpoint.settings.isolateEffects && endpoint.Enabled()) {
                notes += endpoint.isolatePendingCleanup
                             ? L"; other effects pending Apply"
                             : L"; other effects ignored";
            }
        }
        if (endpoint.streamOurs) {
            if (!notes.empty()) {
                notes += L"; ";
            }
            notes += L"also in Stream";
        }
        ListView_SetItemText(g_list, row, 3, notes.data());

        ListView_SetCheckState(g_list, row, endpoint.Enabled());
    }

    int selectedRow = -1;
    if (!selectedId.empty()) {
        for (size_t i = 0; i < g_endpoints.size(); ++i) {
            if (_wcsicmp(g_endpoints[i].id.c_str(), selectedId.c_str()) == 0) {
                selectedRow = static_cast<int>(i);
                break;
            }
        }
    }
    if (selectedRow < 0 && !g_endpoints.empty()) {
        selectedRow = 0;
    }
    if (selectedRow >= 0) {
        ListView_SetItemState(g_list, selectedRow,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, selectedRow, FALSE);
    }
    g_deviceIndex = selectedRow;
    g_loading = false;
}

void RebuildEffectCombo(const std::wstring& selectedId)
{
    g_comboLoading = true;
    SendMessageW(g_effectDeviceCombo, CB_RESETCONTENT, 0, 0);

    int selectedItem = -1;
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        const apo::Endpoint& endpoint = g_endpoints[i];
        if (!endpoint.Enabled()) {
            continue;
        }

        const int item = static_cast<int>(SendMessageW(
            g_effectDeviceCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(endpoint.name.c_str())));
        SendMessageW(g_effectDeviceCombo, CB_SETITEMDATA, item,
                     static_cast<LPARAM>(i));
        if (selectedItem < 0 && selectedId.empty()) {
            selectedItem = item;
        }
        if (!selectedId.empty() &&
            _wcsicmp(endpoint.id.c_str(), selectedId.c_str()) == 0) {
            selectedItem = item;
        }
    }

    if (selectedItem < 0) {
        const int count =
            static_cast<int>(SendMessageW(g_effectDeviceCombo, CB_GETCOUNT, 0,
                                          0));
        if (count > 0) {
            selectedItem = 0;
        }
    }

    SendMessageW(g_effectDeviceCombo, CB_SETCURSEL, selectedItem, 0);
    if (selectedItem >= 0) {
        g_effectIndex = static_cast<int>(SendMessageW(
            g_effectDeviceCombo, CB_GETITEMDATA, selectedItem, 0));
    } else {
        g_effectIndex = -1;
    }
    g_comboLoading = false;
}

void Refresh()
{
    StopListening();

    std::wstring deviceId;
    if (g_deviceIndex >= 0 &&
        g_deviceIndex < static_cast<int>(g_endpoints.size())) {
        deviceId = g_endpoints[g_deviceIndex].id;
    }

    std::wstring effectId;
    if (g_effectIndex >= 0 &&
        g_effectIndex < static_cast<int>(g_endpoints.size())) {
        effectId = g_endpoints[g_effectIndex].id;
    }

    // Devices Apply only persists bindings and isolation. Carry unsaved
    // effect edits across the refresh so they are not silently discarded.
    std::vector<std::pair<std::wstring, apo::ApoSettings>> pendingEffects;
    if (g_preserveDirtyEffects) {
        for (const apo::Endpoint& endpoint : g_endpoints) {
            if (endpoint.settingsDirty) {
                pendingEffects.emplace_back(endpoint.id, endpoint.settings);
            }
        }
    }
    g_preserveDirtyEffects = false;

    g_deviceIndex = -1;
    g_effectIndex = -1;
    g_files = apo::GetFilesStatus();
    g_endpoints = apo::EnumEndpoints();
    for (apo::Endpoint& endpoint : g_endpoints) {
        endpoint.settingsDirty = false;
        endpoint.isolateSaved = endpoint.settings.isolateEffects;
        endpoint.isolationDirty = false;
    }
    for (apo::Endpoint& endpoint : g_endpoints) {
        if (!endpoint.Enabled()) {
            continue;
        }
        for (const auto& pending : pendingEffects) {
            if (_wcsicmp(endpoint.id.c_str(), pending.first.c_str()) == 0) {
                endpoint.settings = pending.second;
                endpoint.settingsDirty = true;
                break;
            }
        }
    }

    RebuildDeviceList(deviceId);
    RebuildEffectCombo(effectId);
    ShowDeviceForSelected();
    ShowEffectForSelected();
    UpdateFilesUi();
    UpdateDevicesUi();
    UpdateEffectsUi();
    ResizeColumns();
    if (g_tabs != nullptr) {
        InvalidateRect(g_tabs, nullptr, TRUE);
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

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

    ResizeColumns();
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

    switch (g_view) {
    case View::Install:
        LayoutInstall(contentX, contentY, contentWidth, contentHeight);
        break;
    case View::Devices:
        LayoutDevices(contentX, contentY, contentWidth, contentHeight);
        break;
    case View::Effects:
        LayoutEffects(contentX, contentY, contentWidth, contentHeight);
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
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

void StartWork(std::function<apo::Result()> work, const wchar_t* status)
{
    if (g_busy) {
        return;
    }

    g_busy = true;
    SetWindowTextW(g_status, status);
    UpdateFilesUi();
    UpdateDevicesUi();
    UpdateEffectsUi();

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
    g_preserveDirtyEffects = true;
    const bool update = g_files.DllStale();
    StartWork([] { return apo::InstallFiles(); },
              update ? L"Updating..." : L"Installing...");
}

void DoUninstall()
{
    if (g_busy) {
        return;
    }

    StopListening();
    g_preserveDirtyEffects = false;
    StartWork([] { return apo::UninstallFiles(); }, L"Uninstalling...");
}

void StartApply(std::vector<apo::Change> changes, const wchar_t* status)
{
    const bool wasListening = IsListening();
    std::wstring listenId;
    if (wasListening && ValidEffectIndex()) {
        listenId = g_endpoints[g_effectIndex].id;
    }

    StopListening();
    g_resumeListen = wasListening && !listenId.empty();
    g_resumeListenId = g_resumeListen ? listenId : std::wstring();

    StartWork([changes = std::move(changes)] {
        return apo::ApplyChanges(changes);
    }, status);
}

void DoApplyDevices()
{
    if (g_busy) {
        return;
    }
    if (!CommitEffectEditor()) {
        return;
    }

    std::vector<apo::Change> changes;
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        const bool checked =
            ListView_GetCheckState(g_list, static_cast<int>(i)) != FALSE;
        const bool bindingChanged = checked != g_endpoints[i].Enabled();

        // The Devices view owns binding and isolation. Effect edits are left
        // for the Effects Apply button, so only write settings when the
        // isolation option actually changed.
        const bool isolateChanged =
            g_endpoints[i].isolationDirty ||
            g_endpoints[i].isolatePendingCleanup;
        if (!bindingChanged && !isolateChanged) {
            continue;
        }

        apo::Change change;
        change.id = g_endpoints[i].id;
        change.enable = checked;
        change.settingsChanged = isolateChanged;
        if (isolateChanged) {
            if (g_endpoints[i].settingsDirty) {
                // Persist the isolation edit on top of the saved effect
                // settings, not on top of the unsaved edits in memory.
                apo::ApoSettings persisted;
                apo::LoadSettings(g_endpoints[i].id, persisted);
                persisted.isolateEffects =
                    g_endpoints[i].settings.isolateEffects;
                change.settings = persisted;
            } else {
                change.settings = g_endpoints[i].settings;
            }
        }
        changes.push_back(std::move(change));
    }
    if (changes.empty()) {
        return;
    }

    g_preserveDirtyEffects = true;
    StartApply(std::move(changes), L"Applying...");
}

void DoApplyEffects()
{
    if (g_busy) {
        return;
    }
    if (!CommitEffectEditor()) {
        return;
    }

    g_preserveDirtyEffects = false;

    std::vector<apo::Change> changes;
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        if (!g_endpoints[i].settingsDirty) {
            continue;
        }

        apo::Change change;
        change.id = g_endpoints[i].id;
        change.enable = g_endpoints[i].Enabled();
        change.settingsChanged = true;
        change.settings = g_endpoints[i].settings;
        changes.push_back(std::move(change));
    }
    if (changes.empty()) {
        return;
    }

    StartApply(std::move(changes), L"Applying...");
}

void DoCaptureNoiseProfile()
{
    if (g_busy) {
        return;
    }
    if (!CommitEffectEditor()) {
        return;
    }
    g_preserveDirtyEffects = false;
    if (!ValidEffectIndex()) {
        return;
    }
    if (!apo::IsDeviceActive(g_endpoints[g_effectIndex].state)) {
        MessageBoxW(g_window,
                    L"This device is not active. Enable it in Windows and try "
                    L"again.",
                    L"Better Micro APO", MB_ICONWARNING);
        return;
    }

    const int index = g_effectIndex;
    const std::wstring id = g_endpoints[index].id;
    const bool bound = g_endpoints[index].Enabled();
    const apo::ApoSettings original = g_endpoints[index].settings;
    const bool isolateDirty = g_endpoints[index].isolationDirty;
    const bool isolatePending = original.isolateEffects;

    // Capture restarts audio and rewrites the settings file. Do not persist a
    // pending isolation edit as part of that write; the Devices view owns it.
    apo::ApoSettings captureBase = original;
    if (isolateDirty) {
        apo::ApoSettings persisted;
        apo::LoadSettings(id, persisted);
        captureBase.isolateEffects = persisted.isolateEffects;
    }

    // The profile must be raw room noise: the noise stage runs first, before
    // the compressor and makeup gain. Disabling every stage makes the APO a
    // plain passthrough for the duration of the capture.
    apo::ApoSettings snapshot = captureBase;
    snapshot.noise.enabled = false;
    snapshot.gate.enabled = false;
    snapshot.dynamics.enabled = false;
    snapshot.compressor.enabled = false;
    snapshot.makeupEnabled = false;
    snapshot.mono = false;

    StopListening();

    g_workKind = WorkKind::CaptureNoise;
    g_captureIndex = index;
    g_captureSettings = captureBase;
    g_captureSucceeded = false;
    g_captureIsolationDirty = isolateDirty;
    g_captureIsolatePending = isolatePending;
    g_captureIsolateSaved = captureBase.isolateEffects;
    g_busy = true;
    SetWindowTextW(g_status, L"Capturing noise profile...");
    UpdateFilesUi();
    UpdateDevicesUi();
    UpdateEffectsUi();

    g_worker = std::thread([id, bound, captureBase, snapshot] {
        apo::Result result;
        std::wstring error;

        auto applySettings = [&](const apo::ApoSettings& settings) {
            apo::Change change;
            change.id = id;
            change.enable = true;
            change.settingsChanged = true;
            change.settings = settings;
            return apo::ApplyChanges({change});
        };

        if (bound) {
            const apo::Result prepared = applySettings(snapshot);
            if (!prepared.ok) {
                // The snapshot may have been saved before the failure, so put
                // the real settings back before reporting the error.
                applySettings(captureBase);
                result = prepared;
                result.summary =
                    L"Could not prepare the device for noise capture.";
                g_captureSettings = captureBase;
                g_result = result;
                PostMessageW(g_window, WM_APP_WORK_DONE, 0, 0);
                return;
            }
        }

        std::vector<float> samples;
        unsigned sampleRate = 0;
        unsigned channels = 0;
        error = apo::CaptureNoise(id, kNoiseCaptureMs, samples, sampleRate,
                                  channels);
        bool captured = error.empty();
        if (captured &&
            !apo::SaveNoiseCapture(id, sampleRate, channels, samples)) {
            error = L"Could not save the noise profile file.";
            captured = false;
        }

        apo::ApoSettings final = captureBase;
        if (captured) {
            final.noise.enabled = true;
        }

        if (bound) {
            const apo::Result restored = applySettings(final);
            if (!restored.ok) {
                if (error.empty()) {
                    error = L"Profile saved, but applying the settings failed.";
                    result = restored;
                }
            }
        } else if (!apo::SaveSettings(id, final) && error.empty()) {
            error = L"Could not save the settings file.";
        }

        g_captureSettings = final;
        g_captureSucceeded = error.empty();
        result.ok = error.empty();
        result.summary =
            error.empty() ? L"Noise profile captured."
                          : L"Noise profile capture failed.";
        if (!error.empty()) {
            result.details = error;
        }
        g_result = result;
        PostMessageW(g_window, WM_APP_WORK_DONE, 0, 0);
    });
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

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

void CreateControls(HWND window)
{
    g_title = CreateStatic(window, IDC_TITLE, L"Better Micro APO", SS_LEFT);
    ApplyFont(g_title, g_titleFont);
    g_refresh = CreateButton(window, IDC_REFRESH, L"Refresh");

    // The tab control must exist before the view controls so the view content
    // is drawn on top of its body area.
    g_tabs = CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | TCS_HOTTRACK,
        0, 0, 0, 0,
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TABS)),
        g_instance, nullptr);
    ApplyFont(g_tabs, g_font);
    const wchar_t* tabNames[] = {L"Install", L"Devices", L"Effects"};
    for (int i = 0; i < 3; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(tabNames[i]);
        TabCtrl_InsertItem(g_tabs, i, &item);
    }

    // Install view.
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

    // Devices view.
    g_groupDevices =
        CreateGroup(window, IDC_GROUP_DEVICES, L"Devices (PreMix)");
    g_list = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS |
            LVS_SINGLESEL,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), g_instance,
        nullptr);
    ListView_SetExtendedListViewStyle(
        g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                    LVS_EX_INFOTIP);
    ApplyFont(g_list, g_font);
    AddColumn(0, L"Device", 300);
    AddColumn(1, L"State", 90);
    AddColumn(2, L"APO", 130);
    AddColumn(3, L"Notes", 170);
    g_isolate = CreateCheck(window, IDC_ISOLATE, L"Ignore other effects");
    g_hint = CreateStatic(window, IDC_HINT, L"", SS_LEFT);
    g_devicesUnsaved =
        CreateStatic(window, IDC_DEVICES_UNSAVED, L"", SS_RIGHT);
    g_devicesApply = CreateButton(window, IDC_DEVICES_APPLY, L"Apply");
    g_devicesReset = CreateButton(window, IDC_DEVICES_RESET, L"Reset");

    // Effects view.
    g_effectDeviceLabel =
        CreateStatic(window, IDC_EFFECT_DEVICE_LABEL, L"Device:", SS_LEFT);
    g_effectDeviceCombo = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EFFECT_DEVICE_COMBO)),
        g_instance, nullptr);
    ApplyFont(g_effectDeviceCombo, g_font);

    g_effectListGroup =
        CreateGroup(window, IDC_EFFECT_LIST_GROUP, L"Effects");
    g_effectList = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EFFECT_LIST)),
        g_instance, nullptr);
    ListView_SetExtendedListViewStyle(
        g_effectList,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ApplyFont(g_effectList, g_font);
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
        CreateGroup(window, IDC_EFFECT_GROUP, L"Effect parameters");
    g_effectHeading =
        CreateStatic(window, IDC_EFFECT_HEADING, L"Noise reduction", SS_LEFT);
    ApplyFont(g_effectHeading, g_boldFont);
    g_monoNote = CreateStatic(
        window, IDC_MONO_NOTE,
        L"Copies the processed left channel to every other channel.",
        SS_LEFT);

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
    g_gateLookaheadLabel = CreateStatic(
        window, IDC_GATE_LOOKAHEAD_LABEL, L"Lookahead (ms)", SS_LEFT);
    g_gateLookaheadEdit = CreateEdit(window, IDC_GATE_LOOKAHEAD_EDIT);

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

    g_dynamicExtend =
        CreateCheck(window, IDC_DYNAMIC_EXTEND, L"Extend quiet lows");
    g_dynFrequencyLabel = CreateStatic(
        window, IDC_DYN_FREQUENCY_LABEL, L"Frequency (Hz)", SS_LEFT);
    g_dynFrequencyEdit = CreateEdit(window, IDC_DYN_FREQUENCY_EDIT);
    g_dynQLabel = CreateStatic(window, IDC_DYN_Q_LABEL, L"Q", SS_LEFT);
    g_dynQEdit = CreateEdit(window, IDC_DYN_Q_EDIT);
    g_dynThresholdLabel = CreateStatic(
        window, IDC_DYN_THRESHOLD_LABEL, L"Threshold (dBFS)", SS_LEFT);
    g_dynThresholdEdit = CreateEdit(window, IDC_DYN_THRESHOLD_EDIT);
    g_dynRatioLabel =
        CreateStatic(window, IDC_DYN_RATIO_LABEL, L"Ratio (:1)", SS_LEFT);
    g_dynRatioEdit = CreateEdit(window, IDC_DYN_RATIO_EDIT);
    g_dynRangeLabel =
        CreateStatic(window, IDC_DYN_RANGE_LABEL, L"Range (dB)", SS_LEFT);
    g_dynRangeEdit = CreateEdit(window, IDC_DYN_RANGE_EDIT);
    g_dynAttackLabel = CreateStatic(window, IDC_DYN_ATTACK_LABEL,
                                    L"Attack (ms)", SS_LEFT);
    g_dynAttackEdit = CreateEdit(window, IDC_DYN_ATTACK_EDIT);
    g_dynReleaseLabel = CreateStatic(window, IDC_DYN_RELEASE_LABEL,
                                     L"Release (ms)", SS_LEFT);
    g_dynReleaseEdit = CreateEdit(window, IDC_DYN_RELEASE_EDIT);

    g_noiseReductionLabel = CreateStatic(
        window, IDC_NOISE_REDUCTION_LABEL, L"Reduction (dB)", SS_LEFT);
    g_noiseReductionEdit = CreateEdit(window, IDC_NOISE_REDUCTION_EDIT);
    g_noiseCapture =
        CreateButton(window, IDC_NOISE_CAPTURE, L"Capture noise profile");
    g_noiseNote = CreateStatic(window, IDC_NOISE_NOTE, L"", SS_LEFT);

    g_listen = CreateCheck(window, IDC_LISTEN, L"Listen to this device");
    g_listenNote = CreateStatic(
        window, IDC_LISTEN_NOTE,
        L"Plays through the default output.\r\n"
        L"Use headphones to avoid feedback.",
        SS_LEFT);
    g_meter =
        CreateStatic(window, IDC_METER, L"Peak: --  RMS: --", SS_LEFT);

    g_effectsUnsaved =
        CreateStatic(window, IDC_EFFECTS_UNSAVED, L"", SS_RIGHT);
    g_effectsApply = CreateButton(window, IDC_EFFECTS_APPLY, L"Apply");
    g_effectsReset = CreateButton(window, IDC_EFFECTS_RESET, L"Reset");

    // Shared status bar.
    g_status = CreateStatic(window, IDC_STATUS, L"", SS_LEFT);
    g_logButton = CreateButton(window, IDC_LOG, L"Open log");
}

void OnEffectListChanged(const NMLISTVIEW& item)
{
    if (g_effectLoading) {
        return;
    }

    const bool stateChanged = (item.uChanged & LVIF_STATE) != 0;
    if (!stateChanged) {
        return;
    }

    const bool checkChanged =
        ((item.uOldState ^ item.uNewState) & LVIS_STATEIMAGEMASK) != 0;
    if (checkChanged && ValidEffectIndex() && item.iItem >= 0 &&
        item.iItem < static_cast<int>(EffectKind::Count)) {
        const bool checked =
            ((item.uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
        SetEffectEnabled(g_endpoints[g_effectIndex].settings,
                         static_cast<EffectKind>(item.iItem), checked);
        MarkSettingsDirty();
    }

    const bool selectionChanged =
        ((item.uOldState ^ item.uNewState) & LVIS_SELECTED) != 0;
    if (selectionChanged && (item.uNewState & LVIS_SELECTED) != 0 &&
        item.iItem >= 0 &&
        item.iItem < static_cast<int>(EffectKind::Count)) {
        if (!CommitEffectEditor()) {
            g_effectLoading = true;
            ListView_SetItemState(g_effectList,
                                  static_cast<int>(g_effectKind),
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            g_effectLoading = false;
            return;
        }
        g_effectKind = static_cast<EffectKind>(item.iItem);
        UpdateViewVisibility();
        ShowEffectForSelected();
        Relayout();
    }
}

void OnDeviceListChanged(const NMLISTVIEW& item)
{
    if (g_loading) {
        return;
    }

    const bool stateChanged = (item.uChanged & LVIF_STATE) != 0;
    const bool selectionChanged =
        stateChanged && ((item.uOldState ^ item.uNewState) & LVIS_SELECTED) != 0;
    if (selectionChanged && (item.uNewState & LVIS_SELECTED) != 0) {
        g_deviceIndex = item.iItem;
        ShowDeviceForSelected();
    }
    UpdateDevicesUi();
}

void OnEffectComboChanged()
{
    if (g_comboLoading) {
        return;
    }
    if (!CommitEffectEditor()) {
        return;
    }

    const int item = static_cast<int>(
        SendMessageW(g_effectDeviceCombo, CB_GETCURSEL, 0, 0));
    if (item >= 0) {
        g_effectIndex = static_cast<int>(
            SendMessageW(g_effectDeviceCombo, CB_GETITEMDATA, item, 0));
    } else {
        g_effectIndex = -1;
    }

    ShowEffectForSelected();
    UpdateEffectsUi();
}

LRESULT HandleNotify(LPARAM lParam)
{
    auto* header = reinterpret_cast<NMHDR*>(lParam);

    if (header->hwndFrom == g_tabs) {
        switch (header->code) {
        case TCN_SELCHANGING: {
            if (g_tabLoading) {
                break;
            }
            const int target = TabCtrl_GetCurFocus(g_tabs);
            if (!TabAvailable(target)) {
                return TRUE;
            }
            break;
        }
        case TCN_SELCHANGE: {
            if (g_tabLoading) {
                break;
            }
            const int selected = TabCtrl_GetCurSel(g_tabs);
            if (!TabAvailable(selected)) {
                g_tabLoading = true;
                TabCtrl_SetCurSel(g_tabs, ViewToTab(g_view));
                g_tabLoading = false;
                return 0;
            }
            SetActiveView(TabToView(selected));
            break;
        }
        case NM_CUSTOMDRAW: {
            auto* draw = reinterpret_cast<NMTBCUSTOMDRAW*>(lParam);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                if (!TabAvailable(static_cast<int>(draw->nmcd.dwItemSpec))) {
                    draw->clrText = GetSysColor(COLOR_GRAYTEXT);
                    return CDRF_NEWFONT;
                }
                return CDRF_DODEFAULT;
            }
            break;
        }
        default:
            break;
        }
        return 0;
    }

    if (header->hwndFrom == g_effectList) {
        if (header->code == LVN_ITEMCHANGED) {
            OnEffectListChanged(*reinterpret_cast<NMLISTVIEW*>(lParam));
        }
        return 0;
    }

    if (header->hwndFrom == g_list) {
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

        case LVN_ITEMCHANGED:
            OnDeviceListChanged(*reinterpret_cast<NMLISTVIEW*>(lParam));
            break;

        case LVN_GETINFOTIP: {
            auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
            const size_t index = static_cast<size_t>(tip->iItem);
            if (index < g_endpoints.size()) {
                std::wstring text = g_endpoints[index].id;
                if (g_endpoints[index].compositePresent) {
                    text += L"\r\nUses an effect pack (composite FX)";
                }
                if (g_endpoints[index].compositePresent &&
                    g_endpoints[index].settings.isolateEffects) {
                    text += g_endpoints[index].isolatePendingCleanup
                                ? L"\r\nApply to finish ignoring the other effects"
                                : L"\r\nOther effects are ignored on this device";
                }
                if (g_endpoints[index].compositeOurs) {
                    text += L"\r\nAlso bound in the composite stream chain";
                }
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
        return 0;
    }

    return 0;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        CreateFonts();
        CreateControls(window);
        UpdateViewVisibility();
        Refresh();
        SetActiveView(DefaultView());
        return 0;
    }

    case WM_SIZE:
        if (g_list) {
            Layout(LOWORD(lParam), HIWORD(lParam));
        }
        break;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(940);
        info->ptMinTrackSize.y = Scale(720);
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        HWND control = reinterpret_cast<HWND>(lParam);
        if (control == g_effectsUnsaved || control == g_devicesUnsaved) {
            SetTextColor(dc, RGB(200, 0, 0));
        } else {
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        }
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_REFRESH:
            g_preserveDirtyEffects = false;
            Refresh();
            UpdateViewAvailability();
            break;
        case IDC_INSTALL:
            DoInstall();
            break;
        case IDC_UNINSTALL:
            DoUninstall();
            break;
        case IDC_DEVICES_APPLY:
            DoApplyDevices();
            break;
        case IDC_DEVICES_RESET:
            ResetDevices();
            break;
        case IDC_EFFECTS_APPLY:
            DoApplyEffects();
            break;
        case IDC_EFFECTS_RESET:
            ResetEffects();
            break;
        case IDC_LOG:
            DoOpenLog();
            break;
        case IDC_EFFECT_DEVICE_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                OnEffectComboChanged();
            }
            break;
        case IDC_GATE_THRESHOLD_EDIT:
        case IDC_GATE_RANGE_EDIT:
        case IDC_GATE_ATTACK_EDIT:
        case IDC_GATE_HOLD_EDIT:
        case IDC_GATE_RELEASE_EDIT:
        case IDC_GATE_LOOKAHEAD_EDIT:
        case IDC_COMP_THRESHOLD_EDIT:
        case IDC_COMP_RATIO_EDIT:
        case IDC_COMP_ATTACK_EDIT:
        case IDC_COMP_RELEASE_EDIT:
        case IDC_MAKEUP_EDIT:
        case IDC_NOISE_REDUCTION_EDIT:
        case IDC_DYN_FREQUENCY_EDIT:
        case IDC_DYN_Q_EDIT:
        case IDC_DYN_THRESHOLD_EDIT:
        case IDC_DYN_RATIO_EDIT:
        case IDC_DYN_RANGE_EDIT:
        case IDC_DYN_ATTACK_EDIT:
        case IDC_DYN_RELEASE_EDIT:
            if (HIWORD(wParam) == EN_CHANGE) {
                MarkSettingsDirty();
            }
            break;
        case IDC_DYNAMIC_EXTEND:
            if (HIWORD(wParam) == BN_CLICKED) {
                MarkSettingsDirty();
            }
            break;
        case IDC_ISOLATE:
            if (HIWORD(wParam) == BN_CLICKED) {
                OnIsolateClicked();
            }
            break;
        case IDC_NOISE_CAPTURE:
            if (HIWORD(wParam) == BN_CLICKED) {
                DoCaptureNoiseProfile();
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

    case WM_NOTIFY:
        return HandleNotify(lParam);

    case WM_APP_WORK_DONE: {
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_busy = false;

        const WorkKind kind = g_workKind;
        const int captureIndex = g_captureIndex;
        const bool captureOk = g_captureSucceeded;
        g_workKind = WorkKind::Generic;
        g_captureIndex = -1;
        g_captureSucceeded = false;

        if (kind == WorkKind::CaptureNoise && captureOk &&
            captureIndex >= 0 &&
            captureIndex < static_cast<int>(g_endpoints.size())) {
            // Keep unsaved edits on the other devices. The captured device was
            // saved by the worker, so its effect edits are no longer dirty.
            apo::Endpoint& endpoint = g_endpoints[captureIndex];
            endpoint.settings = g_captureSettings;
            endpoint.settingsDirty = false;
            if (g_captureIsolationDirty) {
                // The capture deliberately did not persist the pending
                // isolation edit, so keep it pending for the Devices view.
                endpoint.settings.isolateEffects = g_captureIsolatePending;
                endpoint.isolationDirty = true;
                endpoint.isolateSaved = g_captureIsolateSaved;
            } else {
                endpoint.isolateSaved = endpoint.settings.isolateEffects;
                endpoint.isolationDirty = false;
            }
            g_captureIsolationDirty = false;
            ShowEffectForSelected();
            UpdateFilesUi();
            UpdateDevicesUi();
            UpdateEffectsUi();
            UpdateViewAvailability();
        } else {
            // A failed operation may have changed nothing, so keep unsaved
            // effect edits instead of reloading over them.
            if (!g_result.ok) {
                g_preserveDirtyEffects = true;
            }
            Refresh();
            UpdateViewAvailability();
        }

        // Restart the audition stream if it was active before Apply. The
        // endpoint id is checked so a disabled or removed device does not
        // spring back to life.
        if (kind == WorkKind::Generic && g_resumeListen) {
            const std::wstring listenId = g_resumeListenId;
            g_resumeListen = false;
            g_resumeListenId.clear();
            if (ValidEffectIndex() &&
                _wcsicmp(g_endpoints[g_effectIndex].id.c_str(),
                         listenId.c_str()) == 0) {
                SendMessageW(g_listen, BM_SETCHECK, BST_CHECKED, 0);
                StartListening();
            }
        }

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
    }

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
    text += L"dll_up_to_date = ";
    text += !files.dllPresent
                ? L"n/a\r\n"
                : (files.dllUpToDate ? L"yes\r\n" : L"no\r\n");
    text += L"com_registered = ";
    text += files.comRegistered ? L"yes\r\n" : L"no\r\n";
    text += L"com_path = " + files.registeredPath + L"\r\n";
    text += L"apo_key = ";
    text += files.apoKeyPresent ? L"yes\r\n" : L"no\r\n";
    text += L"overall = ";
    text += files.FullyInstalled()
                ? (files.DllStale() ? L"installed (dll outdated)\r\n"
                                    : L"installed\r\n")
                : (files.AnyPresent() ? L"partial\r\n" : L"none\r\n");
    text += L"endpoints = " + std::to_wstring(endpoints.size()) + L"\r\n";

    for (const apo::Endpoint& endpoint : endpoints) {
        text += L"endpoint " + endpoint.id + L" | name=\"" + endpoint.name +
                L"\" | state=" + std::to_wstring(endpoint.state) +
                L" | active=" +
                (apo::IsDeviceActive(endpoint.state) ? L"yes" : L"no") +
                L" | premix=" +
                (endpoint.premixOurs ? L"installed" : L"no") +
                L" | composite=" +
                (endpoint.compositeOurs ? L"installed" : L"no") +
                L" | effect_pack=" +
                (endpoint.compositePresent ? L"yes" : L"no") +
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
                L" | gate_lookahead=" +
                FormatNumber(endpoint.settings.gate.lookaheadMs) +
                L" | dynamic=" +
                (endpoint.settings.dynamics.enabled ? L"on" : L"off") +
                L" | dynamic_mode=" +
                (endpoint.settings.dynamics.expand ? L"extend" : L"reduce") +
                L" | dynamic_frequency=" +
                FormatNumber(endpoint.settings.dynamics.frequencyHz) +
                L" | dynamic_q=" +
                FormatNumber(endpoint.settings.dynamics.q) +
                L" | dynamic_threshold=" +
                FormatNumber(endpoint.settings.dynamics.thresholdDb) +
                L" | dynamic_ratio=" +
                FormatNumber(endpoint.settings.dynamics.ratio) +
                L" | dynamic_range=" +
                FormatNumber(endpoint.settings.dynamics.rangeDb) +
                L" | dynamic_attack=" +
                FormatNumber(endpoint.settings.dynamics.attackMs) +
                L" | dynamic_release=" +
                FormatNumber(endpoint.settings.dynamics.releaseMs) +
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
                L" | gain=" +
                (endpoint.settings.makeupEnabled ? L"on" : L"off") +
                L" | makeup=" + FormatNumber(endpoint.settings.makeupDb) +
                L" | mono=" + (endpoint.settings.mono ? L"on" : L"off") +
                L" | noise=" +
                (endpoint.settings.noise.enabled ? L"on" : L"off") +
                L" | noise_reduction=" +
                FormatNumber(endpoint.settings.noise.reductionDb) +
                L" | noise_profile=" +
                (apo::FileExists(apo::NoiseProfilePath(endpoint.id)) ? L"yes"
                                                                   : L"no") +
                L" | isolate=" +
                (endpoint.settings.isolateEffects ? L"on" : L"off") +
                L" | isolate_pending=" +
                (endpoint.isolatePendingCleanup ? L"yes" : L"no") +
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
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES |
                     ICC_STANDARD_CLASSES;
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
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        Scale(1040), Scale(820), nullptr, nullptr, instance, nullptr);
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
