#include "apo.h"

#include "apo_internal.h"
#include "apo_monitor.h"

#include "ui/structure.h"
#include "ui/theme.h"
#include "ui/ui_state.h"
#include "ui/widgets.h"

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

using ui::FormatNumber;
using ui::GetNumber;
using ui::Scale;
using ui::SetNumber;

constexpr UINT WM_APP_WORK_DONE = WM_APP + 1;
constexpr UINT_PTR kMonitorTimer = 1;
constexpr DWORD kMeterWindowMs = 500;

constexpr wchar_t kWindowClass[] = L"BetterMicroApoInstaller";
constexpr wchar_t kInstanceMutex[] = L"Local\\better_micro_apo_gui_instance";


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
    SetNumber(g_noiseReductionEdit, settings.noise.reductionDb);
    SetNumber(g_gateThresholdEdit, settings.gate.thresholdDb);
    SetNumber(g_gateRangeEdit, settings.gate.rangeDb);
    SetNumber(g_gateAttackEdit, settings.gate.attackMs);
    SetNumber(g_gateHoldEdit, settings.gate.holdMs);
    SetNumber(g_gateReleaseEdit, settings.gate.releaseMs);
    SetNumber(g_gateLookaheadEdit, settings.gate.lookaheadMs);
    SetNumber(g_compThresholdEdit, settings.compressor.thresholdDb);
    SetNumber(g_compRatioEdit, settings.compressor.ratio);
    SetNumber(g_compAttackEdit, settings.compressor.attackMs);
    SetNumber(g_compReleaseEdit, settings.compressor.releaseMs);
    SetNumber(g_makeupEdit, settings.makeupDb);
    SetNumber(g_dynFrequencyEdit, settings.dynamics.frequencyHz);
    SetNumber(g_dynQEdit, settings.dynamics.q);
    SetNumber(g_dynThresholdEdit, settings.dynamics.thresholdDb);
    SetNumber(g_dynRatioEdit, settings.dynamics.ratio);
    SetNumber(g_dynRangeEdit, settings.dynamics.rangeDb);
    SetNumber(g_dynAttackEdit, settings.dynamics.attackMs);
    SetNumber(g_dynReleaseEdit, settings.dynamics.releaseMs);
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
    if (!GetNumber(g_noiseReductionEdit, settings.noise.reductionDb) ||
        !GetNumber(g_gateThresholdEdit, settings.gate.thresholdDb) ||
        !GetNumber(g_gateRangeEdit, settings.gate.rangeDb) ||
        !GetNumber(g_gateAttackEdit, settings.gate.attackMs) ||
        !GetNumber(g_gateHoldEdit, settings.gate.holdMs) ||
        !GetNumber(g_gateReleaseEdit, settings.gate.releaseMs) ||
        !GetNumber(g_gateLookaheadEdit, settings.gate.lookaheadMs) ||
        !GetNumber(g_compThresholdEdit,
                        settings.compressor.thresholdDb) ||
        !GetNumber(g_compRatioEdit, settings.compressor.ratio) ||
        !GetNumber(g_compAttackEdit, settings.compressor.attackMs) ||
        !GetNumber(g_compReleaseEdit, settings.compressor.releaseMs) ||
        !GetNumber(g_makeupEdit, settings.makeupDb) ||
        !GetNumber(g_dynFrequencyEdit, settings.dynamics.frequencyHz) ||
        !GetNumber(g_dynQEdit, settings.dynamics.q) ||
        !GetNumber(g_dynThresholdEdit,
                        settings.dynamics.thresholdDb) ||
        !GetNumber(g_dynRatioEdit, settings.dynamics.ratio) ||
        !GetNumber(g_dynRangeEdit, settings.dynamics.rangeDb) ||
        !GetNumber(g_dynAttackEdit, settings.dynamics.attackMs) ||
        !GetNumber(g_dynReleaseEdit, settings.dynamics.releaseMs)) {
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
    SetNumber(g_noiseReductionEdit, settings.noise.reductionDb);
    SetNumber(g_gateThresholdEdit, settings.gate.thresholdDb);
    SetNumber(g_gateRangeEdit, settings.gate.rangeDb);
    SetNumber(g_gateAttackEdit, settings.gate.attackMs);
    SetNumber(g_gateHoldEdit, settings.gate.holdMs);
    SetNumber(g_gateReleaseEdit, settings.gate.releaseMs);
    SetNumber(g_gateLookaheadEdit, settings.gate.lookaheadMs);
    SetNumber(g_compThresholdEdit, settings.compressor.thresholdDb);
    SetNumber(g_compRatioEdit, settings.compressor.ratio);
    SetNumber(g_compAttackEdit, settings.compressor.attackMs);
    SetNumber(g_compReleaseEdit, settings.compressor.releaseMs);
    SetNumber(g_makeupEdit, settings.makeupDb);
    SetNumber(g_dynFrequencyEdit, settings.dynamics.frequencyHz);
    SetNumber(g_dynQEdit, settings.dynamics.q);
    SetNumber(g_dynThresholdEdit, settings.dynamics.thresholdDb);
    SetNumber(g_dynRatioEdit, settings.dynamics.ratio);
    SetNumber(g_dynRangeEdit, settings.dynamics.rangeDb);
    SetNumber(g_dynAttackEdit, settings.dynamics.attackMs);
    SetNumber(g_dynReleaseEdit, settings.dynamics.releaseMs);
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

    EnableWindow(g_list, available);
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
    // Keep the list enabled while busy: the native disabled state repaints the
    // whole control light. Interaction is rejected in the notification
    // handler instead.
    EnableWindow(g_effectList, available && hasDevice);
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

// Store each tab's availability in its item parameter. The custom dark-mode
// tab painter reads it to gray out unreachable tabs, and the notification
// handler uses it to refuse selection changes.
void UpdateTabAvailability()
{
    if (g_tabs == nullptr) {
        return;
    }
    const int count = TabCtrl_GetItemCount(g_tabs);
    for (int i = 0; i < count; ++i) {
        TCITEMW item{};
        item.mask = TCIF_PARAM;
        item.lParam = TabAvailable(i) ? 1 : 0;
        TabCtrl_SetItem(g_tabs, i, &item);
    }
    InvalidateRect(g_tabs, nullptr, TRUE);
}

void UpdateViewAvailability()
{
    if (g_view == View::Effects && !EffectsAvailable()) {
        SetActiveView(DevicesAvailable() ? View::Devices : View::Install);
    } else if (g_view == View::Devices && !DevicesAvailable()) {
        SetActiveView(View::Install);
    }
    UpdateTabAvailability();
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
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
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
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    g_loading = false;
}

void RebuildEffectCombo(const std::wstring& selectedId)
{
    g_comboLoading = true;
    SendMessageW(g_effectDeviceCombo, WM_SETREDRAW, FALSE, 0);
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
    SendMessageW(g_effectDeviceCombo, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_effectDeviceCombo, nullptr, TRUE);
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
    UpdateTabAvailability();
    RepaintAll();
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
    RepaintAll();
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
                    draw->clrText = ui::GetTheme().color.disabledText;
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
        switch (header->code) {
        case LVN_ITEMCHANGING: {
            auto* item = reinterpret_cast<NMLISTVIEW*>(lParam);
            if (g_busy && (item->uChanged & LVIF_STATE) != 0) {
                return TRUE;
            }
            break;
        }
        case LVN_ITEMCHANGED:
            OnEffectListChanged(*reinterpret_cast<NMLISTVIEW*>(lParam));
            break;
        default:
            break;
        }
        return 0;
    }

    if (header->hwndFrom == g_list) {
        switch (header->code) {
        case LVN_ITEMCHANGING: {
            auto* item = reinterpret_cast<NMLISTVIEW*>(lParam);
            if (g_busy && (item->uChanged & LVIF_STATE) != 0) {
                return TRUE;
            }
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
                    draw->clrText = ui::GetTheme().color.disabledText;
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


LRESULT HandleStaticColorMessage(WPARAM wParam, LPARAM lParam)
{
    const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
    const bool error =
        id == IDC_EFFECTS_UNSAVED || id == IDC_DEVICES_UNSAVED;
    return ui::HandleStaticColor(reinterpret_cast<HDC>(wParam), error);
}

// The page panel is the parent of every view control, so it receives their
// WM_CTLCOLOR messages. Handle them here instead of in WindowProc.
LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM wParam,
                          LPARAM lParam)
{
    switch (message) {
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect,
                 ui::GetTheme().backgroundBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(dc, &rect, ui::GetTheme().backgroundBrush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        return HandleStaticColorMessage(wParam, lParam);
    case WM_CTLCOLORBTN:
        return ui::HandleButtonColor(reinterpret_cast<HDC>(wParam));
    case WM_CTLCOLOREDIT:
        return ui::HandleEditColor(reinterpret_cast<HDC>(wParam));
    case WM_CTLCOLORLISTBOX:
        return ui::HandleListBoxColor(reinterpret_cast<HDC>(wParam));
    default:
        break;
    }

    // The page panel owns the view controls, so their command and
    // notification messages arrive here. The main window proc still handles
    // them.
    switch (message) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_COMPAREITEM:
        return SendMessageW(GetParent(window), message, wParam, lParam);
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        ui::CreateFonts();
        CreateControls(window);
        ui::ApplyNativeThemeToChildren(window);
        ui::ApplyListStateImages(g_list);
        ui::ApplyListStateImages(g_effectList);
        UpdateViewVisibility();
        Refresh();
        SetActiveView(DefaultView());
        RepaintAll();
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect,
                 ui::GetTheme().backgroundBrush);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(dc, &rect, ui::GetTheme().backgroundBrush);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_SIZE:
        if (g_list) {
            Layout(LOWORD(lParam), HIWORD(lParam));
            RepaintAll();
        }
        break;

    case WM_SETTINGCHANGE:
        if (wParam == 0 && lParam != 0 &&
            wcscmp(reinterpret_cast<const wchar_t*>(lParam),
                   L"ImmersiveColorSet") == 0) {
            ui::RefreshTheme();
            ui::ApplyTitleBarTheme(window);
            ui::ApplyNativeThemeToChildren(window);
            ui::ApplyListStateImages(g_list);
            ui::ApplyListStateImages(g_effectList);
            RepaintAll();
        }
        break;

    case WM_THEMECHANGED:
        ui::RefreshTheme();
        ui::ApplyTitleBarTheme(window);
        ui::ApplyNativeThemeToChildren(window);
        ui::ApplyListStateImages(g_list);
        ui::ApplyListStateImages(g_effectList);
        RepaintAll();
        break;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(940);
        info->ptMinTrackSize.y = Scale(720);
        break;
    }

    case WM_CTLCOLORSTATIC:
        return HandleStaticColorMessage(wParam, lParam);

    case WM_CTLCOLORBTN:
        return ui::HandleButtonColor(reinterpret_cast<HDC>(wParam));

    case WM_CTLCOLOREDIT:
        return ui::HandleEditColor(reinterpret_cast<HDC>(wParam));

    case WM_CTLCOLORLISTBOX:
        return ui::HandleListBoxColor(reinterpret_cast<HDC>(wParam));

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
            } else if (HIWORD(wParam) == CBN_CLOSEUP) {
                RepaintAll();
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
        ui::DestroyFonts();
        ui::ShutdownTheme();
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

    // Read the system app theme before any window is created so the first
    // paint already uses the right palette and native control theme.
    ui::InitializeTheme();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    // The theme brushes are recreated when the system theme changes, so the
    // classes leave the background to WM_ERASEBKGND instead of caching one.
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        ui::ShutdownTheme();
        return 1;
    }

    WNDCLASSEXW pageClass{};
    pageClass.cbSize = sizeof(pageClass);
    pageClass.lpfnWndProc = PageProc;
    pageClass.hInstance = instance;
    pageClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pageClass.lpszClassName = kPageClass;
    if (!RegisterClassExW(&pageClass)) {
        ui::ShutdownTheme();
        return 1;
    }

    HWND window = CreateWindowExW(
        0, windowClass.lpszClassName, L"Better Micro APO",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        Scale(1040), Scale(820), nullptr, nullptr, instance, nullptr);
    if (!window) {
        ui::ShutdownTheme();
        return 1;
    }

    ui::ApplyTitleBarTheme(window);

    ShowWindow(window, show);
    UpdateWindow(window);
    Relayout();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

