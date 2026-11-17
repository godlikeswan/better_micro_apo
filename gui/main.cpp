#include "apo.h"

#include "apo_internal.h"
#include "apo_monitor.h"
#include "preferences.h"
#include "runtime_settings.h"

#include "ui/effect_dialog.h"
#include "ui/graph.h"
#include "ui/info_dialog.h"
#include "ui/menu.h"
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
using ui::GetSliderValue;
using ui::Scale;
using ui::SetSliderValue;

constexpr UINT WM_APP_WORK_DONE = WM_APP + 1;
constexpr UINT_PTR kMonitorTimer = 1;
constexpr UINT_PTR kLiveApplyTimer = 2;
constexpr UINT kLiveApplyDelayMs = 200;
constexpr DWORD kMeterWindowMs = 500;

constexpr wchar_t kWindowClass[] = L"BetterMicroApoInstaller";
constexpr wchar_t kInstanceMutex[] = L"Local\\better_micro_apo_gui_instance";


DWORD g_clipHoldUntil = 0;
unsigned g_dropTotal = 0;
bool g_liveApplyPending = false;

struct MeterEntry {
    DWORD tick = 0;
    double peak = 0.0;
    double sumSquares = 0.0;
    unsigned long long samples = 0;
};

std::deque<MeterEntry> g_meterHistory;
std::wstring g_meterText;

bool g_busy = false;
bool g_effectLoading = false;   // Guard for the effect list/editor.
bool g_comboLoading = false;    // Guard for the device combo.

// Persisted View menu choice. Off means the selector lists active capture
// devices only; on adds inactive endpoints with a state marker.
bool g_showAllDevices = false;

std::vector<apo::Endpoint> g_endpoints;
apo::FilesStatus g_files;
std::thread g_worker;
apo::Result g_result;

int g_effectIndex = -1;   // Selected endpoint in the device combo.

constexpr unsigned kNoiseCaptureMs = 3000;


enum class WorkKind {
    Generic,
    CaptureNoise,
};

WorkKind g_workKind = WorkKind::Generic;
int g_captureIndex = -1;
apo::ApoSettings g_captureSettings;
bool g_captureSucceeded = false;

// Apply briefly stops the audio service, which also stops the monitor stream.
// Remember the listening state so it can be restarted once Apply finishes.
bool g_resumeListen = false;
std::wstring g_resumeListenId;

// Install/Uninstall refreshes the endpoint list. Keep unsaved effect edits so
// the refresh does not silently discard them.
bool g_preserveDirtyEffects = false;

// Forward declarations for helpers that are used before their definitions.
void UpdateMainUi();
void UpdateEffectsUi();
void UpdateParamResetButtons();
void MarkSettingsDirty();
void RefreshEffectGraph();
bool ValidEffectIndex();
void DoEnable(bool enable);
void UpdateChromeUi();

int CountEffectType(const apo::ApoSettings& settings, apo::EffectType type)
{
    const apo::EffectPreset& preset = apo::ActivePreset(settings);
    int count = 0;
    for (int i = 0; i < preset.effectCount; ++i) {
        if (preset.effects[i].type == type) {
            ++count;
        }
    }
    return count;
}

bool HasEffectSelection()
{
    return ValidEffectIndex() && g_effectSelection >= 0 &&
           g_effectSelection <
               apo::ActivePreset(g_endpoints[g_effectIndex].settings)
                   .effectCount;
}

// ---------------------------------------------------------------------------
// Device and dirty-state helpers
// ---------------------------------------------------------------------------

bool EndpointVisible(const apo::Endpoint& endpoint)
{
    return g_showAllDevices || apo::IsDeviceActive(endpoint.state);
}

const wchar_t* SelectorMarker(DWORD state)
{
    if ((state & 2) != 0) {
        return L"[disabled]";
    }
    if ((state & 8) != 0) {
        return L"[unplugged]";
    }
    if ((state & 4) != 0) {
        return L"[not present]";
    }
    return nullptr;
}

std::wstring SelectorLabel(const apo::Endpoint& endpoint)
{
    if (!g_showAllDevices || apo::IsDeviceActive(endpoint.state)) {
        return endpoint.name;
    }
    const wchar_t* marker = SelectorMarker(endpoint.state);
    return marker != nullptr ? endpoint.name + L" " + marker : endpoint.name;
}

int BoundDeviceCount()
{
    return static_cast<int>(std::count_if(
        g_endpoints.begin(), g_endpoints.end(),
        [](const apo::Endpoint& endpoint) { return endpoint.Enabled(); }));
}

bool EffectsDirty()
{
    return std::any_of(g_endpoints.begin(), g_endpoints.end(),
                       [](const apo::Endpoint& endpoint) {
                           return endpoint.settingsDirty;
                       });
}

bool ValidEffectIndex()
{
    if (g_effectIndex < 0 ||
        g_effectIndex >= static_cast<int>(g_endpoints.size())) {
        return false;
    }
    return EndpointVisible(g_endpoints[g_effectIndex]);
}

// ---------------------------------------------------------------------------
// Effect editor
// ---------------------------------------------------------------------------

void ShowEffectControls(apo::EffectType type, bool show)
{
    auto showAll = [show](std::initializer_list<HWND> controls) {
        for (HWND control : controls) {
            if (control != nullptr) {
                ShowWindow(control, show ? SW_SHOW : SW_HIDE);
            }
        }
    };

    switch (type) {
    case apo::EffectType::Noise:
        showAll({g_noiseReductionLabel, g_noiseReductionSlider, g_noiseCapture,
                 g_noiseNote});
        break;
    case apo::EffectType::Gate:
        showAll({g_gateThresholdLabel, g_gateThresholdSlider, g_gateRangeLabel,
                 g_gateRangeSlider, g_gateAttackLabel, g_gateAttackSlider,
                 g_gateHoldLabel, g_gateHoldSlider, g_gateReleaseLabel,
                 g_gateReleaseSlider, g_gateLookaheadLabel,
                 g_gateLookaheadSlider});
        break;
    case apo::EffectType::Dynamic:
        showAll({g_dynamicExtend, g_dynFrequencyLabel, g_dynFrequencySlider,
                 g_dynQLabel, g_dynQSlider, g_dynThresholdLabel,
                 g_dynThresholdSlider, g_dynRatioLabel, g_dynRatioSlider,
                 g_dynRangeLabel, g_dynRangeSlider, g_dynAttackLabel,
                 g_dynAttackSlider, g_dynReleaseLabel, g_dynReleaseSlider});
        break;
    case apo::EffectType::Compressor:
        showAll({g_compThresholdLabel, g_compThresholdSlider, g_compRatioLabel,
                 g_compRatioSlider, g_compAttackLabel, g_compAttackSlider,
                 g_compReleaseLabel, g_compReleaseSlider});
        break;
    case apo::EffectType::Gain:
        showAll({g_makeupLabel, g_makeupSlider});
        break;
    case apo::EffectType::Mono:
        showAll({g_monoNote});
        break;
    case apo::EffectType::Distortion:
        showAll({g_distFunctionLabel, g_distFunctionCombo,
                 g_distDriveLabel, g_distDriveSlider,
                 g_distCurvatureLabel, g_distCurvatureSlider,
                 g_distOutputLabel, g_distOutputSlider, g_distortionNote});
        break;
    case apo::EffectType::Reverb:
        showAll({g_reverbMixLabel, g_reverbMixSlider, g_reverbSizeLabel,
                 g_reverbSizeSlider, g_reverbDampingLabel,
                 g_reverbDampingSlider, g_reverbPredelayLabel,
                 g_reverbPredelaySlider});
        break;
    default:
        break;
    }
}

void EnableEffectControls(bool enabled)
{
    const HWND controls[] = {
        g_gateThresholdLabel, g_gateThresholdSlider,
        g_gateRangeLabel, g_gateRangeSlider,
        g_gateAttackLabel, g_gateAttackSlider,
        g_gateHoldLabel, g_gateHoldSlider,
        g_gateReleaseLabel, g_gateReleaseSlider,
        g_gateLookaheadLabel, g_gateLookaheadSlider,
        g_compThresholdLabel, g_compThresholdSlider,
        g_compRatioLabel, g_compRatioSlider,
        g_compAttackLabel, g_compAttackSlider,
        g_compReleaseLabel, g_compReleaseSlider,
        g_makeupLabel, g_makeupSlider,
        g_dynamicExtend,
        g_dynFrequencyLabel, g_dynFrequencySlider,
        g_dynQLabel, g_dynQSlider,
        g_dynThresholdLabel, g_dynThresholdSlider,
        g_dynRatioLabel, g_dynRatioSlider,
        g_dynRangeLabel, g_dynRangeSlider,
        g_dynAttackLabel, g_dynAttackSlider,
        g_dynReleaseLabel, g_dynReleaseSlider,
        g_distFunctionLabel, g_distFunctionCombo,
        g_distDriveLabel, g_distDriveSlider,
        g_distCurvatureLabel, g_distCurvatureSlider,
        g_distOutputLabel, g_distOutputSlider,
        g_reverbMixLabel, g_reverbMixSlider,
        g_reverbSizeLabel, g_reverbSizeSlider,
        g_reverbDampingLabel, g_reverbDampingSlider,
        g_reverbPredelayLabel, g_reverbPredelaySlider,
        g_noiseReductionLabel, g_noiseReductionSlider, g_noiseCapture,
        g_effectGraph,
    };
    for (HWND control : controls) {
        EnableWindow(control, enabled);
    }
}

void ClearEffectEditor()
{
    g_effectSelection = -1;
    g_effectLoading = true;
    // Sliders always hold a value, so there is nothing to blank out. Hide the
    // parameter controls instead; ShowEffectForSelected brings them back.
    for (int i = 0; i < static_cast<int>(apo::EffectType::Count); ++i) {
        ShowEffectControls(static_cast<apo::EffectType>(i), false);
    }
    SendMessageW(g_dynamicExtend, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(g_distFunctionCombo, CB_SETCURSEL, apo::kDistortionTanh, 0);
    SetWindowTextW(g_effectHeading, L"Select an effect");
    SetWindowTextW(g_noiseNote, L"");
    ShowWindow(g_effectGraph, SW_HIDE);
    g_effectLoading = false;
    RefreshEffectGraph();
    Relayout();
    UpdateParamResetButtons();
}

// Rebuilds the list rows from the selected endpoint's effect array. Called when
// the set of effects changes, not from inside a list notification.
void RebuildEffectList()
{
    int count = 0;
    if (ValidEffectIndex()) {
        count =
            apo::ActivePreset(g_endpoints[g_effectIndex].settings).effectCount;
    }

    if (count == 0) {
        g_effectSelection = -1;
    } else if (g_effectSelection < 0) {
        g_effectSelection = 0;
    } else if (g_effectSelection >= count) {
        g_effectSelection = count - 1;
    }

    g_effectLoading = true;
    SendMessageW(g_effectList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_effectList);

    if (count > 0) {
        const apo::EffectPreset& preset =
            apo::ActivePreset(g_endpoints[g_effectIndex].settings);
        for (int i = 0; i < count; ++i) {
            const apo::EffectInstance& effect = preset.effects[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText =
                const_cast<wchar_t*>(apo::EffectTypeName(effect.type));
            const int row = ListView_InsertItem(g_effectList, &item);
            ListView_SetItemText(g_effectList, row, 1,
                                 const_cast<wchar_t*>(L"\u00D7"));
            ListView_SetCheckState(g_effectList, row,
                                   apo::EffectEnabled(effect) ? TRUE : FALSE);
        }
    }

    if (g_effectSelection >= 0) {
        ListView_SetItemState(g_effectList, g_effectSelection,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_effectList, g_effectSelection, FALSE);
        g_effectType =
            apo::ActivePreset(g_endpoints[g_effectIndex].settings)
                .effects[g_effectSelection]
                .type;
    }

    SendMessageW(g_effectList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_effectList, nullptr, TRUE);
    g_effectLoading = false;
}

// Copies the editor controls into `effect`. The sliders always hold valid
// values, but ClampSettings still runs after this so a programmatic value
// cannot escape the supported range.
void ReadEffectControls(apo::EffectInstance& effect)
{
    switch (effect.type) {
    case apo::EffectType::Noise:
        GetSliderValue(g_noiseReductionSlider, effect.noise.reductionDb);
        break;
    case apo::EffectType::Gate:
        GetSliderValue(g_gateThresholdSlider, effect.gate.thresholdDb);
        GetSliderValue(g_gateRangeSlider, effect.gate.rangeDb);
        GetSliderValue(g_gateAttackSlider, effect.gate.attackMs);
        GetSliderValue(g_gateHoldSlider, effect.gate.holdMs);
        GetSliderValue(g_gateReleaseSlider, effect.gate.releaseMs);
        GetSliderValue(g_gateLookaheadSlider, effect.gate.lookaheadMs);
        break;
    case apo::EffectType::Dynamic:
        GetSliderValue(g_dynFrequencySlider, effect.dynamics.frequencyHz);
        GetSliderValue(g_dynQSlider, effect.dynamics.q);
        GetSliderValue(g_dynThresholdSlider, effect.dynamics.thresholdDb);
        GetSliderValue(g_dynRatioSlider, effect.dynamics.ratio);
        GetSliderValue(g_dynRangeSlider, effect.dynamics.rangeDb);
        GetSliderValue(g_dynAttackSlider, effect.dynamics.attackMs);
        GetSliderValue(g_dynReleaseSlider, effect.dynamics.releaseMs);
        effect.dynamics.expand =
            SendMessageW(g_dynamicExtend, BM_GETCHECK, 0, 0) == BST_CHECKED;
        break;
    case apo::EffectType::Compressor:
        GetSliderValue(g_compThresholdSlider, effect.compressor.thresholdDb);
        GetSliderValue(g_compRatioSlider, effect.compressor.ratio);
        GetSliderValue(g_compAttackSlider, effect.compressor.attackMs);
        GetSliderValue(g_compReleaseSlider, effect.compressor.releaseMs);
        break;
    case apo::EffectType::Gain:
        GetSliderValue(g_makeupSlider, effect.gain.gainDb);
        break;
    case apo::EffectType::Mono:
        break;
    case apo::EffectType::Distortion: {
        const int function = static_cast<int>(
            SendMessageW(g_distFunctionCombo, CB_GETCURSEL, 0, 0));
        if (function >= 0 && function < apo::kDistortionFunctionCount) {
            effect.distortion.function = function;
        }
        GetSliderValue(g_distDriveSlider, effect.distortion.driveDb);
        GetSliderValue(g_distCurvatureSlider, effect.distortion.curvature);
        GetSliderValue(g_distOutputSlider, effect.distortion.outputDb);
        break;
    }
    case apo::EffectType::Reverb:
        GetSliderValue(g_reverbMixSlider, effect.reverb.mix);
        GetSliderValue(g_reverbSizeSlider, effect.reverb.size);
        GetSliderValue(g_reverbDampingSlider, effect.reverb.damping);
        GetSliderValue(g_reverbPredelaySlider, effect.reverb.predelayMs);
        break;
    default:
        break;
    }
}

// Redraws the curve from the values currently in the editor. Called on every
// change, including while a slider is still being dragged, so the graph always
// shows what the next apply will send to the APO.
void RefreshEffectGraph()
{
    if (g_effectGraph == nullptr) {
        return;
    }

    apo::EffectInstance effect;
    if (HasEffectSelection()) {
        effect = apo::ActivePreset(g_endpoints[g_effectIndex].settings)
                     .effects[g_effectSelection];
        ReadEffectControls(effect);
    }
    ui::SetGraphEffect(g_effectGraph, effect);
}

void ShowEffectForSelected()
{
    if (ValidEffectIndex()) {
        SendMessageW(g_effectPresetCombo, CB_SETCURSEL,
                     g_endpoints[g_effectIndex].settings.presetIndex, 0);
    }

    if (!HasEffectSelection()) {
        ClearEffectEditor();
        return;
    }

    const apo::EffectInstance& effect =
        apo::ActivePreset(g_endpoints[g_effectIndex].settings)
            .effects[g_effectSelection];
    g_effectType = effect.type;

    g_effectLoading = true;
    SetSliderValue(g_noiseReductionSlider, effect.noise.reductionDb);
    SetSliderValue(g_gateThresholdSlider, effect.gate.thresholdDb);
    SetSliderValue(g_gateRangeSlider, effect.gate.rangeDb);
    SetSliderValue(g_gateAttackSlider, effect.gate.attackMs);
    SetSliderValue(g_gateHoldSlider, effect.gate.holdMs);
    SetSliderValue(g_gateReleaseSlider, effect.gate.releaseMs);
    SetSliderValue(g_gateLookaheadSlider, effect.gate.lookaheadMs);
    SetSliderValue(g_compThresholdSlider, effect.compressor.thresholdDb);
    SetSliderValue(g_compRatioSlider, effect.compressor.ratio);
    SetSliderValue(g_compAttackSlider, effect.compressor.attackMs);
    SetSliderValue(g_compReleaseSlider, effect.compressor.releaseMs);
    SetSliderValue(g_makeupSlider, effect.gain.gainDb);
    SetSliderValue(g_dynFrequencySlider, effect.dynamics.frequencyHz);
    SetSliderValue(g_dynQSlider, effect.dynamics.q);
    SetSliderValue(g_dynThresholdSlider, effect.dynamics.thresholdDb);
    SetSliderValue(g_dynRatioSlider, effect.dynamics.ratio);
    SetSliderValue(g_dynRangeSlider, effect.dynamics.rangeDb);
    SetSliderValue(g_dynAttackSlider, effect.dynamics.attackMs);
    SetSliderValue(g_dynReleaseSlider, effect.dynamics.releaseMs);
    SetSliderValue(g_reverbMixSlider, effect.reverb.mix);
    SetSliderValue(g_reverbSizeSlider, effect.reverb.size);
    SetSliderValue(g_reverbDampingSlider, effect.reverb.damping);
    SetSliderValue(g_reverbPredelaySlider, effect.reverb.predelayMs);
    SendMessageW(g_distFunctionCombo, CB_SETCURSEL,
                 effect.distortion.function, 0);
    SetSliderValue(g_distDriveSlider, effect.distortion.driveDb);
    SetSliderValue(g_distCurvatureSlider, effect.distortion.curvature);
    SetSliderValue(g_distOutputSlider, effect.distortion.outputDb);
    SendMessageW(g_dynamicExtend, BM_SETCHECK,
                 effect.dynamics.expand ? BST_CHECKED : BST_UNCHECKED, 0);

    SetWindowTextW(g_effectHeading, apo::EffectTypeName(effect.type));
    // The parameter panel is shared: hide every type first so switching to an
    // effect of another type does not leave the previous controls on screen.
    for (int i = 0; i < static_cast<int>(apo::EffectType::Count); ++i) {
        ShowEffectControls(static_cast<apo::EffectType>(i), false);
    }
    ShowEffectControls(effect.type, true);

    ShowWindow(g_effectGraph,
               ui::GraphSupported(effect.type) ? SW_SHOW : SW_HIDE);

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
    RefreshEffectGraph();
    Relayout();
    UpdateParamResetButtons();
}

bool CommitEffectEditor(bool showValues = true)
{
    if (!HasEffectSelection()) {
        return true;
    }

    apo::ApoSettings settings = g_endpoints[g_effectIndex].settings;
    apo::EffectInstance& effect =
        apo::ActivePreset(settings).effects[g_effectSelection];

    // Sliders always hold a valid, in-range value, so there is nothing to
    // reject here. The settings are clamped again below for safety.
    ReadEffectControls(effect);

    apo::ClampSettings(settings);
    g_endpoints[g_effectIndex].settings = settings;

    // Show the clamped values so the user sees what will be saved. The silent
    // auto-apply path leaves the controls alone so it cannot move a slider
    // while the user is still dragging it.
    if (showValues) {
        const apo::EffectInstance& shown =
            apo::ActivePreset(settings).effects[g_effectSelection];
        g_effectLoading = true;
        switch (shown.type) {
        case apo::EffectType::Noise:
            SetSliderValue(g_noiseReductionSlider, shown.noise.reductionDb);
            break;
        case apo::EffectType::Gate:
            SetSliderValue(g_gateThresholdSlider, shown.gate.thresholdDb);
            SetSliderValue(g_gateRangeSlider, shown.gate.rangeDb);
            SetSliderValue(g_gateAttackSlider, shown.gate.attackMs);
            SetSliderValue(g_gateHoldSlider, shown.gate.holdMs);
            SetSliderValue(g_gateReleaseSlider, shown.gate.releaseMs);
            SetSliderValue(g_gateLookaheadSlider, shown.gate.lookaheadMs);
            break;
        case apo::EffectType::Dynamic:
            SetSliderValue(g_dynFrequencySlider, shown.dynamics.frequencyHz);
            SetSliderValue(g_dynQSlider, shown.dynamics.q);
            SetSliderValue(g_dynThresholdSlider, shown.dynamics.thresholdDb);
            SetSliderValue(g_dynRatioSlider, shown.dynamics.ratio);
            SetSliderValue(g_dynRangeSlider, shown.dynamics.rangeDb);
            SetSliderValue(g_dynAttackSlider, shown.dynamics.attackMs);
            SetSliderValue(g_dynReleaseSlider, shown.dynamics.releaseMs);
            SendMessageW(g_dynamicExtend, BM_SETCHECK,
                         shown.dynamics.expand ? BST_CHECKED : BST_UNCHECKED,
                         0);
            break;
        case apo::EffectType::Compressor:
            SetSliderValue(g_compThresholdSlider,
                           shown.compressor.thresholdDb);
            SetSliderValue(g_compRatioSlider, shown.compressor.ratio);
            SetSliderValue(g_compAttackSlider, shown.compressor.attackMs);
            SetSliderValue(g_compReleaseSlider, shown.compressor.releaseMs);
            break;
        case apo::EffectType::Gain:
            SetSliderValue(g_makeupSlider, shown.gain.gainDb);
            break;
        case apo::EffectType::Distortion:
            SendMessageW(g_distFunctionCombo, CB_SETCURSEL,
                         shown.distortion.function, 0);
            SetSliderValue(g_distDriveSlider, shown.distortion.driveDb);
            SetSliderValue(g_distCurvatureSlider, shown.distortion.curvature);
            SetSliderValue(g_distOutputSlider, shown.distortion.outputDb);
            break;
        case apo::EffectType::Reverb:
            SetSliderValue(g_reverbMixSlider, shown.reverb.mix);
            SetSliderValue(g_reverbSizeSlider, shown.reverb.size);
            SetSliderValue(g_reverbDampingSlider, shown.reverb.damping);
            SetSliderValue(g_reverbPredelaySlider, shown.reverb.predelayMs);
            break;
        default:
            break;
        }
        g_effectLoading = false;
    }
    RefreshEffectGraph();
    return true;
}

namespace {

bool SameValue(double left, double right)
{
    return std::abs(left - right) < 1e-6;
}

bool ParameterAtDefault(const apo::EffectInstance& effect,
                        const apo::EffectInstance& defaults, int param)
{
    switch (param) {
    case ParamResetNoiseReduction:
        return SameValue(effect.noise.reductionDb, defaults.noise.reductionDb);
    case ParamResetGateThreshold:
        return SameValue(effect.gate.thresholdDb, defaults.gate.thresholdDb);
    case ParamResetGateRange:
        return SameValue(effect.gate.rangeDb, defaults.gate.rangeDb);
    case ParamResetGateAttack:
        return SameValue(effect.gate.attackMs, defaults.gate.attackMs);
    case ParamResetGateHold:
        return SameValue(effect.gate.holdMs, defaults.gate.holdMs);
    case ParamResetGateRelease:
        return SameValue(effect.gate.releaseMs, defaults.gate.releaseMs);
    case ParamResetGateLookahead:
        return SameValue(effect.gate.lookaheadMs, defaults.gate.lookaheadMs);
    case ParamResetCompThreshold:
        return SameValue(effect.compressor.thresholdDb,
                         defaults.compressor.thresholdDb);
    case ParamResetCompRatio:
        return SameValue(effect.compressor.ratio, defaults.compressor.ratio);
    case ParamResetCompAttack:
        return SameValue(effect.compressor.attackMs,
                         defaults.compressor.attackMs);
    case ParamResetCompRelease:
        return SameValue(effect.compressor.releaseMs,
                         defaults.compressor.releaseMs);
    case ParamResetMakeup:
        return SameValue(effect.gain.gainDb, defaults.gain.gainDb);
    case ParamResetDynExpand:
        return effect.dynamics.expand == defaults.dynamics.expand;
    case ParamResetDynFrequency:
        return SameValue(effect.dynamics.frequencyHz,
                         defaults.dynamics.frequencyHz);
    case ParamResetDynQ:
        return SameValue(effect.dynamics.q, defaults.dynamics.q);
    case ParamResetDynThreshold:
        return SameValue(effect.dynamics.thresholdDb,
                         defaults.dynamics.thresholdDb);
    case ParamResetDynRatio:
        return SameValue(effect.dynamics.ratio, defaults.dynamics.ratio);
    case ParamResetDynRange:
        return SameValue(effect.dynamics.rangeDb, defaults.dynamics.rangeDb);
    case ParamResetDynAttack:
        return SameValue(effect.dynamics.attackMs, defaults.dynamics.attackMs);
    case ParamResetDynRelease:
        return SameValue(effect.dynamics.releaseMs, defaults.dynamics.releaseMs);
    case ParamResetReverbMix:
        return SameValue(effect.reverb.mix, defaults.reverb.mix);
    case ParamResetReverbSize:
        return SameValue(effect.reverb.size, defaults.reverb.size);
    case ParamResetReverbDamping:
        return SameValue(effect.reverb.damping, defaults.reverb.damping);
    case ParamResetReverbPredelay:
        return SameValue(effect.reverb.predelayMs, defaults.reverb.predelayMs);
    case ParamResetDistFunction:
        return effect.distortion.function == defaults.distortion.function;
    case ParamResetDistDrive:
        return SameValue(effect.distortion.driveDb, defaults.distortion.driveDb);
    case ParamResetDistCurvature:
        return SameValue(effect.distortion.curvature,
                         defaults.distortion.curvature);
    case ParamResetDistOutput:
        return SameValue(effect.distortion.outputDb, defaults.distortion.outputDb);
    default:
        return true;
    }
}

} // namespace

void UpdateParamResetButtons()
{
    if (g_busy || g_effectLoading || !HasEffectSelection() ||
        !CommitEffectEditor(false)) {
        for (HWND button : g_paramResetButtons) {
            EnableWindow(button, FALSE);
        }
        return;
    }

    const apo::EffectInstance effect =
        apo::ActivePreset(g_endpoints[g_effectIndex].settings)
            .effects[g_effectSelection];
    const apo::EffectInstance defaults =
        apo::MakeEffect(effect.type, effect.id);
    for (int param = 0; param < ParamResetCount; ++param) {
        EnableWindow(g_paramResetButtons[param],
                     !ParameterAtDefault(effect, defaults, param));
    }
}

void ResetParameter(int param)
{
    if (g_busy || !HasEffectSelection() || !CommitEffectEditor(false)) {
        return;
    }

    apo::Endpoint& endpoint = g_endpoints[g_effectIndex];
    apo::ApoSettings settings = endpoint.settings;
    apo::EffectInstance& effect =
        apo::ActivePreset(settings).effects[g_effectSelection];
    const apo::EffectInstance defaults =
        apo::MakeEffect(effect.type, effect.id);

    switch (param) {
    case ParamResetNoiseReduction:
        effect.noise.reductionDb = defaults.noise.reductionDb;
        break;
    case ParamResetGateThreshold:
        effect.gate.thresholdDb = defaults.gate.thresholdDb;
        break;
    case ParamResetGateRange:
        effect.gate.rangeDb = defaults.gate.rangeDb;
        break;
    case ParamResetGateAttack:
        effect.gate.attackMs = defaults.gate.attackMs;
        break;
    case ParamResetGateHold:
        effect.gate.holdMs = defaults.gate.holdMs;
        break;
    case ParamResetGateRelease:
        effect.gate.releaseMs = defaults.gate.releaseMs;
        break;
    case ParamResetGateLookahead:
        effect.gate.lookaheadMs = defaults.gate.lookaheadMs;
        break;
    case ParamResetCompThreshold:
        effect.compressor.thresholdDb = defaults.compressor.thresholdDb;
        break;
    case ParamResetCompRatio:
        effect.compressor.ratio = defaults.compressor.ratio;
        break;
    case ParamResetCompAttack:
        effect.compressor.attackMs = defaults.compressor.attackMs;
        break;
    case ParamResetCompRelease:
        effect.compressor.releaseMs = defaults.compressor.releaseMs;
        break;
    case ParamResetMakeup:
        effect.gain.gainDb = defaults.gain.gainDb;
        break;
    case ParamResetDynExpand:
        effect.dynamics.expand = defaults.dynamics.expand;
        break;
    case ParamResetDynFrequency:
        effect.dynamics.frequencyHz = defaults.dynamics.frequencyHz;
        break;
    case ParamResetDynQ:
        effect.dynamics.q = defaults.dynamics.q;
        break;
    case ParamResetDynThreshold:
        effect.dynamics.thresholdDb = defaults.dynamics.thresholdDb;
        break;
    case ParamResetDynRatio:
        effect.dynamics.ratio = defaults.dynamics.ratio;
        break;
    case ParamResetDynRange:
        effect.dynamics.rangeDb = defaults.dynamics.rangeDb;
        break;
    case ParamResetDynAttack:
        effect.dynamics.attackMs = defaults.dynamics.attackMs;
        break;
    case ParamResetDynRelease:
        effect.dynamics.releaseMs = defaults.dynamics.releaseMs;
        break;
    case ParamResetReverbMix:
        effect.reverb.mix = defaults.reverb.mix;
        break;
    case ParamResetReverbSize:
        effect.reverb.size = defaults.reverb.size;
        break;
    case ParamResetReverbDamping:
        effect.reverb.damping = defaults.reverb.damping;
        break;
    case ParamResetReverbPredelay:
        effect.reverb.predelayMs = defaults.reverb.predelayMs;
        break;
    case ParamResetDistFunction:
        effect.distortion.function = defaults.distortion.function;
        break;
    case ParamResetDistDrive:
        effect.distortion.driveDb = defaults.distortion.driveDb;
        break;
    case ParamResetDistCurvature:
        effect.distortion.curvature = defaults.distortion.curvature;
        break;
    case ParamResetDistOutput:
        effect.distortion.outputDb = defaults.distortion.outputDb;
        break;
    default:
        return;
    }

    apo::ClampSettings(settings);
    endpoint.settings = settings;
    ShowEffectForSelected();
    MarkSettingsDirty();
}

void MarkSettingsDirty()
{
    if (g_effectLoading || !ValidEffectIndex()) {
        return;
    }
    g_endpoints[g_effectIndex].settingsDirty = true;
    UpdateEffectsUi();
    RefreshEffectGraph();

    // Effect settings reach the running APO through the runtime file. Apply
    // them without stopping the audio service.
    //
    // Arm the timer only once per burst. Resetting it on every slider step
    // would starve a slow drag: the user could move the slider for a long time
    // and never hear a change because the timer keeps moving. Letting the
    // pending timer fire gives a steady stream of applies during a drag, and
    // the last change is still applied shortly after the drag stops.
    if (!g_liveApplyPending) {
        g_liveApplyPending =
            SetTimer(g_window, kLiveApplyTimer, kLiveApplyDelayMs, nullptr) != 0;
    }
}

void AddEffect(apo::EffectType type)
{
    if (g_busy || !ValidEffectIndex() || !CommitEffectEditor()) {
        return;
    }

    apo::ApoSettings& settings = g_endpoints[g_effectIndex].settings;
    apo::EffectPreset& preset = apo::ActivePreset(settings);
    if (preset.effectCount >= apo::kMaxEffects ||
        CountEffectType(settings, type) >= apo::EffectTypeCapacity(type)) {
        return;
    }

    // The list order is the processing order. Insert after the highlighted
    // effect, or append when nothing is selected.
    const int insertAt =
        g_effectSelection >= 0 && g_effectSelection < preset.effectCount
            ? g_effectSelection + 1
            : preset.effectCount;
    for (int i = preset.effectCount; i > insertAt; --i) {
        preset.effects[i] = preset.effects[i - 1];
    }

    preset.effects[insertAt] =
        apo::MakeEffect(type, preset.nextEffectId++);
    ++preset.effectCount;
    g_effectSelection = insertAt;

    RebuildEffectList();
    ShowEffectForSelected();
    MarkSettingsDirty();
    Relayout();
}

void DeleteEffect(int row)
{
    if (g_busy || !ValidEffectIndex() || !CommitEffectEditor()) {
        return;
    }

    apo::ApoSettings& settings = g_endpoints[g_effectIndex].settings;
    apo::EffectPreset& preset = apo::ActivePreset(settings);
    if (row < 0 || row >= preset.effectCount) {
        return;
    }

    for (int i = row; i + 1 < preset.effectCount; ++i) {
        preset.effects[i] = preset.effects[i + 1];
    }
    --preset.effectCount;
    preset.effects[preset.effectCount] = apo::EffectInstance{};

    if (g_effectSelection > row) {
        --g_effectSelection;
    } else if (g_effectSelection == row) {
        g_effectSelection = (row < preset.effectCount) ? row : row - 1;
    }

    // Remove only the clicked row. Rebuilding the whole list and relaying out
    // the window for every cross click makes rapid deletions feel throttled.
    g_effectLoading = true;
    ListView_DeleteItem(g_effectList, row);
    for (int i = 0; i < preset.effectCount; ++i) {
        ListView_SetCheckState(g_effectList, i,
                               apo::EffectEnabled(preset.effects[i])
                                   ? TRUE
                                   : FALSE);
    }
    if (g_effectSelection >= 0) {
        ListView_SetItemState(g_effectList, g_effectSelection,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_effectList, g_effectSelection, FALSE);
    }
    g_effectLoading = false;

    ShowEffectForSelected();
    MarkSettingsDirty();
}

void DoAddEffect()
{
    if (g_busy || !ValidEffectIndex()) {
        return;
    }

    apo::EffectType type = apo::EffectType::Noise;
    if (!ui::ShowAddEffectDialog(g_window,
                                 g_endpoints[g_effectIndex].settings, type)) {
        return;
    }
    AddEffect(type);
}

void DoApplyEffects();

void DoLiveApply()
{
    KillTimer(g_window, kLiveApplyTimer);
    g_liveApplyPending = false;

    if (g_busy || g_effectLoading || !ValidEffectIndex()) {
        return;
    }

    apo::Endpoint& endpoint = g_endpoints[g_effectIndex];
    if (!endpoint.settingsDirty) {
        return;
    }
    if (!CommitEffectEditor(false)) {
        return;
    }

    const apo::ApoSettings& toSave = endpoint.settings;
    const bool bound = endpoint.Enabled();

    // A bound APO needs a live channel. If it is missing, use the normal apply
    // path, which performs the one restart that installs it. An unbound device
    // has no channel yet; its settings are only saved so the next Enable starts
    // from them.
    if (bound && (!g_files.dllUpToDate ||
                  !apo::RuntimeSettingsFileReady(endpoint.id))) {
        DoApplyEffects();
        return;
    }

    if (!apo::SaveSettings(endpoint.id, toSave)) {
        SetWindowTextW(g_status, L"Could not save the settings file.");
        return;
    }
    if (bound && !apo::WriteRuntimeSettings(endpoint.id, toSave)) {
        SetWindowTextW(g_status, L"Could not apply the settings live.");
        return;
    }

    endpoint.settingsDirty = false;
    UpdateEffectsUi();
    SetWindowTextW(g_status, bound ? L"Settings applied live."
                                   : L"Settings saved.");
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

void UpdateChromeUi()
{
    EnableWindow(g_refresh, !g_busy);
    ui::UpdateMenuCommands(g_window, g_busy,
                           !g_files.FullyInstalled() || g_files.DllStale(),
                           g_files.AnyPresent());
}

void UpdateEffectsUi()
{
    const bool hasDevice = ValidEffectIndex();
    const bool dirty = EffectsDirty();
    const bool active =
        hasDevice && apo::IsDeviceActive(g_endpoints[g_effectIndex].state);
    const bool bound = hasDevice && g_endpoints[g_effectIndex].Enabled();

    // Lock the device selector while edits are pending so a half-edited device
    // cannot be left behind. The automatic apply unlocks it.
    EnableWindow(g_effectDeviceCombo, !g_busy && !dirty);
    // The preset selector only changes which list is edited, so it stays usable
    // while edits are pending; the switch commits the editor and applies live.
    EnableWindow(g_effectPresetCombo, !g_busy && hasDevice);
    // Keep the list enabled while busy: the native disabled state repaints the
    // whole control light. Interaction is rejected in the notification handler
    // instead.
    EnableWindow(g_effectList, hasDevice);
    EnableEffectControls(!g_busy && hasDevice);
    const int effectCount =
        hasDevice ? apo::ActivePreset(g_endpoints[g_effectIndex].settings)
                        .effectCount
                  : 0;
    EnableWindow(g_effectAdd,
                 !g_busy && hasDevice && effectCount < apo::kMaxEffects);
    EnableWindow(g_listen, !g_busy && hasDevice && active && bound);
    UpdateParamResetButtons();

    UpdateMainUi();
}

void UpdateMainUi()
{
    if (g_mainEnable == nullptr || g_mainHint == nullptr) {
        return;
    }

    const bool hasDevice = ValidEffectIndex();
    const bool active =
        hasDevice && apo::IsDeviceActive(g_endpoints[g_effectIndex].state);
    const bool bound = hasDevice && g_endpoints[g_effectIndex].Enabled();

    EnableWindow(g_mainEnable, !g_busy && active);
    // Keep the user's just-clicked state visible while the worker runs. The
    // refresh after the operation sets the real state.
    if (!g_busy) {
        SendMessageW(g_mainEnable, BM_SETCHECK,
                     bound ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    std::wstring hint;
    if (!hasDevice) {
        hint = g_endpoints.empty()
                   ? L"No capture devices found."
                   : L"No active capture devices. Turn on View / Show all "
                     L"devices to edit an inactive endpoint.";
    } else if (!active) {
        hint = L"This device is inactive. Its settings can be edited, but "
               L"Enable needs an active device.";
    } else if (bound) {
        hint = L"Enabled on this device. Effects apply live and other effects "
               L"are ignored.";
    } else if (!g_files.FullyInstalled() || g_files.DllStale()) {
        hint = L"Not enabled. Checking Enable installs or updates the APO, "
               L"then applies it to this device.";
    } else {
        hint = L"Not enabled. Checking Enable applies the APO to this device.";
    }
    SetWindowTextW(g_mainHint, hint.c_str());
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void RebuildEffectCombo(const std::wstring& selectedId)
{
    g_comboLoading = true;
    SendMessageW(g_effectDeviceCombo, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_effectDeviceCombo, CB_RESETCONTENT, 0, 0);

    int selectedItem = -1;
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        const apo::Endpoint& endpoint = g_endpoints[i];
        if (!EndpointVisible(endpoint)) {
            continue;
        }

        const std::wstring label = SelectorLabel(endpoint);
        const int item = static_cast<int>(SendMessageW(
            g_effectDeviceCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str())));
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
    // Refresh also runs after Apply. Keep the audition stream when the same
    // endpoint is still selected; StopListening handles the cases where audio
    // actually restarted.
    const bool wasListening = IsListening();
    std::wstring listenId;
    if (wasListening && ValidEffectIndex()) {
        listenId = g_endpoints[g_effectIndex].id;
    }

    std::wstring effectId;
    if (ValidEffectIndex()) {
        effectId = g_endpoints[g_effectIndex].id;
    }

    // An install or an apply refreshes the endpoint list. Carry unsaved effect
    // edits across it so they are not silently discarded.
    std::vector<std::pair<std::wstring, apo::ApoSettings>> pendingEffects;
    if (g_preserveDirtyEffects) {
        for (const apo::Endpoint& endpoint : g_endpoints) {
            if (endpoint.settingsDirty) {
                pendingEffects.emplace_back(endpoint.id, endpoint.settings);
            }
        }
    }
    g_preserveDirtyEffects = false;

    g_effectIndex = -1;
    g_files = apo::GetFilesStatus();
    g_endpoints = apo::EnumEndpoints();
    for (apo::Endpoint& endpoint : g_endpoints) {
        endpoint.settingsDirty = false;
    }
    for (apo::Endpoint& endpoint : g_endpoints) {
        for (const auto& pending : pendingEffects) {
            if (_wcsicmp(endpoint.id.c_str(), pending.first.c_str()) == 0) {
                endpoint.settings = pending.second;
                endpoint.settingsDirty = true;
                break;
            }
        }
    }

    RebuildEffectCombo(effectId);
    RebuildEffectList();
    ShowEffectForSelected();
    UpdateChromeUi();
    UpdateEffectsUi();
    ResizeColumns();
    RepaintAll();

    if (wasListening) {
        const bool stillMonitored =
            ValidEffectIndex() &&
            _wcsicmp(g_endpoints[g_effectIndex].id.c_str(),
                     listenId.c_str()) == 0;
        if (!stillMonitored) {
            StopListening();
        }
    }
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

void StartWork(std::function<apo::Result()> work, const wchar_t* status)
{
    if (g_busy) {
        return;
    }

    KillTimer(g_window, kLiveApplyTimer);
    g_liveApplyPending = false;
    g_busy = true;
    SetWindowTextW(g_status, status);
    UpdateChromeUi();
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
    g_resumeListen = false;
    g_resumeListenId.clear();
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
    g_resumeListen = false;
    g_resumeListenId.clear();
    g_preserveDirtyEffects = true;
    StartWork([] { return apo::UninstallFiles(); }, L"Uninstalling...");
}

void StartApply(std::vector<apo::Change> changes, const wchar_t* status)
{
    // Only a real audio restart interrupts the audition stream. Settings-only
    // applies reach the APO through the runtime file, so keep listening.
    const bool restart = apo::ChangesNeedRestart(changes);
    const bool wasListening = restart && IsListening();
    std::wstring listenId;
    if (wasListening && ValidEffectIndex()) {
        listenId = g_endpoints[g_effectIndex].id;
    }

    if (restart) {
        StopListening();
    }
    g_resumeListen = wasListening && !listenId.empty();
    g_resumeListenId = g_resumeListen ? listenId : std::wstring();

    StartWork([changes = std::move(changes)] {
        return apo::ApplyChanges(changes);
    }, status);
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

// Writes pending effect edits before an enable/disable operation refreshes the
// endpoint list.
void FlushDirtySettings()
{
    for (apo::Endpoint& endpoint : g_endpoints) {
        if (!endpoint.settingsDirty) {
            continue;
        }
        if (apo::SaveSettings(endpoint.id, endpoint.settings)) {
            endpoint.settingsDirty = false;
        }
    }
}

// The main Enable checkbox. Enabling installs or updates the APO when needed,
// then binds the selected device. Disabling unbinds it; when it was the last
// bound device the files and registration are removed as well.
void DoEnable(bool enable)
{
    if (g_busy || !ValidEffectIndex()) {
        return;
    }

    apo::Endpoint& endpoint = g_endpoints[g_effectIndex];
    if (!apo::IsDeviceActive(endpoint.state)) {
        return;
    }

    const std::wstring id = endpoint.id;
    const apo::ApoSettings settings = endpoint.settings;
    const bool bound = endpoint.Enabled();
    if (enable == bound) {
        UpdateMainUi();
        return;
    }

    int enabledCount = 0;
    for (const apo::Endpoint& item : g_endpoints) {
        if (item.Enabled()) {
            ++enabledCount;
        }
    }
    const bool uninstall = !enable && enabledCount <= 1;

    // Keep edits made on other devices across the refresh. The selected device
    // is saved again by the worker as part of the change.
    FlushDirtySettings();
    g_preserveDirtyEffects = true;

    // A binding change is not a settings-only apply, so do not bring the
    // audition stream back afterwards.
    StopListening();
    g_resumeListen = false;
    g_resumeListenId.clear();

    if (enable) {
        StartWork([id, settings] {
            // Check the installed DLL and the APO registration every time. The
            // hash check also catches an outdated DLL left by an older build.
            const apo::FilesStatus status = apo::GetFilesStatus();
            if (!status.FullyInstalled() || status.DllStale()) {
                const apo::Result installed = apo::InstallFiles();
                if (!installed.ok) {
                    return installed;
                }
            }

            apo::Change change;
            change.id = id;
            change.enable = true;
            change.settingsChanged = true;
            change.settings = settings;
            return apo::ApplyChanges({change});
        }, L"Enabling...");
    } else if (uninstall) {
        StartWork([id, settings] {
            apo::Result result;
            // UninstallFiles restores every binding and removes the files in a
            // single audio stop. Keep the effect edits for the next enable.
            if (!apo::SaveSettings(id, settings)) {
                result.details += L"Could not save the settings file.\r\n";
            }
            const apo::Result uninstalled = apo::UninstallFiles();
            result.ok = uninstalled.ok;
            result.summary = uninstalled.summary;
            result.details += uninstalled.details;
            return result;
        }, L"Disabling...");
    } else {
        StartWork([id, settings] {
            apo::Change change;
            change.id = id;
            change.enable = false;
            change.settingsChanged = true;
            change.settings = settings;
            return apo::ApplyChanges({change});
        }, L"Disabling...");
    }
}

void OnMainEnableClicked()
{
    if (g_busy || !ValidEffectIndex()) {
        UpdateMainUi();
        return;
    }

    const bool want =
        SendMessageW(g_mainEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool bound = g_endpoints[g_effectIndex].Enabled();
    if (want == bound) {
        return;
    }

    if (!CommitEffectEditor()) {
        UpdateMainUi();
        return;
    }

    DoEnable(want);
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
    const apo::ApoSettings captureBase = g_endpoints[index].settings;

    // The profile must be raw room noise: disable every effect so the APO is a
    // plain passthrough for the duration of the capture.
    apo::ApoSettings snapshot = captureBase;
    apo::EffectPreset& snapshotPreset = apo::ActivePreset(snapshot);
    for (int i = 0; i < snapshotPreset.effectCount; ++i) {
        apo::SetEffectEnabled(snapshotPreset.effects[i], false);
    }

    StopListening();

    g_workKind = WorkKind::CaptureNoise;
    g_captureIndex = index;
    g_captureSettings = captureBase;
    g_captureSucceeded = false;
    g_busy = true;
    SetWindowTextW(g_status, L"Capturing noise profile...");
    UpdateChromeUi();
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
            // The noise profile is loaded at LockForProcess, so the APO must
            // reload it before the restored settings take effect.
            change.forceRestart = true;
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
        apo::EffectPreset& finalPreset = apo::ActivePreset(final);
        if (captured) {
            // Turn on the first Noise effect, or append one when the list has
            // no noise stage yet and still has room.
            bool enabledNoise = false;
            for (int i = 0; i < finalPreset.effectCount; ++i) {
                if (finalPreset.effects[i].type == apo::EffectType::Noise) {
                    apo::SetEffectEnabled(finalPreset.effects[i], true);
                    enabledNoise = true;
                    break;
                }
            }
            if (!enabledNoise &&
                finalPreset.effectCount < apo::kMaxEffects) {
                finalPreset.effects[finalPreset.effectCount] =
                    apo::MakeEffect(apo::EffectType::Noise,
                                    finalPreset.nextEffectId++);
                ++finalPreset.effectCount;
            }
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
    if (g_effectLoading || !ValidEffectIndex() || item.iItem < 0) {
        return;
    }

    const bool stateChanged = (item.uChanged & LVIF_STATE) != 0;
    if (!stateChanged) {
        return;
    }

    apo::ApoSettings& settings = g_endpoints[g_effectIndex].settings;
    apo::EffectPreset& preset = apo::ActivePreset(settings);
    if (item.iItem >= preset.effectCount) {
        return;
    }

    // The row index is the effect index: the list mirrors the effect array.
    const bool checkChanged =
        ((item.uOldState ^ item.uNewState) & LVIS_STATEIMAGEMASK) != 0;
    if (checkChanged) {
        const bool checked =
            ((item.uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
        apo::SetEffectEnabled(preset.effects[item.iItem], checked);
        MarkSettingsDirty();
    }

    const bool selectionChanged =
        ((item.uOldState ^ item.uNewState) & LVIS_SELECTED) != 0;
    if (selectionChanged && (item.uNewState & LVIS_SELECTED) != 0) {
        // Commit the editor before the selection moves to another effect.
        if (!CommitEffectEditor()) {
            g_effectLoading = true;
            ListView_SetItemState(g_effectList, g_effectSelection,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            g_effectLoading = false;
            return;
        }
        g_effectSelection = item.iItem;
        g_effectType = preset.effects[g_effectSelection].type;
        ShowEffectForSelected();
        Relayout();
    }
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
    int selected = -1;
    if (item >= 0) {
        selected = static_cast<int>(
            SendMessageW(g_effectDeviceCombo, CB_GETITEMDATA, item, 0));
    }

    // The monitor stream belongs to the previously selected endpoint. Stop it
    // when the user picks a different device instead of leaving the meter on
    // an endpoint that is no longer shown.
    if (selected != g_effectIndex) {
        StopListening();
        g_effectSelection = 0;
    }
    g_effectIndex = selected;

    RebuildEffectList();
    ShowEffectForSelected();
    UpdateEffectsUi();
    Relayout();
}

void OnEffectPresetChanged()
{
    if (g_comboLoading || !ValidEffectIndex()) {
        return;
    }
    if (!CommitEffectEditor()) {
        return;
    }

    const int item = static_cast<int>(
        SendMessageW(g_effectPresetCombo, CB_GETCURSEL, 0, 0));
    if (item < 0 || item >= apo::kPresetCount) {
        return;
    }

    apo::ApoSettings& settings = g_endpoints[g_effectIndex].settings;
    if (settings.presetIndex == item) {
        return;
    }

    // Only the selected preset changes. The editor and list are rebuilt from
    // the new list, and the switch reaches the running APO through the normal
    // live-apply path.
    settings.presetIndex = item;
    g_effectSelection = 0;
    RebuildEffectList();
    ShowEffectForSelected();
    MarkSettingsDirty();
    Relayout();
}

LRESULT HandleNotify(LPARAM lParam)
{
    auto* header = reinterpret_cast<NMHDR*>(lParam);

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
        case NM_CLICK: {
            // Column 1 is the delete button. The click arrives before the
            // selection change, so deleting the clicked row directly keeps the
            // row index unambiguous.
            auto* activate = reinterpret_cast<NMITEMACTIVATE*>(lParam);
            if (activate->iItem >= 0 && activate->iSubItem == 1) {
                DeleteEffect(activate->iItem);
                return 0;
            }
            break;
        }
        default:
            break;
        }
        return 0;
    }


    return 0;
}


LRESULT HandleStaticColorMessage(WPARAM wParam, LPARAM)
{
    return ui::HandleStaticColor(reinterpret_cast<HDC>(wParam), false);
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

void ToggleShowAllDevices()
{
    if (g_busy) {
        return;
    }

    std::wstring selectedId;
    if (ValidEffectIndex()) {
        selectedId = g_endpoints[g_effectIndex].id;
    }

    g_showAllDevices = !g_showAllDevices;
    gui::SaveShowAllDevices(g_showAllDevices);
    ui::UpdateMenuShowAllDevices(g_window, g_showAllDevices);

    g_preserveDirtyEffects = true;
    Refresh();
}

void PaintMenuDivider(HWND window)
{
    if (GetMenu(window) == nullptr) {
        return;
    }

    RECT windowRect{};
    GetWindowRect(window, &windowRect);
    POINT clientOrigin{};
    ClientToScreen(window, &clientOrigin);
    const int lineY = clientOrigin.y - windowRect.top - 1;
    if (lineY <= 0) {
        return;
    }

    HDC dc = GetWindowDC(window);
    if (dc == nullptr) {
        return;
    }

    const int width = windowRect.right - windowRect.left;
    const ui::Theme& theme = ui::GetTheme();
    // Cover the native focus-sensitive menu border first, then draw one
    // subtle divider on top so focused and unfocused windows look the same.
    RECT band{0, std::max(0, lineY - 2), width, lineY + 1};
    FillRect(dc, &band, theme.menuBrush);
    RECT line{0, lineY, width, lineY + 1};
    HBRUSH brush = CreateSolidBrush(theme.color.menuDivider);
    FillRect(dc, &line, brush);
    DeleteObject(brush);
    ReleaseDC(window, dc);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                            LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        ui::CreateFonts();
        g_showAllDevices = gui::LoadShowAllDevices();
        ui::CreateMenuBar(window, g_showAllDevices);
        CreateControls(window);
        ui::ApplyNativeThemeToChildren(window);
        ui::ApplyListStateImages(g_effectList);
        Refresh();
        RepaintAll();
        return 0;
    }

    case WM_NCACTIVATE: {
        LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        // Windows repaints the menu bar border with a bright inactive color.
        // Force another non-client paint so our divider always wins.
        RedrawWindow(window, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        return result;
    }

    case WM_NCPAINT: {
        LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        PaintMenuDivider(window);
        return result;
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
        if (g_pageBackground != nullptr) {
            Layout(LOWORD(lParam), HIWORD(lParam));
            RepaintAll();
        }
        break;

    case WM_SETTINGCHANGE:
        if (wParam == 0 && lParam != 0 &&
            wcscmp(reinterpret_cast<const wchar_t*>(lParam),
                   L"ImmersiveColorSet") == 0) {
            ui::RefreshTheme();
            ui::RefreshMenuBar(window);
            ui::ApplyTitleBarTheme(window);
            ui::ApplyNativeThemeToChildren(window);
            ui::ApplyNativeTheme(g_infoTooltip);
            ui::ApplyListStateImages(g_effectList);
            RefreshInfoIcons();
            RedrawWindow(window, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
            RepaintAll();
        }
        break;

    case WM_THEMECHANGED:
        ui::RefreshTheme();
        ui::RefreshMenuBar(window);
        ui::ApplyTitleBarTheme(window);
        ui::ApplyNativeThemeToChildren(window);
        ui::ApplyNativeTheme(g_infoTooltip);
        ui::ApplyListStateImages(g_effectList);
        RefreshInfoIcons();
        RedrawWindow(window, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
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

    case WM_MEASUREITEM:
        if (ui::MeasureMenuItem(
                window, reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        break;

    case WM_DRAWITEM:
        if (ui::DrawIconButton(
                reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        if (ui::DrawMenuItem(window,
                             reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_INSTALL:
            DoInstall();
            break;
        case IDM_FILE_UNINSTALL:
            DoUninstall();
            break;
        case IDM_FILE_EXIT:
            PostMessageW(window, WM_CLOSE, 0, 0);
            break;
        case IDM_VIEW_SHOW_ALL_DEVICES:
            ToggleShowAllDevices();
            break;
        case IDM_ABOUT_INFO:
            ui::ShowInfoDialog(window, g_files, BoundDeviceCount());
            break;
        case IDM_ABOUT_OPEN_LOG:
            DoOpenLog();
            break;
        case IDC_REFRESH:
            g_preserveDirtyEffects = true;
            Refresh();
            break;
        case IDC_MAIN_ENABLE:
            if (HIWORD(wParam) == BN_CLICKED) {
                OnMainEnableClicked();
            }
            break;
        case IDC_EFFECT_ADD:
            if (HIWORD(wParam) == BN_CLICKED) {
                DoAddEffect();
            }
            break;
        case IDC_MAIN_ENABLE_ICON:
        case IDC_GATE_LOOKAHEAD_ICON:
        case IDC_REVERB_PREDELAY_ICON:
            if (HIWORD(wParam) == STN_CLICKED) {
                SendMessageW(g_infoTooltip, TTM_POPUP, 0, 0);
            }
            break;
        case IDC_PARAM_RESET:
            if (HIWORD(wParam) == BN_CLICKED) {
                HWND button = reinterpret_cast<HWND>(lParam);
                if (button != nullptr) {
                    int param = static_cast<int>(
                        GetWindowLongPtrW(button, GWLP_USERDATA));
                    ResetParameter(param);
                }
            }
            break;
        case IDC_EFFECT_DEVICE_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                OnEffectComboChanged();
            } else if (HIWORD(wParam) == CBN_CLOSEUP) {
                RepaintAll();
            }
            break;
        case IDC_EFFECT_PRESET_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                OnEffectPresetChanged();
            } else if (HIWORD(wParam) == CBN_CLOSEUP) {
                RepaintAll();
            }
            break;
        case IDC_DIST_FUNCTION_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                MarkSettingsDirty();
            } else if (HIWORD(wParam) == CBN_CLOSEUP) {
                RepaintAll();
            }
            break;
        case IDC_GATE_THRESHOLD_SLIDER:
        case IDC_GATE_RANGE_SLIDER:
        case IDC_GATE_ATTACK_SLIDER:
        case IDC_GATE_HOLD_SLIDER:
        case IDC_GATE_RELEASE_SLIDER:
        case IDC_GATE_LOOKAHEAD_SLIDER:
        case IDC_COMP_THRESHOLD_SLIDER:
        case IDC_COMP_RATIO_SLIDER:
        case IDC_COMP_ATTACK_SLIDER:
        case IDC_COMP_RELEASE_SLIDER:
        case IDC_MAKEUP_SLIDER:
        case IDC_NOISE_REDUCTION_SLIDER:
        case IDC_DYN_FREQUENCY_SLIDER:
        case IDC_DYN_Q_SLIDER:
        case IDC_DYN_THRESHOLD_SLIDER:
        case IDC_DYN_RATIO_SLIDER:
        case IDC_DYN_RANGE_SLIDER:
        case IDC_DYN_ATTACK_SLIDER:
        case IDC_DYN_RELEASE_SLIDER:
        case IDC_REVERB_MIX_SLIDER:
        case IDC_REVERB_SIZE_SLIDER:
        case IDC_REVERB_DAMPING_SLIDER:
        case IDC_REVERB_PREDELAY_SLIDER:
        case IDC_DIST_DRIVE_SLIDER:
        case IDC_DIST_CURVATURE_SLIDER:
        case IDC_DIST_OUTPUT_SLIDER:
            if (ui::IsSliderChange(HIWORD(wParam))) {
                MarkSettingsDirty();
            }
            break;
        case IDC_DYNAMIC_EXTEND:
            if (HIWORD(wParam) == BN_CLICKED) {
                MarkSettingsDirty();
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
            // The captured device was saved by the worker, so its effect edits
            // are no longer dirty.
            apo::Endpoint& endpoint = g_endpoints[captureIndex];
            endpoint.settings = g_captureSettings;
            endpoint.settingsDirty = false;
            RebuildEffectList();
            ShowEffectForSelected();
            UpdateChromeUi();
            UpdateEffectsUi();
            Relayout();
        } else {
            // A failed operation may have changed nothing, so keep unsaved
            // effect edits instead of reloading over them.
            if (!g_result.ok) {
                g_preserveDirtyEffects = true;
            }
            Refresh();

            // A settings edit can arrive while an apply is running. Resume the
            // live save now that the worker is done; if the live channel is
            // missing, the edit stays pending for the normal Apply path.
            if (kind == WorkKind::Generic && ValidEffectIndex() &&
                g_endpoints[g_effectIndex].settingsDirty) {
                g_liveApplyPending =
                    SetTimer(g_window, kLiveApplyTimer, kLiveApplyDelayMs,
                             nullptr) != 0;
            }
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
        } else if (wParam == kLiveApplyTimer) {
            DoLiveApply();
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
        gui::SaveShowAllDevices(g_showAllDevices);
        ui::DestroyMenuBar(window);
        KillTimer(window, kMonitorTimer);
        KillTimer(window, kLiveApplyTimer);
        g_liveApplyPending = false;
        apo::StopMonitor();
        if (g_worker.joinable()) {
            g_worker.join();
        }
        ui::DestroyFonts();
        DestroyInfoIcons();
        ui::ShutdownTheme();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return 0;
}

std::wstring DescribeEffectList(const apo::ApoSettings& settings)
{
    const apo::EffectPreset& preset = apo::ActivePreset(settings);
    std::wstring text;
    for (int i = 0; i < preset.effectCount; ++i) {
        const apo::EffectInstance& effect = preset.effects[i];
        if (i > 0) {
            text += L",";
        }
        text += apo::EffectTypeName(effect.type);
        text += apo::EffectEnabled(effect) ? L"(on)" : L"(off)";
    }
    return text;
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
                L" | endpoint=" +
                (endpoint.endpointOurs ? L"installed" : L"no") +
                L" | composite_endpoint=" +
                (endpoint.compositeEndpointOurs ? L"installed" : L"no") +
                L" | sysfx=" +
                (endpoint.disableSysFx ? L"disabled" : L"enabled") +
                L" | effects=[" +
                DescribeEffectList(endpoint.settings) + L"]" +
                L" | noise_profile=" +
                (apo::FileExists(apo::NoiseProfilePath(endpoint.id)) ? L"yes"
                                                                   : L"no") +
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

