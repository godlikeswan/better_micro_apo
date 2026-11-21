#include "apo.h"

#include "apo_internal.h"
#include "apo_monitor.h"
#include "preferences.h"
#include "runtime_settings.h"
#include "stage_capture.h"
#include "stage_monitor.h"

#include "ui/effect_dialog.h"
#include "ui/graph.h"
#include "ui/info_dialog.h"
#include "ui/menu.h"
#include "ui/structure.h"
#include "ui/theme.h"
#include "ui/ui_state.h"
#include "ui/widgets.h"

#include <commctrl.h>
#include <dbt.h>
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
constexpr UINT_PTR kRefreshTimer = 3;
constexpr UINT_PTR kDeviceChangeTimer = 4;
constexpr UINT_PTR kStageMonitorTimer = 5;
constexpr UINT kLiveApplyDelayMs = 200;
constexpr UINT kRefreshPollMs = 1200;
constexpr UINT kStageMonitorPollMs = 100;
constexpr UINT kDeviceChangeDebounceMs = 200;
constexpr DWORD kMeterWindowMs = 500;
// Keyboard Looper gestures use the main window's timer IDs. The mouse buttons
// own their own timer IDs, so these never collide.
constexpr UINT_PTR kLooperKeyLongPressBase = 10;
constexpr UINT_PTR kLooperKeyTapBase = 20;

constexpr wchar_t kWindowClass[] = L"BetterMicroApoInstaller";
constexpr wchar_t kInstanceMutex[] = L"Local\\better_micro_apo_gui_instance";


DWORD g_clipHoldUntil = 0;
unsigned g_dropTotal = 0;
bool g_liveApplyPending = false;

// Looper gestures are optimistic: telemetry confirms the APO's real transport
// state on the next poll, but the labels and the next gesture must not wait for
// that round trip. Commands use a monotonic serial so a later settings-only
// apply cannot replay an old transport request.
apo::LooperState g_looperLocalStates[apo::kLooperTrackCount]{};
apo::LooperState g_looperLocalUndoState = apo::LooperState::Empty;
int g_looperLocalUndoTrack = -1;
bool g_looperLocalUndoValid = false;
std::wstring g_looperLocalEndpointId;
int g_looperLocalEffectId = -1;
ULONGLONG g_looperCommandSerial = 0;
ULONG g_lastLooperMonitorGeneration = 0;
std::wstring g_lastLooperMonitorEndpointId;
DWORD g_looperMonitorMissingSince = 0;

struct LooperKeyGestureState
{
    bool down = false;
    bool longFired = false;
    bool consumed = false;
    bool tapPending = false;
    bool provisional = false;
    // Recording already finished on the first press; the gesture only needs
    // to know whether a later double tap must still stop the track.
    bool provisionalFinish = false;
    DWORD downTick = 0;
    DWORD lastTapTick = 0;
    unsigned generation = 0;
    unsigned timerGeneration = 0;
    apo::LooperState originalState = apo::LooperState::Empty;
    bool originalUndoValid = false;
    int originalUndoTrack = -1;
    apo::LooperState originalUndoState = apo::LooperState::Empty;
};

LooperKeyGestureState g_looperKeyGestures[apo::kLooperTrackCount];

apo::DynamicsSettings& DynamicsOf(apo::EffectInstance& effect)
{
    return effect.type == apo::EffectType::Gate ? effect.gate
                                                 : effect.compressor;
}

const apo::DynamicsSettings& DynamicsOf(const apo::EffectInstance& effect)
{
    return effect.type == apo::EffectType::Gate ? effect.gate
                                                 : effect.compressor;
}

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
apo::StageMonitorView g_stageMonitor;
std::thread g_worker;
apo::Result g_result;

int g_effectIndex = -1;   // Selected endpoint in the device combo.

constexpr unsigned kNoiseCaptureMs = 3000;


// Apply briefly stops the audio service, which also stops the monitor stream.
// Remember the listening state so it can be restarted once Apply finishes.
bool g_resumeListen = false;
std::wstring g_resumeListenId;

// Install/Uninstall refreshes the endpoint list. Keep unsaved effect edits so
// the refresh does not silently discard them.
bool g_preserveDirtyEffects = false;

// Snapshot of externally visible install/device state. The UI compares it on a
// timer and after device-change notifications so plug/unplug and out-of-band
// binding edits refresh without a manual Refresh button.
std::wstring g_stateSignature;

// Forward declarations for helpers that are used before their definitions.
void UpdateMainUi();
void UpdateEffectsUi();
void ApplyEqualizerDynamicEnable(bool parentEnabled);
void UpdateParamResetButtons();
void MarkSettingsDirty();
void RefreshEffectGraph();
bool ValidEffectIndex();
void DoEnable(bool enable);
void UpdateChromeUi();
void PollStageMonitor();
void HandleLooperKeyTimer(int track, bool longTimer);
bool PublishLooperCommand(apo::LooperCommand command, int track);
void SetLooperLocalState(int track, apo::LooperState state);

// Describes the install state and the identity/state/bindings of every capture
// endpoint. Deliberately excludes per-device settings: those are edited in
// memory by this window, so including them would make a poll reload over the
// user's unsaved effect edits.
std::wstring StateSignature(const apo::FilesStatus& files,
                            const std::vector<apo::Endpoint>& endpoints)
{
    std::wstring text;
    text.reserve(256);
    const auto flag = [&text](bool value) { text += value ? L'1' : L'0'; };

    flag(files.dllPresent);
    flag(files.dllUpToDate);
    flag(files.comRegistered);
    flag(files.apoKeyPresent);
    text += std::to_wstring(files.embeddedHash);
    text += L'/';
    text += std::to_wstring(files.installedHash);
    text += files.registeredPath;
    text += L'|';

    for (const apo::Endpoint& endpoint : endpoints) {
        text += endpoint.id;
        text += L'/';
        text += endpoint.name;
        text += L'/';
        text += std::to_wstring(endpoint.state);
        flag(endpoint.premixOurs);
        flag(endpoint.streamOurs);
        flag(endpoint.endpointOurs);
        flag(endpoint.disableSysFx);
        flag(endpoint.compositePresent);
        flag(endpoint.compositeOurs);
        flag(endpoint.compositeEndpointOurs);
        text += L';';
    }
    return text;
}

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
    case apo::EffectType::Input:
        showAll({g_inputGainLabel, g_inputGainSlider, g_inputTreatMono});
        break;
    case apo::EffectType::Noise:
        showAll({g_noiseReductionLabel, g_noiseReductionSlider,
                 g_noiseFftSizeLabel, g_noiseFftSizeCombo, g_noiseCapture,
                 g_noiseNote});
        break;
    case apo::EffectType::Gate:
    case apo::EffectType::Compressor:
        showAll({g_dynamicsInputLabel, g_dynamicsInputSlider,
                 g_dynamicsThresholdLabel, g_dynamicsThresholdSlider,
                 g_dynamicsRatioLabel, g_dynamicsRatioSlider,
                 g_dynamicsAttackLabel, g_dynamicsAttackSlider,
                 g_dynamicsReleaseLabel, g_dynamicsReleaseSlider,
                 g_dynamicsHoldLabel, g_dynamicsHoldSlider,
                 g_dynamicsOutputLabel, g_dynamicsOutputSlider,
                 g_dynamicsLookaheadLabel, g_dynamicsLookaheadSlider});
        break;
    case apo::EffectType::Equalizer:
        showAll({g_eqDynamicLabel, g_eqDynamicCombo, g_eqFrequencyLabel,
                 g_eqFrequencySlider, g_eqQLabel, g_eqQSlider,
                 g_eqLevelLabel, g_eqLevelSlider, g_eqThresholdLabel,
                 g_eqThresholdSlider, g_eqRatioLabel, g_eqRatioSlider,
                 g_eqRangeLabel, g_eqRangeSlider, g_eqAttackLabel,
                 g_eqAttackSlider, g_eqReleaseLabel, g_eqReleaseSlider,
                 g_eqHoldLabel, g_eqHoldSlider, g_eqLookaheadLabel,
                 g_eqLookaheadSlider});
        break;
    case apo::EffectType::Gain:
        showAll({g_gainLabel, g_gainSlider});
        break;
    case apo::EffectType::Saturation:
        showAll({g_distFunctionLabel, g_distFunctionCombo,
                 g_distDriveLabel, g_distDriveSlider,
                 g_distCurvatureLabel, g_distCurvatureSlider,
                 g_distOutputLabel, g_distOutputSlider});
        break;
    case apo::EffectType::Reverb:
        showAll({g_reverbAlgorithmLabel, g_reverbAlgorithmCombo,
                 g_reverbLevelLabel, g_reverbLevelSlider,
                 g_reverbMaxLengthLabel, g_reverbMaxLengthSlider,
                 g_reverbSizeLabel, g_reverbSizeSlider,
                 g_reverbDampingLabel, g_reverbDampingSlider,
                 g_reverbWidthLabel, g_reverbWidthSlider,
                 g_reverbDensityLabel, g_reverbDensitySlider,
                 g_reverbPredelayLabel, g_reverbPredelaySlider});
        break;
    case apo::EffectType::Echo:
        showAll({g_echoModeLabel, g_echoModeCombo, g_echoTimeLabel,
                 g_echoTimeSlider, g_echoFeedbackLabel, g_echoFeedbackSlider,
                 g_echoDampingLabel, g_echoDampingSlider, g_echoLevelLabel,
                 g_echoLevelSlider});
        break;
    case apo::EffectType::Chorus:
        showAll({g_chorusVoicesLabel, g_chorusVoicesSlider,
                 g_chorusRateLabel, g_chorusRateSlider, g_chorusDepthLabel,
                 g_chorusDepthSlider, g_chorusWidthLabel, g_chorusWidthSlider,
                 g_chorusLevelLabel, g_chorusLevelSlider});
        break;
    case apo::EffectType::Tremolo:
        showAll({g_tremoloRateLabel, g_tremoloRateSlider,
                 g_tremoloDepthLabel, g_tremoloDepthSlider,
                 g_tremoloWaveformLabel, g_tremoloWaveformCombo,
                 g_tremoloSpreadLabel, g_tremoloSpreadSlider});
        break;
    case apo::EffectType::Vibrato:
        showAll({g_vibratoRateLabel, g_vibratoRateSlider,
                 g_vibratoDepthLabel, g_vibratoDepthSlider,
                 g_vibratoWaveformLabel, g_vibratoWaveformCombo,
                 g_vibratoSpreadLabel, g_vibratoSpreadSlider});
        break;
    case apo::EffectType::PitchShifter:
        showAll({g_pitchAlgorithmLabel, g_pitchAlgorithmCombo,
                 g_pitchShiftLabel, g_pitchShiftSlider, g_pitchWindowLabel,
                 g_pitchWindowSlider, g_pitchLevelLabel, g_pitchLevelSlider,
                 g_pitchDryLevelLabel, g_pitchDryLevelSlider});
        break;
    case apo::EffectType::Tuner:
        showAll({g_tunerAlgorithmLabel, g_tunerAlgorithmCombo,
                 g_tunerSampleSizeLabel, g_tunerSampleSizeCombo,
                 g_tunerSmoothingLabel, g_tunerSmoothingSlider,
                 g_tunerReferenceLabel, g_tunerReferenceSlider});
        break;
    case apo::EffectType::Looper:
        showAll({g_looperHeaderTrack, g_looperHeaderState,
                 g_looperHeaderVolume, g_looperHeaderFree,
                 g_looperHeaderStop, g_looperNote});
        for (int track = 0; track < apo::kLooperTrackCount; ++track) {
            showAll({g_looperButtons[track], g_looperStateLabels[track],
                     g_looperVolumeSliders[track],
                     g_looperFreeLength[track], g_looperStopAtEnd[track]});
        }
        break;
    default:
        break;
    }
}

void EnableEffectControls(bool enabled)
{
    const HWND controls[] = {
        g_dynamicsInputLabel, g_dynamicsInputSlider,
        g_dynamicsThresholdLabel, g_dynamicsThresholdSlider,
        g_dynamicsRatioLabel, g_dynamicsRatioSlider,
        g_dynamicsAttackLabel, g_dynamicsAttackSlider,
        g_dynamicsReleaseLabel, g_dynamicsReleaseSlider,
        g_dynamicsHoldLabel, g_dynamicsHoldSlider,
        g_dynamicsOutputLabel, g_dynamicsOutputSlider,
        g_dynamicsLookaheadLabel, g_dynamicsLookaheadSlider,
        g_inputGainLabel, g_inputGainSlider, g_inputTreatMono,
        g_gainLabel, g_gainSlider,
        g_eqDynamicLabel, g_eqDynamicCombo,
        g_eqFrequencyLabel, g_eqFrequencySlider,
        g_eqQLabel, g_eqQSlider,
        g_eqLevelLabel, g_eqLevelSlider,
        g_eqThresholdLabel, g_eqThresholdSlider,
        g_eqRatioLabel, g_eqRatioSlider,
        g_eqRangeLabel, g_eqRangeSlider,
        g_eqAttackLabel, g_eqAttackSlider,
        g_eqReleaseLabel, g_eqReleaseSlider,
        g_eqHoldLabel, g_eqHoldSlider,
        g_eqLookaheadLabel, g_eqLookaheadSlider,
        g_distFunctionLabel, g_distFunctionCombo,
        g_distDriveLabel, g_distDriveSlider,
        g_distCurvatureLabel, g_distCurvatureSlider,
        g_distOutputLabel, g_distOutputSlider,
        g_reverbAlgorithmLabel, g_reverbAlgorithmCombo,
        g_reverbLevelLabel, g_reverbLevelSlider,
        g_reverbMaxLengthLabel, g_reverbMaxLengthSlider,
        g_reverbSizeLabel, g_reverbSizeSlider,
        g_reverbDampingLabel, g_reverbDampingSlider,
        g_reverbWidthLabel, g_reverbWidthSlider,
        g_reverbDensityLabel, g_reverbDensitySlider,
        g_reverbPredelayLabel, g_reverbPredelaySlider,
        g_echoModeLabel, g_echoModeCombo, g_echoTimeLabel, g_echoTimeSlider,
        g_echoFeedbackLabel, g_echoFeedbackSlider, g_echoDampingLabel,
        g_echoDampingSlider, g_echoLevelLabel, g_echoLevelSlider,
        g_chorusVoicesLabel, g_chorusVoicesSlider, g_chorusRateLabel,
        g_chorusRateSlider, g_chorusDepthLabel, g_chorusDepthSlider,
        g_chorusWidthLabel, g_chorusWidthSlider, g_chorusLevelLabel,
        g_chorusLevelSlider,
        g_tremoloRateLabel, g_tremoloRateSlider, g_tremoloDepthLabel,
        g_tremoloDepthSlider, g_tremoloWaveformLabel, g_tremoloWaveformCombo,
        g_tremoloSpreadLabel, g_tremoloSpreadSlider,
        g_vibratoRateLabel, g_vibratoRateSlider, g_vibratoDepthLabel,
        g_vibratoDepthSlider, g_vibratoWaveformLabel, g_vibratoWaveformCombo,
        g_vibratoSpreadLabel, g_vibratoSpreadSlider,
        g_pitchAlgorithmLabel, g_pitchAlgorithmCombo, g_pitchShiftLabel,
        g_pitchShiftSlider, g_pitchWindowLabel, g_pitchWindowSlider,
        g_pitchLevelLabel, g_pitchLevelSlider,
        g_pitchDryLevelLabel, g_pitchDryLevelSlider,
        g_noiseReductionLabel, g_noiseReductionSlider,
        g_noiseFftSizeLabel, g_noiseFftSizeCombo, g_noiseCapture,
        g_tunerAlgorithmLabel, g_tunerAlgorithmCombo,
        g_tunerSampleSizeLabel, g_tunerSampleSizeCombo,
        g_tunerSmoothingLabel, g_tunerSmoothingSlider,
        g_tunerReferenceLabel, g_tunerReferenceSlider,
        g_looperButtons[0], g_looperStateLabels[0], g_looperVolumeSliders[0],
        g_looperFreeLength[0], g_looperStopAtEnd[0],
        g_looperButtons[1], g_looperStateLabels[1], g_looperVolumeSliders[1],
        g_looperFreeLength[1], g_looperStopAtEnd[1],
        g_looperButtons[2], g_looperStateLabels[2], g_looperVolumeSliders[2],
        g_looperFreeLength[2], g_looperStopAtEnd[2],
        g_looperButtons[3], g_looperStateLabels[3], g_looperVolumeSliders[3],
        g_looperFreeLength[3], g_looperStopAtEnd[3],
        g_looperButtons[4], g_looperStateLabels[4], g_looperVolumeSliders[4],
        g_looperFreeLength[4], g_looperStopAtEnd[4],
        g_looperHeaderTrack, g_looperHeaderState, g_looperHeaderVolume,
        g_looperHeaderFree, g_looperHeaderStop,
        g_looperNote,
        g_effectGraph,
    };
    for (HWND control : controls) {
        EnableWindow(control, enabled);
    }
    ApplyEqualizerDynamicEnable(enabled);
}

// The Equalizer's static controls (frequency, Q, level) stay usable with
// dynamics off; only the detector controls follow the Dynamic selector.
void ApplyEqualizerDynamicEnable(bool parentEnabled)
{
    const int mode = static_cast<int>(
        SendMessageW(g_eqDynamicCombo, CB_GETCURSEL, 0, 0));
    const bool hasDynamics =
        mode > static_cast<int>(apo::EqualizerDynamicsMode::None) &&
        mode < static_cast<int>(apo::EqualizerDynamicsMode::Count);
    const bool enabled = parentEnabled && hasDynamics && HasEffectSelection() &&
                         g_effectType == apo::EffectType::Equalizer;
    const HWND controls[] = {
        g_eqThresholdLabel,  g_eqThresholdSlider,
        g_eqRatioLabel,      g_eqRatioSlider,
        g_eqRangeLabel,      g_eqRangeSlider,
        g_eqAttackLabel,     g_eqAttackSlider,
        g_eqReleaseLabel,    g_eqReleaseSlider,
        g_eqHoldLabel,       g_eqHoldSlider,
        g_eqLookaheadLabel,  g_eqLookaheadSlider,
        g_eqLookaheadIcon,
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
    SendMessageW(g_eqDynamicCombo, CB_SETCURSEL,
                 static_cast<int>(apo::EqualizerDynamicsMode::None), 0);
    SendMessageW(g_distFunctionCombo, CB_SETCURSEL, apo::kDistortionTanh, 0);
    SendMessageW(g_noiseFftSizeCombo, CB_SETCURSEL,
                 apo::NoiseFftSizeIndex(apo::kNoiseDefaultFftSize), 0);
    SendMessageW(g_echoModeCombo, CB_SETCURSEL,
                 static_cast<int>(apo::EchoMode::Stereo), 0);
    SendMessageW(g_tremoloWaveformCombo, CB_SETCURSEL,
                 static_cast<int>(apo::ModulationWaveform::Sine), 0);
    SendMessageW(g_vibratoWaveformCombo, CB_SETCURSEL,
                 static_cast<int>(apo::ModulationWaveform::Sine), 0);
    SendMessageW(g_pitchAlgorithmCombo, CB_SETCURSEL,
                 static_cast<int>(apo::PitchAlgorithm::Granular), 0);
    SendMessageW(g_tunerAlgorithmCombo, CB_SETCURSEL,
                 static_cast<int>(apo::TunerAlgorithm::Autocorrelation), 0);
    SendMessageW(g_tunerSampleSizeCombo, CB_SETCURSEL,
                 apo::TunerSampleSizeIndex(apo::kTunerDefaultSampleSize), 0);
    SetWindowTextW(g_effectHeading, L"Select an effect");
    SetWindowTextW(g_noiseNote, L"");
    ShowWindow(g_effectGraph, SW_HIDE);
    g_effectLoading = false;
    ui::ClearGraphMonitor(g_effectGraph);
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
            // Input is structural: no delete affordance and always checked.
            ListView_SetItemText(
                g_effectList, row, 1,
                const_cast<wchar_t*>(i == 0 ? L"" : L"\u00D7"));
            ListView_SetCheckState(
                g_effectList, row,
                (i == 0 || apo::EffectEnabled(effect)) ? TRUE : FALSE);
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

void CancelLooperKeyGestures()
{
    for (int track = 0; track < apo::kLooperTrackCount; ++track) {
        LooperKeyGestureState& state = g_looperKeyGestures[track];
        if (g_window != nullptr) {
            KillTimer(g_window, kLooperKeyLongPressBase + track);
            KillTimer(g_window, kLooperKeyTapBase + track);
        }
        if (state.provisional && !state.provisionalFinish) {
            // Focus loss or a reset must not leave a half-gesture take behind.
            if (!g_busy && !g_effectLoading && ValidEffectIndex() &&
                HasEffectSelection() && g_endpoints[g_effectIndex].Enabled())
            {
                PublishLooperCommand(apo::LooperCommand::DiscardProvisional,
                                     track);
            }
            g_looperLocalUndoValid = state.originalUndoValid;
            g_looperLocalUndoTrack = state.originalUndoTrack;
            g_looperLocalUndoState = state.originalUndoState;
            SetLooperLocalState(track, state.originalState);
        }
        ++state.generation;
        state.timerGeneration = state.generation;
        state.down = false;
        state.longFired = false;
        state.consumed = false;
        state.tapPending = false;
        state.provisional = false;
        state.provisionalFinish = false;
        state.lastTapTick = 0;
    }
}

void SetLooperLocalState(int track, apo::LooperState state)
{
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return;
    }
    g_looperLocalStates[track] = state;
    if (g_looperStateLabels[track] != nullptr) {
        SetWindowTextW(g_looperStateLabels[track],
                       apo::LooperStateName(state));
    }
}

void ResetLooperLocalStates()
{
    CancelLooperKeyGestures();
    for (int track = 0; track < apo::kLooperTrackCount; ++track) {
        SetLooperLocalState(track, apo::LooperState::Empty);
    }
    g_looperLocalUndoState = apo::LooperState::Empty;
    g_looperLocalUndoTrack = -1;
    g_looperLocalUndoValid = false;
}

// Takes are restored from the per-device bank, not guessed by the GUI. Reset
// the optimistic state only when the selected Looper instance actually changes,
// so switching away and back does not make a live loop look empty.
void EnsureLooperLocalStates(const apo::Endpoint& endpoint, int effectId)
{
    if (g_looperLocalEndpointId == endpoint.id &&
        g_looperLocalEffectId == effectId)
    {
        return;
    }
    g_looperLocalEndpointId = endpoint.id;
    g_looperLocalEffectId = effectId;
    ResetLooperLocalStates();
}

// Copies the editor controls into `effect`. The sliders always hold valid
// values, but ClampSettings still runs after this so a programmatic value
// cannot escape the supported range.
void ReadEffectControls(apo::EffectInstance& effect)
{
    switch (effect.type) {
    case apo::EffectType::Input:
        GetSliderValue(g_inputGainSlider, effect.gain.gainDb);
        effect.gain.treatAsMono =
            SendMessageW(g_inputTreatMono, BM_GETCHECK, 0, 0) == BST_CHECKED;
        break;
    case apo::EffectType::Noise: {
        GetSliderValue(g_noiseReductionSlider, effect.noise.reductionDb);
        const int index = static_cast<int>(
            SendMessageW(g_noiseFftSizeCombo, CB_GETCURSEL, 0, 0));
        if (index >= 0 &&
            index < static_cast<int>(apo::kNoiseFftSizes.size())) {
            effect.noise.fftSize = apo::NoiseFftSizeAt(index);
        }
        break;
    }
    case apo::EffectType::Gate:
    case apo::EffectType::Compressor: {
        apo::DynamicsSettings& dynamics = DynamicsOf(effect);
        GetSliderValue(g_dynamicsInputSlider, dynamics.inputDb);
        GetSliderValue(g_dynamicsThresholdSlider, dynamics.thresholdDb);
        GetSliderValue(g_dynamicsRatioSlider, dynamics.ratio);
        GetSliderValue(g_dynamicsAttackSlider, dynamics.attackMs);
        GetSliderValue(g_dynamicsReleaseSlider, dynamics.releaseMs);
        GetSliderValue(g_dynamicsHoldSlider, dynamics.holdMs);
        GetSliderValue(g_dynamicsOutputSlider, dynamics.outputDb);
        GetSliderValue(g_dynamicsLookaheadSlider, dynamics.lookaheadMs);
        break;
    }
    case apo::EffectType::Equalizer: {
        const int mode = static_cast<int>(
            SendMessageW(g_eqDynamicCombo, CB_GETCURSEL, 0, 0));
        if (mode >= 0 &&
            mode < static_cast<int>(apo::EqualizerDynamicsMode::Count)) {
            effect.equalizer.mode =
                static_cast<apo::EqualizerDynamicsMode>(mode);
        }
        GetSliderValue(g_eqFrequencySlider, effect.equalizer.frequencyHz);
        GetSliderValue(g_eqQSlider, effect.equalizer.q);
        GetSliderValue(g_eqLevelSlider, effect.equalizer.levelDb);
        GetSliderValue(g_eqThresholdSlider, effect.equalizer.thresholdDb);
        GetSliderValue(g_eqRatioSlider, effect.equalizer.ratio);
        GetSliderValue(g_eqRangeSlider, effect.equalizer.rangeDb);
        GetSliderValue(g_eqAttackSlider, effect.equalizer.attackMs);
        GetSliderValue(g_eqReleaseSlider, effect.equalizer.releaseMs);
        GetSliderValue(g_eqHoldSlider, effect.equalizer.holdMs);
        GetSliderValue(g_eqLookaheadSlider, effect.equalizer.lookaheadMs);
        break;
    }
    case apo::EffectType::Gain:
        GetSliderValue(g_gainSlider, effect.gain.gainDb);
        break;
    case apo::EffectType::Saturation: {
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
    case apo::EffectType::Reverb: {
        const int algorithm = static_cast<int>(
            SendMessageW(g_reverbAlgorithmCombo, CB_GETCURSEL, 0, 0));
        if (algorithm >= 0 &&
            algorithm < static_cast<int>(apo::ReverbAlgorithm::Count)) {
            effect.reverb.algorithm =
                static_cast<apo::ReverbAlgorithm>(algorithm);
        }
        GetSliderValue(g_reverbLevelSlider, effect.reverb.level);
        double maxLengthSeconds = effect.reverb.maxLengthMs / 1000.0;
        GetSliderValue(g_reverbMaxLengthSlider, maxLengthSeconds);
        effect.reverb.maxLengthMs = maxLengthSeconds * 1000.0;
        GetSliderValue(g_reverbSizeSlider, effect.reverb.size);
        GetSliderValue(g_reverbDampingSlider, effect.reverb.damping);
        GetSliderValue(g_reverbWidthSlider, effect.reverb.width);
        GetSliderValue(g_reverbDensitySlider, effect.reverb.density);
        GetSliderValue(g_reverbPredelaySlider, effect.reverb.predelayMs);
        break;
    }
    case apo::EffectType::Echo: {
        const int mode = static_cast<int>(
            SendMessageW(g_echoModeCombo, CB_GETCURSEL, 0, 0));
        if (mode >= 0 && mode < static_cast<int>(apo::EchoMode::Count)) {
            effect.echo.mode = static_cast<apo::EchoMode>(mode);
        }
        GetSliderValue(g_echoTimeSlider, effect.echo.timeMs);
        GetSliderValue(g_echoFeedbackSlider, effect.echo.feedback);
        GetSliderValue(g_echoDampingSlider, effect.echo.damping);
        GetSliderValue(g_echoLevelSlider, effect.echo.level);
        break;
    }
    case apo::EffectType::Chorus: {
        double voices = static_cast<double>(effect.chorus.voices);
        GetSliderValue(g_chorusVoicesSlider, voices);
        effect.chorus.voices = static_cast<int>(voices + 0.5);
        GetSliderValue(g_chorusRateSlider, effect.chorus.rateHz);
        GetSliderValue(g_chorusDepthSlider, effect.chorus.depthMs);
        GetSliderValue(g_chorusWidthSlider, effect.chorus.width);
        GetSliderValue(g_chorusLevelSlider, effect.chorus.level);
        break;
    }
    case apo::EffectType::Tremolo: {
        const int waveform = static_cast<int>(
            SendMessageW(g_tremoloWaveformCombo, CB_GETCURSEL, 0, 0));
        if (waveform >= 0 &&
            waveform < static_cast<int>(apo::ModulationWaveform::Count)) {
            effect.tremolo.waveform =
                static_cast<apo::ModulationWaveform>(waveform);
        }
        GetSliderValue(g_tremoloRateSlider, effect.tremolo.rateHz);
        GetSliderValue(g_tremoloDepthSlider, effect.tremolo.depth);
        GetSliderValue(g_tremoloSpreadSlider, effect.tremolo.stereoSpread);
        break;
    }
    case apo::EffectType::Vibrato: {
        const int waveform = static_cast<int>(
            SendMessageW(g_vibratoWaveformCombo, CB_GETCURSEL, 0, 0));
        if (waveform >= 0 &&
            waveform < static_cast<int>(apo::ModulationWaveform::Count)) {
            effect.vibrato.waveform =
                static_cast<apo::ModulationWaveform>(waveform);
        }
        GetSliderValue(g_vibratoRateSlider, effect.vibrato.rateHz);
        GetSliderValue(g_vibratoDepthSlider, effect.vibrato.depthCents);
        GetSliderValue(g_vibratoSpreadSlider, effect.vibrato.stereoSpread);
        break;
    }
    case apo::EffectType::PitchShifter: {
        const int algorithm = static_cast<int>(
            SendMessageW(g_pitchAlgorithmCombo, CB_GETCURSEL, 0, 0));
        if (algorithm >= 0 &&
            algorithm < static_cast<int>(apo::PitchAlgorithm::Count)) {
            effect.pitchShifter.algorithm =
                static_cast<apo::PitchAlgorithm>(algorithm);
        }
        GetSliderValue(g_pitchShiftSlider,
                       effect.pitchShifter.shiftSemitones);
        double window =
            static_cast<double>(effect.pitchShifter.windowSamples);
        GetSliderValue(g_pitchWindowSlider, window);
        effect.pitchShifter.windowSamples = static_cast<int>(window + 0.5);
        GetSliderValue(g_pitchLevelSlider, effect.pitchShifter.level);
        GetSliderValue(g_pitchDryLevelSlider, effect.pitchShifter.dryLevel);
        break;
    }
    case apo::EffectType::Tuner: {
        const int algorithm = static_cast<int>(
            SendMessageW(g_tunerAlgorithmCombo, CB_GETCURSEL, 0, 0));
        if (algorithm >= 0 &&
            algorithm < static_cast<int>(apo::TunerAlgorithm::Count)) {
            effect.tuner.algorithm =
                static_cast<apo::TunerAlgorithm>(algorithm);
        }
        const int sizeIndex = static_cast<int>(
            SendMessageW(g_tunerSampleSizeCombo, CB_GETCURSEL, 0, 0));
        if (sizeIndex >= 0 &&
            sizeIndex < static_cast<int>(apo::kTunerSampleSizes.size())) {
            effect.tuner.sampleSize = apo::TunerSampleSizeAt(sizeIndex);
        }
        GetSliderValue(g_tunerSmoothingSlider, effect.tuner.smoothing);
        GetSliderValue(g_tunerReferenceSlider, effect.tuner.referenceHz);
        break;
    }
    case apo::EffectType::Looper:
        for (int track = 0; track < apo::kLooperTrackCount; ++track) {
            apo::LooperTrackSettings& settings =
                effect.looper.tracks[track];
            GetSliderValue(g_looperVolumeSliders[track], settings.volume);
            settings.freeLength =
                SendMessageW(g_looperFreeLength[track], BM_GETCHECK, 0, 0) ==
                BST_CHECKED;
            settings.stopAtLoopEnd =
                SendMessageW(g_looperStopAtEnd[track], BM_GETCHECK, 0, 0) ==
                BST_CHECKED;
        }
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
    SetSliderValue(g_inputGainSlider, effect.gain.gainDb);
    SendMessageW(g_inputTreatMono, BM_SETCHECK,
                 effect.gain.treatAsMono ? BST_CHECKED : BST_UNCHECKED, 0);
    SetSliderValue(g_noiseReductionSlider, effect.noise.reductionDb);
    SendMessageW(g_noiseFftSizeCombo, CB_SETCURSEL,
                 apo::NoiseFftSizeIndex(effect.noise.fftSize), 0);
    const apo::DynamicsSettings& dynamics = DynamicsOf(effect);
    SetSliderValue(g_dynamicsInputSlider, dynamics.inputDb);
    SetSliderValue(g_dynamicsThresholdSlider, dynamics.thresholdDb);
    SetSliderValue(g_dynamicsRatioSlider, dynamics.ratio);
    SetSliderValue(g_dynamicsAttackSlider, dynamics.attackMs);
    SetSliderValue(g_dynamicsReleaseSlider, dynamics.releaseMs);
    SetSliderValue(g_dynamicsHoldSlider, dynamics.holdMs);
    SetSliderValue(g_dynamicsOutputSlider, dynamics.outputDb);
    SetSliderValue(g_dynamicsLookaheadSlider, dynamics.lookaheadMs);
    SetSliderValue(g_gainSlider, effect.gain.gainDb);
    SendMessageW(g_eqDynamicCombo, CB_SETCURSEL,
                 static_cast<int>(effect.equalizer.mode), 0);
    SetSliderValue(g_eqFrequencySlider, effect.equalizer.frequencyHz);
    SetSliderValue(g_eqQSlider, effect.equalizer.q);
    SetSliderValue(g_eqLevelSlider, effect.equalizer.levelDb);
    SetSliderValue(g_eqThresholdSlider, effect.equalizer.thresholdDb);
    SetSliderValue(g_eqRatioSlider, effect.equalizer.ratio);
    SetSliderValue(g_eqRangeSlider, effect.equalizer.rangeDb);
    SetSliderValue(g_eqAttackSlider, effect.equalizer.attackMs);
    SetSliderValue(g_eqReleaseSlider, effect.equalizer.releaseMs);
    SetSliderValue(g_eqHoldSlider, effect.equalizer.holdMs);
    SetSliderValue(g_eqLookaheadSlider, effect.equalizer.lookaheadMs);
    SendMessageW(g_reverbAlgorithmCombo, CB_SETCURSEL,
                 static_cast<int>(effect.reverb.algorithm), 0);
    SetSliderValue(g_reverbLevelSlider, effect.reverb.level);
    SetSliderValue(g_reverbMaxLengthSlider,
                   effect.reverb.maxLengthMs / 1000.0);
    SetSliderValue(g_reverbSizeSlider, effect.reverb.size);
    SetSliderValue(g_reverbDampingSlider, effect.reverb.damping);
    SetSliderValue(g_reverbWidthSlider, effect.reverb.width);
    SetSliderValue(g_reverbDensitySlider, effect.reverb.density);
    SetSliderValue(g_reverbPredelaySlider, effect.reverb.predelayMs);
    SendMessageW(g_distFunctionCombo, CB_SETCURSEL,
                 effect.distortion.function, 0);
    SetSliderValue(g_distDriveSlider, effect.distortion.driveDb);
    SetSliderValue(g_distCurvatureSlider, effect.distortion.curvature);
    SetSliderValue(g_distOutputSlider, effect.distortion.outputDb);
    SendMessageW(g_echoModeCombo, CB_SETCURSEL,
                 static_cast<int>(effect.echo.mode), 0);
    SetSliderValue(g_echoTimeSlider, effect.echo.timeMs);
    SetSliderValue(g_echoFeedbackSlider, effect.echo.feedback);
    SetSliderValue(g_echoDampingSlider, effect.echo.damping);
    SetSliderValue(g_echoLevelSlider, effect.echo.level);
    SetSliderValue(g_chorusVoicesSlider,
                   static_cast<double>(effect.chorus.voices));
    SetSliderValue(g_chorusRateSlider, effect.chorus.rateHz);
    SetSliderValue(g_chorusDepthSlider, effect.chorus.depthMs);
    SetSliderValue(g_chorusWidthSlider, effect.chorus.width);
    SetSliderValue(g_chorusLevelSlider, effect.chorus.level);
    SendMessageW(g_tremoloWaveformCombo, CB_SETCURSEL,
                 static_cast<int>(effect.tremolo.waveform), 0);
    SetSliderValue(g_tremoloRateSlider, effect.tremolo.rateHz);
    SetSliderValue(g_tremoloDepthSlider, effect.tremolo.depth);
    SetSliderValue(g_tremoloSpreadSlider, effect.tremolo.stereoSpread);
    SendMessageW(g_vibratoWaveformCombo, CB_SETCURSEL,
                 static_cast<int>(effect.vibrato.waveform), 0);
    SetSliderValue(g_vibratoRateSlider, effect.vibrato.rateHz);
    SetSliderValue(g_vibratoDepthSlider, effect.vibrato.depthCents);
    SetSliderValue(g_vibratoSpreadSlider, effect.vibrato.stereoSpread);
    SendMessageW(g_pitchAlgorithmCombo, CB_SETCURSEL,
                 static_cast<int>(effect.pitchShifter.algorithm), 0);
    SetSliderValue(g_pitchShiftSlider, effect.pitchShifter.shiftSemitones);
    SetSliderValue(g_pitchWindowSlider,
                   static_cast<double>(effect.pitchShifter.windowSamples));
    SetSliderValue(g_pitchLevelSlider, effect.pitchShifter.level);
    SetSliderValue(g_pitchDryLevelSlider, effect.pitchShifter.dryLevel);
    SendMessageW(g_tunerAlgorithmCombo, CB_SETCURSEL,
                 static_cast<int>(effect.tuner.algorithm), 0);
    SendMessageW(g_tunerSampleSizeCombo, CB_SETCURSEL,
                 apo::TunerSampleSizeIndex(effect.tuner.sampleSize), 0);
    SetSliderValue(g_tunerSmoothingSlider, effect.tuner.smoothing);
    SetSliderValue(g_tunerReferenceSlider, effect.tuner.referenceHz);
    if (effect.type == apo::EffectType::Looper) {
        EnsureLooperLocalStates(g_endpoints[g_effectIndex], effect.id);
        for (int track = 0; track < apo::kLooperTrackCount; ++track) {
            const apo::LooperTrackSettings& settings =
                effect.looper.tracks[track];
            SetSliderValue(g_looperVolumeSliders[track], settings.volume);
            SendMessageW(g_looperFreeLength[track], BM_SETCHECK,
                         settings.freeLength ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_looperStopAtEnd[track], BM_SETCHECK,
                         settings.stopAtLoopEnd ? BST_CHECKED : BST_UNCHECKED,
                         0);
            SetWindowTextW(g_looperStateLabels[track],
                           apo::LooperStateName(g_looperLocalStates[track]));
        }
    }

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
              L"Audio keeps playing."
            : L"Stay quiet for ~3 s while capturing.\r\n"
              L"Audio keeps playing.");
    g_effectLoading = false;
    RefreshEffectGraph();
    Relayout();
    ApplyEqualizerDynamicEnable(!g_busy && ValidEffectIndex());
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
        case apo::EffectType::Input:
            SetSliderValue(g_inputGainSlider, shown.gain.gainDb);
            SendMessageW(g_inputTreatMono, BM_SETCHECK,
                         shown.gain.treatAsMono ? BST_CHECKED : BST_UNCHECKED,
                         0);
            break;
        case apo::EffectType::Noise:
            SetSliderValue(g_noiseReductionSlider, shown.noise.reductionDb);
            break;
        case apo::EffectType::Gate:
        case apo::EffectType::Compressor: {
            const apo::DynamicsSettings& dynamics = DynamicsOf(shown);
            SetSliderValue(g_dynamicsInputSlider, dynamics.inputDb);
            SetSliderValue(g_dynamicsThresholdSlider, dynamics.thresholdDb);
            SetSliderValue(g_dynamicsRatioSlider, dynamics.ratio);
            SetSliderValue(g_dynamicsAttackSlider, dynamics.attackMs);
            SetSliderValue(g_dynamicsReleaseSlider, dynamics.releaseMs);
            SetSliderValue(g_dynamicsHoldSlider, dynamics.holdMs);
            SetSliderValue(g_dynamicsOutputSlider, dynamics.outputDb);
            SetSliderValue(g_dynamicsLookaheadSlider, dynamics.lookaheadMs);
            break;
        }
        case apo::EffectType::Equalizer:
            SendMessageW(g_eqDynamicCombo, CB_SETCURSEL,
                         static_cast<int>(shown.equalizer.mode), 0);
            SetSliderValue(g_eqFrequencySlider, shown.equalizer.frequencyHz);
            SetSliderValue(g_eqQSlider, shown.equalizer.q);
            SetSliderValue(g_eqLevelSlider, shown.equalizer.levelDb);
            SetSliderValue(g_eqThresholdSlider,
                           shown.equalizer.thresholdDb);
            SetSliderValue(g_eqRatioSlider, shown.equalizer.ratio);
            SetSliderValue(g_eqRangeSlider, shown.equalizer.rangeDb);
            SetSliderValue(g_eqAttackSlider, shown.equalizer.attackMs);
            SetSliderValue(g_eqReleaseSlider, shown.equalizer.releaseMs);
            SetSliderValue(g_eqHoldSlider, shown.equalizer.holdMs);
            SetSliderValue(g_eqLookaheadSlider,
                           shown.equalizer.lookaheadMs);
            break;
        case apo::EffectType::Gain:
            SetSliderValue(g_gainSlider, shown.gain.gainDb);
            break;
        case apo::EffectType::Saturation:
            SendMessageW(g_distFunctionCombo, CB_SETCURSEL,
                         shown.distortion.function, 0);
            SetSliderValue(g_distDriveSlider, shown.distortion.driveDb);
            SetSliderValue(g_distCurvatureSlider, shown.distortion.curvature);
            SetSliderValue(g_distOutputSlider, shown.distortion.outputDb);
            break;
        case apo::EffectType::Reverb:
            SendMessageW(g_reverbAlgorithmCombo, CB_SETCURSEL,
                         static_cast<int>(shown.reverb.algorithm), 0);
            SetSliderValue(g_reverbLevelSlider, shown.reverb.level);
            SetSliderValue(g_reverbMaxLengthSlider,
                           shown.reverb.maxLengthMs / 1000.0);
            SetSliderValue(g_reverbSizeSlider, shown.reverb.size);
            SetSliderValue(g_reverbDampingSlider, shown.reverb.damping);
            SetSliderValue(g_reverbWidthSlider, shown.reverb.width);
            SetSliderValue(g_reverbDensitySlider, shown.reverb.density);
            SetSliderValue(g_reverbPredelaySlider, shown.reverb.predelayMs);
            break;
        case apo::EffectType::Echo:
            SendMessageW(g_echoModeCombo, CB_SETCURSEL,
                         static_cast<int>(shown.echo.mode), 0);
            SetSliderValue(g_echoTimeSlider, shown.echo.timeMs);
            SetSliderValue(g_echoFeedbackSlider, shown.echo.feedback);
            SetSliderValue(g_echoDampingSlider, shown.echo.damping);
            SetSliderValue(g_echoLevelSlider, shown.echo.level);
            break;
        case apo::EffectType::Chorus:
            SetSliderValue(g_chorusVoicesSlider,
                           static_cast<double>(shown.chorus.voices));
            SetSliderValue(g_chorusRateSlider, shown.chorus.rateHz);
            SetSliderValue(g_chorusDepthSlider, shown.chorus.depthMs);
            SetSliderValue(g_chorusWidthSlider, shown.chorus.width);
            SetSliderValue(g_chorusLevelSlider, shown.chorus.level);
            break;
        case apo::EffectType::Tremolo:
            SendMessageW(g_tremoloWaveformCombo, CB_SETCURSEL,
                         static_cast<int>(shown.tremolo.waveform), 0);
            SetSliderValue(g_tremoloRateSlider, shown.tremolo.rateHz);
            SetSliderValue(g_tremoloDepthSlider, shown.tremolo.depth);
            SetSliderValue(g_tremoloSpreadSlider,
                           shown.tremolo.stereoSpread);
            break;
        case apo::EffectType::Vibrato:
            SendMessageW(g_vibratoWaveformCombo, CB_SETCURSEL,
                         static_cast<int>(shown.vibrato.waveform), 0);
            SetSliderValue(g_vibratoRateSlider, shown.vibrato.rateHz);
            SetSliderValue(g_vibratoDepthSlider, shown.vibrato.depthCents);
            SetSliderValue(g_vibratoSpreadSlider,
                           shown.vibrato.stereoSpread);
            break;
        case apo::EffectType::PitchShifter:
            SendMessageW(g_pitchAlgorithmCombo, CB_SETCURSEL,
                         static_cast<int>(shown.pitchShifter.algorithm), 0);
            SetSliderValue(g_pitchShiftSlider,
                           shown.pitchShifter.shiftSemitones);
            SetSliderValue(
                g_pitchWindowSlider,
                static_cast<double>(shown.pitchShifter.windowSamples));
            SetSliderValue(g_pitchLevelSlider, shown.pitchShifter.level);
            SetSliderValue(g_pitchDryLevelSlider,
                           shown.pitchShifter.dryLevel);
            break;
        case apo::EffectType::Tuner:
            SendMessageW(g_tunerAlgorithmCombo, CB_SETCURSEL,
                         static_cast<int>(shown.tuner.algorithm), 0);
            SendMessageW(g_tunerSampleSizeCombo, CB_SETCURSEL,
                         apo::TunerSampleSizeIndex(shown.tuner.sampleSize), 0);
            SetSliderValue(g_tunerSmoothingSlider, shown.tuner.smoothing);
            SetSliderValue(g_tunerReferenceSlider, shown.tuner.referenceHz);
            break;
        case apo::EffectType::Looper:
            for (int track = 0; track < apo::kLooperTrackCount; ++track) {
                const apo::LooperTrackSettings& trackSettings =
                    shown.looper.tracks[track];
                SetSliderValue(g_looperVolumeSliders[track],
                               trackSettings.volume);
                SendMessageW(
                    g_looperFreeLength[track], BM_SETCHECK,
                    trackSettings.freeLength ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(g_looperStopAtEnd[track], BM_SETCHECK,
                             trackSettings.stopAtLoopEnd ? BST_CHECKED
                                                         : BST_UNCHECKED,
                             0);
                SetWindowTextW(
                    g_looperStateLabels[track],
                    apo::LooperStateName(g_looperLocalStates[track]));
            }
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
    if (std::isinf(left) || std::isinf(right)) {
        return left == right;
    }
    return std::abs(left - right) < 1e-6;
}

bool ParameterAtDefault(const apo::EffectInstance& effect,
                        const apo::EffectInstance& defaults, int param)
{
    const apo::DynamicsSettings& dynamics = DynamicsOf(effect);
    const apo::DynamicsSettings& defaultDynamics = DynamicsOf(defaults);
    switch (param) {
    case ParamResetNoiseReduction:
        return SameValue(effect.noise.reductionDb, defaults.noise.reductionDb);
    case ParamResetNoiseFftSize:
        return effect.noise.fftSize == defaults.noise.fftSize;
    case ParamResetDynamicsInput:
        return SameValue(dynamics.inputDb, defaultDynamics.inputDb);
    case ParamResetDynamicsThreshold:
        return SameValue(dynamics.thresholdDb, defaultDynamics.thresholdDb);
    case ParamResetDynamicsRatio:
        return SameValue(dynamics.ratio, defaultDynamics.ratio);
    case ParamResetDynamicsAttack:
        return SameValue(dynamics.attackMs, defaultDynamics.attackMs);
    case ParamResetDynamicsRelease:
        return SameValue(dynamics.releaseMs, defaultDynamics.releaseMs);
    case ParamResetDynamicsHold:
        return SameValue(dynamics.holdMs, defaultDynamics.holdMs);
    case ParamResetDynamicsOutput:
        return SameValue(dynamics.outputDb, defaultDynamics.outputDb);
    case ParamResetDynamicsLookahead:
        return SameValue(dynamics.lookaheadMs, defaultDynamics.lookaheadMs);
    case ParamResetInputGain:
        return SameValue(effect.gain.gainDb, defaults.gain.gainDb);
    case ParamResetGain:
        return SameValue(effect.gain.gainDb, defaults.gain.gainDb);
    case ParamResetDynMode:
        return effect.equalizer.mode == defaults.equalizer.mode;
    case ParamResetDynFrequency:
        return SameValue(effect.equalizer.frequencyHz,
                         defaults.equalizer.frequencyHz);
    case ParamResetDynQ:
        return SameValue(effect.equalizer.q, defaults.equalizer.q);
    case ParamResetDynLevel:
        return SameValue(effect.equalizer.levelDb, defaults.equalizer.levelDb);
    case ParamResetDynThreshold:
        return SameValue(effect.equalizer.thresholdDb,
                         defaults.equalizer.thresholdDb);
    case ParamResetDynRatio:
        return SameValue(effect.equalizer.ratio, defaults.equalizer.ratio);
    case ParamResetDynRange:
        return SameValue(effect.equalizer.rangeDb, defaults.equalizer.rangeDb);
    case ParamResetDynAttack:
        return SameValue(effect.equalizer.attackMs,
                         defaults.equalizer.attackMs);
    case ParamResetDynRelease:
        return SameValue(effect.equalizer.releaseMs,
                         defaults.equalizer.releaseMs);
    case ParamResetDynHold:
        return SameValue(effect.equalizer.holdMs, defaults.equalizer.holdMs);
    case ParamResetDynLookahead:
        return SameValue(effect.equalizer.lookaheadMs,
                         defaults.equalizer.lookaheadMs);
    case ParamResetReverbAlgorithm:
        return effect.reverb.algorithm == defaults.reverb.algorithm;
    case ParamResetReverbLevel:
        return SameValue(effect.reverb.level, defaults.reverb.level);
    case ParamResetReverbMaxLength:
        return SameValue(effect.reverb.maxLengthMs,
                         defaults.reverb.maxLengthMs);
    case ParamResetReverbSize:
        return SameValue(effect.reverb.size, defaults.reverb.size);
    case ParamResetReverbDamping:
        return SameValue(effect.reverb.damping, defaults.reverb.damping);
    case ParamResetReverbWidth:
        return SameValue(effect.reverb.width, defaults.reverb.width);
    case ParamResetReverbDensity:
        return SameValue(effect.reverb.density, defaults.reverb.density);
    case ParamResetReverbPredelay:
        return SameValue(effect.reverb.predelayMs, defaults.reverb.predelayMs);
    case ParamResetEchoMode:
        return effect.echo.mode == defaults.echo.mode;
    case ParamResetEchoTime:
        return SameValue(effect.echo.timeMs, defaults.echo.timeMs);
    case ParamResetEchoFeedback:
        return SameValue(effect.echo.feedback, defaults.echo.feedback);
    case ParamResetEchoDamping:
        return SameValue(effect.echo.damping, defaults.echo.damping);
    case ParamResetEchoLevel:
        return SameValue(effect.echo.level, defaults.echo.level);
    case ParamResetChorusVoices:
        return effect.chorus.voices == defaults.chorus.voices;
    case ParamResetChorusRate:
        return SameValue(effect.chorus.rateHz, defaults.chorus.rateHz);
    case ParamResetChorusDepth:
        return SameValue(effect.chorus.depthMs, defaults.chorus.depthMs);
    case ParamResetChorusWidth:
        return SameValue(effect.chorus.width, defaults.chorus.width);
    case ParamResetChorusLevel:
        return SameValue(effect.chorus.level, defaults.chorus.level);
    case ParamResetTremoloRate:
        return SameValue(effect.tremolo.rateHz, defaults.tremolo.rateHz);
    case ParamResetTremoloDepth:
        return SameValue(effect.tremolo.depth, defaults.tremolo.depth);
    case ParamResetTremoloWaveform:
        return effect.tremolo.waveform == defaults.tremolo.waveform;
    case ParamResetTremoloSpread:
        return SameValue(effect.tremolo.stereoSpread,
                         defaults.tremolo.stereoSpread);
    case ParamResetVibratoRate:
        return SameValue(effect.vibrato.rateHz, defaults.vibrato.rateHz);
    case ParamResetVibratoDepth:
        return SameValue(effect.vibrato.depthCents,
                         defaults.vibrato.depthCents);
    case ParamResetVibratoWaveform:
        return effect.vibrato.waveform == defaults.vibrato.waveform;
    case ParamResetVibratoSpread:
        return SameValue(effect.vibrato.stereoSpread,
                         defaults.vibrato.stereoSpread);
    case ParamResetPitchAlgorithm:
        return effect.pitchShifter.algorithm ==
               defaults.pitchShifter.algorithm;
    case ParamResetPitchShift:
        return SameValue(effect.pitchShifter.shiftSemitones,
                         defaults.pitchShifter.shiftSemitones);
    case ParamResetPitchWindow:
        return effect.pitchShifter.windowSamples ==
               defaults.pitchShifter.windowSamples;
    case ParamResetPitchLevel:
        return SameValue(effect.pitchShifter.level,
                         defaults.pitchShifter.level);
    case ParamResetPitchDryLevel:
        return SameValue(effect.pitchShifter.dryLevel,
                         defaults.pitchShifter.dryLevel);
    case ParamResetDistFunction:
        return effect.distortion.function == defaults.distortion.function;
    case ParamResetDistDrive:
        return SameValue(effect.distortion.driveDb, defaults.distortion.driveDb);
    case ParamResetDistCurvature:
        return SameValue(effect.distortion.curvature,
                         defaults.distortion.curvature);
    case ParamResetDistOutput:
        return SameValue(effect.distortion.outputDb, defaults.distortion.outputDb);
    case ParamResetTunerAlgorithm:
        return effect.tuner.algorithm == defaults.tuner.algorithm;
    case ParamResetTunerSampleSize:
        return effect.tuner.sampleSize == defaults.tuner.sampleSize;
    case ParamResetTunerSmoothing:
        return SameValue(effect.tuner.smoothing, defaults.tuner.smoothing);
    case ParamResetTunerReference:
        return SameValue(effect.tuner.referenceHz, defaults.tuner.referenceHz);
    default:
        if (param >= ParamResetLooperVolume1 &&
            param <= ParamResetLooperStop5)
        {
            const int local = param - ParamResetLooperVolume1;
            const int track = local / 3;
            const int kind = local % 3;
            if (track >= 0 && track < apo::kLooperTrackCount) {
                const apo::LooperTrackSettings& value =
                    effect.looper.tracks[track];
                const apo::LooperTrackSettings& initial =
                    defaults.looper.tracks[track];
                switch (kind) {
                case 0:
                    return SameValue(value.volume, initial.volume);
                case 1:
                    return value.freeLength == initial.freeLength;
                case 2:
                    return value.stopAtLoopEnd == initial.stopAtLoopEnd;
                default:
                    break;
                }
            }
        }
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
    const bool eqDynamicOff =
        effect.type == apo::EffectType::Equalizer &&
        effect.equalizer.mode == apo::EqualizerDynamicsMode::None;
    for (int param = 0; param < ParamResetCount; ++param) {
        bool enable = !ParameterAtDefault(effect, defaults, param);
        if (eqDynamicOff && param >= ParamResetDynThreshold &&
            param <= ParamResetDynLookahead) {
            enable = false;
        }
        EnableWindow(g_paramResetButtons[param], enable);
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
    apo::DynamicsSettings& dynamics = DynamicsOf(effect);
    const apo::DynamicsSettings& defaultDynamics = DynamicsOf(defaults);

    switch (param) {
    case ParamResetNoiseReduction:
        effect.noise.reductionDb = defaults.noise.reductionDb;
        break;
    case ParamResetNoiseFftSize:
        effect.noise.fftSize = defaults.noise.fftSize;
        break;
    case ParamResetDynamicsInput:
        dynamics.inputDb = defaultDynamics.inputDb;
        break;
    case ParamResetDynamicsThreshold:
        dynamics.thresholdDb = defaultDynamics.thresholdDb;
        break;
    case ParamResetDynamicsRatio:
        dynamics.ratio = defaultDynamics.ratio;
        break;
    case ParamResetDynamicsAttack:
        dynamics.attackMs = defaultDynamics.attackMs;
        break;
    case ParamResetDynamicsRelease:
        dynamics.releaseMs = defaultDynamics.releaseMs;
        break;
    case ParamResetDynamicsHold:
        dynamics.holdMs = defaultDynamics.holdMs;
        break;
    case ParamResetDynamicsOutput:
        dynamics.outputDb = defaultDynamics.outputDb;
        break;
    case ParamResetDynamicsLookahead:
        dynamics.lookaheadMs = defaultDynamics.lookaheadMs;
        break;
    case ParamResetInputGain:
        effect.gain.gainDb = defaults.gain.gainDb;
        break;
    case ParamResetGain:
        effect.gain.gainDb = defaults.gain.gainDb;
        break;
    case ParamResetDynMode:
        effect.equalizer.mode = defaults.equalizer.mode;
        break;
    case ParamResetDynFrequency:
        effect.equalizer.frequencyHz = defaults.equalizer.frequencyHz;
        break;
    case ParamResetDynQ:
        effect.equalizer.q = defaults.equalizer.q;
        break;
    case ParamResetDynLevel:
        effect.equalizer.levelDb = defaults.equalizer.levelDb;
        break;
    case ParamResetDynThreshold:
        effect.equalizer.thresholdDb = defaults.equalizer.thresholdDb;
        break;
    case ParamResetDynRatio:
        effect.equalizer.ratio = defaults.equalizer.ratio;
        break;
    case ParamResetDynRange:
        effect.equalizer.rangeDb = defaults.equalizer.rangeDb;
        break;
    case ParamResetDynAttack:
        effect.equalizer.attackMs = defaults.equalizer.attackMs;
        break;
    case ParamResetDynRelease:
        effect.equalizer.releaseMs = defaults.equalizer.releaseMs;
        break;
    case ParamResetDynHold:
        effect.equalizer.holdMs = defaults.equalizer.holdMs;
        break;
    case ParamResetDynLookahead:
        effect.equalizer.lookaheadMs = defaults.equalizer.lookaheadMs;
        break;
    case ParamResetReverbAlgorithm:
        effect.reverb.algorithm = defaults.reverb.algorithm;
        break;
    case ParamResetReverbLevel:
        effect.reverb.level = defaults.reverb.level;
        break;
    case ParamResetReverbMaxLength:
        effect.reverb.maxLengthMs = defaults.reverb.maxLengthMs;
        break;
    case ParamResetReverbSize:
        effect.reverb.size = defaults.reverb.size;
        break;
    case ParamResetReverbDamping:
        effect.reverb.damping = defaults.reverb.damping;
        break;
    case ParamResetReverbWidth:
        effect.reverb.width = defaults.reverb.width;
        break;
    case ParamResetReverbDensity:
        effect.reverb.density = defaults.reverb.density;
        break;
    case ParamResetReverbPredelay:
        effect.reverb.predelayMs = defaults.reverb.predelayMs;
        break;
    case ParamResetEchoMode:
        effect.echo.mode = defaults.echo.mode;
        break;
    case ParamResetEchoTime:
        effect.echo.timeMs = defaults.echo.timeMs;
        break;
    case ParamResetEchoFeedback:
        effect.echo.feedback = defaults.echo.feedback;
        break;
    case ParamResetEchoDamping:
        effect.echo.damping = defaults.echo.damping;
        break;
    case ParamResetEchoLevel:
        effect.echo.level = defaults.echo.level;
        break;
    case ParamResetChorusVoices:
        effect.chorus.voices = defaults.chorus.voices;
        break;
    case ParamResetChorusRate:
        effect.chorus.rateHz = defaults.chorus.rateHz;
        break;
    case ParamResetChorusDepth:
        effect.chorus.depthMs = defaults.chorus.depthMs;
        break;
    case ParamResetChorusWidth:
        effect.chorus.width = defaults.chorus.width;
        break;
    case ParamResetChorusLevel:
        effect.chorus.level = defaults.chorus.level;
        break;
    case ParamResetTremoloRate:
        effect.tremolo.rateHz = defaults.tremolo.rateHz;
        break;
    case ParamResetTremoloDepth:
        effect.tremolo.depth = defaults.tremolo.depth;
        break;
    case ParamResetTremoloWaveform:
        effect.tremolo.waveform = defaults.tremolo.waveform;
        break;
    case ParamResetTremoloSpread:
        effect.tremolo.stereoSpread = defaults.tremolo.stereoSpread;
        break;
    case ParamResetVibratoRate:
        effect.vibrato.rateHz = defaults.vibrato.rateHz;
        break;
    case ParamResetVibratoDepth:
        effect.vibrato.depthCents = defaults.vibrato.depthCents;
        break;
    case ParamResetVibratoWaveform:
        effect.vibrato.waveform = defaults.vibrato.waveform;
        break;
    case ParamResetVibratoSpread:
        effect.vibrato.stereoSpread = defaults.vibrato.stereoSpread;
        break;
    case ParamResetPitchAlgorithm:
        effect.pitchShifter.algorithm = defaults.pitchShifter.algorithm;
        break;
    case ParamResetPitchShift:
        effect.pitchShifter.shiftSemitones =
            defaults.pitchShifter.shiftSemitones;
        break;
    case ParamResetPitchWindow:
        effect.pitchShifter.windowSamples =
            defaults.pitchShifter.windowSamples;
        break;
    case ParamResetPitchLevel:
        effect.pitchShifter.level = defaults.pitchShifter.level;
        break;
    case ParamResetPitchDryLevel:
        effect.pitchShifter.dryLevel = defaults.pitchShifter.dryLevel;
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
    case ParamResetTunerAlgorithm:
        effect.tuner.algorithm = defaults.tuner.algorithm;
        break;
    case ParamResetTunerSampleSize:
        effect.tuner.sampleSize = defaults.tuner.sampleSize;
        break;
    case ParamResetTunerSmoothing:
        effect.tuner.smoothing = defaults.tuner.smoothing;
        break;
    case ParamResetTunerReference:
        effect.tuner.referenceHz = defaults.tuner.referenceHz;
        break;
    default:
        if (param >= ParamResetLooperVolume1 &&
            param <= ParamResetLooperStop5)
        {
            const int local = param - ParamResetLooperVolume1;
            const int track = local / 3;
            const int kind = local % 3;
            if (track >= 0 && track < apo::kLooperTrackCount) {
                apo::LooperTrackSettings& value =
                    effect.looper.tracks[track];
                const apo::LooperTrackSettings& initial =
                    defaults.looper.tracks[track];
                switch (kind) {
                case 0:
                    value.volume = initial.volume;
                    break;
                case 1:
                    value.freeLength = initial.freeLength;
                    break;
                case 2:
                    value.stopAtLoopEnd = initial.stopAtLoopEnd;
                    break;
                default:
                    return;
                }
                break;
            }
        }
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
    if (type == apo::EffectType::Input) {
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
    // Input owns row 0, so an inserted effect can never displace it.
    const int insertAt = std::max(
        1, g_effectSelection >= 0 && g_effectSelection < preset.effectCount
               ? g_effectSelection + 1
               : preset.effectCount);
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
    if (row <= 0 || row >= preset.effectCount) {
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

// Transport requests ride in the runtime snapshot, not the durable INI file.
// The serial changes on every gesture so the APO can consume a one-shot command
// at a buffer boundary; a later settings-only publish carries the same serial
// with the command already cleared and cannot replay it.
bool PublishLooperCommand(apo::LooperCommand command, int track)
{
    if (g_busy || g_effectLoading || !HasEffectSelection() ||
        g_effectType != apo::EffectType::Looper ||
        track < 0 || track >= apo::kLooperTrackCount ||
        command == apo::LooperCommand::None)
    {
        return false;
    }
    if (!CommitEffectEditor(false)) {
        return false;
    }

    apo::Endpoint& endpoint = g_endpoints[g_effectIndex];
    if (!endpoint.Enabled()) {
        apo::ApoSettings durable = endpoint.settings;
        apo::LooperSettings& looper =
            apo::ActivePreset(durable).effects[g_effectSelection].looper;
        looper.command = apo::LooperCommand::None;
        looper.commandSerial = 0;
        looper.commandTrack = 0;
        if (!apo::SaveSettings(endpoint.id, durable)) {
            SetWindowTextW(g_status, L"Could not save the settings file.");
            return false;
        }
        endpoint.settings = durable;
        endpoint.settingsDirty = false;
        UpdateEffectsUi();
        SetWindowTextW(g_status, L"Enable this effect to use the looper.");
        return false;
    }
    if (!apo::RuntimeSettingsFileReady(endpoint.id)) {
        SetWindowTextW(g_status, L"Apply the effect before using the looper.");
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    g_looperCommandSerial =
        (now > g_looperCommandSerial) ? now : g_looperCommandSerial + 1;
    apo::ApoSettings published = endpoint.settings;
    apo::LooperSettings& live =
        apo::ActivePreset(published).effects[g_effectSelection].looper;
    live.command = command;
    live.commandTrack = track;
    live.commandSerial = g_looperCommandSerial;
    apo::ClampSettings(published);

    if (!apo::WriteRuntimeSettings(endpoint.id, published)) {
        SetWindowTextW(g_status, L"Could not send the looper command.");
        return false;
    }

    apo::ApoSettings durable = published;
    apo::LooperSettings& stored =
        apo::ActivePreset(durable).effects[g_effectSelection].looper;
    stored.command = apo::LooperCommand::None;
    stored.commandSerial = 0;
    stored.commandTrack = 0;
    apo::ClampSettings(durable);

    // Keep the serial in memory so a later settings-only publish has the same
    // generation and cannot replay this command.
    apo::LooperSettings& remembered =
        apo::ActivePreset(published).effects[g_effectSelection].looper;
    remembered.command = apo::LooperCommand::None;
    remembered.commandTrack = 0;
    apo::ClampSettings(published);
    endpoint.settings = published;
    KillTimer(g_window, kLiveApplyTimer);
    g_liveApplyPending = false;
    if (!apo::SaveSettings(endpoint.id, durable)) {
        endpoint.settingsDirty = true;
        SetWindowTextW(
            g_status,
            L"Looper command sent, but the settings file could not be saved.");
    } else {
        endpoint.settingsDirty = false;
        SetWindowTextW(g_status, L"Looper command sent.");
    }
    UpdateEffectsUi();
    return true;
}

struct LooperGestureResult
{
    apo::LooperCommand command = apo::LooperCommand::None;
    apo::LooperState next = apo::LooperState::Empty;
};

LooperGestureResult ResolveLooperGesture(apo::LooperState state,
                                         LooperGesture gesture, int track)
{
    const auto checked = [](HWND control) {
        return control != nullptr &&
               SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    };
    const bool stopAtEnd = checked(g_looperStopAtEnd[track]);
    const bool freeLength = checked(g_looperFreeLength[track]);

    LooperGestureResult result{apo::LooperCommand::None, state};
    switch (gesture) {
    case LooperGesture::Tap:
        switch (state) {
        case apo::LooperState::Empty:
            result.command = apo::LooperCommand::StartRecord;
            result.next = apo::LooperState::Recording;
            break;
        case apo::LooperState::Recording:
            result.command = apo::LooperCommand::FinishRecord;
            result.next = apo::LooperState::Playback;
            break;
        case apo::LooperState::Playback:
            result.command = apo::LooperCommand::StartRecord;
            result.next = apo::LooperState::Recording;
            break;
        case apo::LooperState::Stopped:
            result.command = apo::LooperCommand::Play;
            result.next = apo::LooperState::Playback;
            break;
        }
        break;

    case LooperGesture::DoubleTap:
        if (state == apo::LooperState::Stopped) {
            result.command = apo::LooperCommand::Play;
            result.next = apo::LooperState::Playback;
        } else if (state == apo::LooperState::Recording ||
                   state == apo::LooperState::Playback)
        {
            result.command = apo::LooperCommand::Stop;
            const bool defer = stopAtEnd &&
                               (state == apo::LooperState::Playback ||
                                !freeLength);
            result.next = defer ? state : apo::LooperState::Stopped;
        }
        break;

    case LooperGesture::LongPress:
        if (state == apo::LooperState::Recording) {
            result.command = apo::LooperCommand::FinishRecord;
            result.next = apo::LooperState::Playback;
        } else if (state == apo::LooperState::Playback) {
            result.command = apo::LooperCommand::Undo;
            if (g_looperLocalUndoValid && g_looperLocalUndoTrack == track) {
                result.next = g_looperLocalUndoState == apo::LooperState::Empty
                                  ? apo::LooperState::Empty
                                  : apo::LooperState::Playback;
            }
        } else if (state == apo::LooperState::Stopped) {
            result.command = apo::LooperCommand::Clear;
            result.next = apo::LooperState::Empty;
        }
        break;
    }
    return result;
}

void UpdateLooperUndoAfterCommand(apo::LooperCommand command, int track,
                                  apo::LooperState previousState)
{
    if (command == apo::LooperCommand::StartRecord ||
        command == apo::LooperCommand::StartRecordProvisional)
    {
        g_looperLocalUndoState = previousState;
        g_looperLocalUndoTrack = track;
        g_looperLocalUndoValid = true;
    } else if (command == apo::LooperCommand::Undo ||
               (command == apo::LooperCommand::Clear &&
                g_looperLocalUndoTrack == track))
    {
        g_looperLocalUndoValid = false;
        g_looperLocalUndoTrack = -1;
    }
}

void ApplyLooperGesture(int track, LooperGesture gesture)
{
    if (g_busy || g_effectLoading || !HasEffectSelection() ||
        g_effectType != apo::EffectType::Looper ||
        track < 0 || track >= apo::kLooperTrackCount)
    {
        return;
    }

    EnsureLooperLocalStates(g_endpoints[g_effectIndex],
                            apo::ActivePreset(g_endpoints[g_effectIndex].settings)
                                .effects[g_effectSelection]
                                .id);
    const apo::LooperState state = g_looperLocalStates[track];
    const LooperGestureResult result = ResolveLooperGesture(state, gesture, track);
    if (result.command == apo::LooperCommand::None ||
        !PublishLooperCommand(result.command, track))
    {
        return;
    }

    UpdateLooperUndoAfterCommand(result.command, track, state);
    SetLooperLocalState(track, result.next);
}

// Keyboard presses start a take before the double-tap window closes. If the
// gesture later resolves as a double tap or long press, the APO discards that
// provisional take before applying the real command, so this local state must
// be calculated from the state the gesture started from.
void ApplyLooperGestureFromState(int track, apo::LooperState originalState,
                                 LooperGesture gesture)
{
    const LooperGestureResult result =
        ResolveLooperGesture(originalState, gesture, track);
    const apo::LooperCommand command =
        result.command == apo::LooperCommand::None
            ? apo::LooperCommand::DiscardProvisional
            : result.command;
    if (!PublishLooperCommand(command, track)) {
        return;
    }
    if (result.command != apo::LooperCommand::None) {
        UpdateLooperUndoAfterCommand(result.command, track, originalState);
    }
    SetLooperLocalState(track, result.next);
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

// Reads the APO's per-stage telemetry without touching settings, bindings or
// the audition stream. The APO is the only writer; if its channel is not ready
// yet, retry on the next tick and keep the graph cleared.
void PollStageMonitor()
{
    if (g_effectGraph == nullptr) {
        return;
    }
    const bool unavailable = !ValidEffectIndex() || !HasEffectSelection() ||
                             !g_endpoints[g_effectIndex].Enabled();
    if (g_busy || unavailable) {
        g_stageMonitor.Close();
        ui::ClearGraphMonitor(g_effectGraph);
        if (unavailable) {
            // A disabled/unbound endpoint has no live DSP state. Noise capture
            // and other busy work do not: those keep the generation baseline.
            g_looperMonitorMissingSince = 0;
            g_lastLooperMonitorGeneration = 0;
            g_lastLooperMonitorEndpointId.clear();
            ResetLooperLocalStates();
        }
        return;
    }

    const apo::Endpoint& endpoint = g_endpoints[g_effectIndex];
    if (g_lastLooperMonitorEndpointId != endpoint.id) {
        g_lastLooperMonitorEndpointId = endpoint.id;
        g_lastLooperMonitorGeneration = 0;
        g_looperMonitorMissingSince = 0;
    }
    if (g_stageMonitor.EndpointId() != endpoint.id) {
        g_stageMonitor.Open(endpoint.id, false);
    }

    // The APO publishes a new generation each time it locks for a new stream.
    // Takes are now restored from the Looper bank, so cancel any half-finished
    // gesture but let the snapshot below publish the real restored states.
    // Settings-only applies keep the same generation and same state.
    const ULONG generation = g_stageMonitor.Generation();
    if (generation != 0 && generation != g_lastLooperMonitorGeneration) {
        g_lastLooperMonitorGeneration = generation;
        g_looperMonitorMissingSince = 0;
        CancelLooperKeyGestures();
    }

    const apo::EffectInstance& effect =
        apo::ActivePreset(endpoint.settings).effects[g_effectSelection];
    apo::StageMonitorSnapshot snapshot;
    if (g_stageMonitor.EndpointId() == endpoint.id &&
        g_stageMonitor.Find(effect.id, effect.type, snapshot)) {
        g_looperMonitorMissingSince = 0;
        if (effect.type == apo::EffectType::Looper) {
            for (int track = 0; track < apo::kLooperTrackCount; ++track) {
                const int value = snapshot.looperStates[track];
                if (value >= static_cast<int>(apo::LooperState::Empty) &&
                    value <= static_cast<int>(apo::LooperState::Stopped))
                {
                    SetLooperLocalState(
                        track, static_cast<apo::LooperState>(value));
                }
            }
        }
        ui::SetGraphMonitor(g_effectGraph, snapshot);
    } else {
        ui::ClearGraphMonitor(g_effectGraph);
        // A missing monitor can also mean the APO died without publishing a
        // new generation. Treat a sustained absence as a real restart, but
        // tolerate a short gap while the audio engine settles.
        const DWORD now = GetTickCount();
        if (g_looperMonitorMissingSince == 0) {
            g_looperMonitorMissingSince = now;
        } else if (now - g_looperMonitorMissingSince >= 1000) {
            ResetLooperLocalStates();
        }
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
    apo::LogListenEvent(L"StopListening requested");
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

    apo::LogListenEvent(L"StartListening requested for " +
                        g_endpoints[g_effectIndex].id);
    const std::wstring error =
        apo::StartMonitor(g_endpoints[g_effectIndex].id);
    apo::LogListenEvent(error.empty() ? L"StartListening succeeded"
                                      : (L"StartListening failed: " + error));
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
    if (g_mainEnable == nullptr) {
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

void Refresh(const std::wstring& preferredDeviceId = std::wstring())
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
    if (!preferredDeviceId.empty()) {
        effectId = preferredDeviceId;
    } else if (ValidEffectIndex()) {
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

    // Any refresh establishes the new baseline, including the ones triggered
    // by the poll itself, so it does not immediately refresh again.
    g_stateSignature = StateSignature(g_files, g_endpoints);
}

// Re-enumerates on the UI thread and refreshes only when something outside this
// window actually changed. g_preserveDirtyEffects keeps in-progress effect
// edits alive across the reload, just like the old manual Refresh button did.
void CheckForExternalChanges()
{
    if (g_busy) {
        return;
    }

    const apo::FilesStatus files = apo::GetFilesStatus();
    const std::vector<apo::Endpoint> endpoints = apo::EnumEndpoints();
    if (StateSignature(files, endpoints) == g_stateSignature) {
        return;
    }

    // A device enable/disable or binding change rebuilds the APO chain. Drop
    // optimistic labels now; the restored bank will confirm the real states.
    ResetLooperLocalStates();
    g_preserveDirtyEffects = true;
    Refresh();
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
        ResetLooperLocalStates();
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
    ResetLooperLocalStates();
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
        UpdateMainUi();
        return;
    }
    if (!ValidEffectIndex() || !HasEffectSelection()) {
        return;
    }

    apo::Endpoint& endpoint = g_endpoints[g_effectIndex];
    if (!apo::IsDeviceActive(endpoint.state)) {
        MessageBoxW(g_window,
                    L"This device is not active. Enable it in Windows and try "
                    L"again.",
                    L"Better Micro APO", MB_ICONWARNING);
        return;
    }
    if (!endpoint.Enabled()) {
        MessageBoxW(g_window,
                    L"Enable Better Micro APO for this device before capturing "
                    L"a noise profile.",
                    L"Better Micro APO", MB_ICONWARNING);
        return;
    }

    const apo::ApoSettings snapshot = endpoint.settings;
    const apo::EffectPreset& preset = apo::ActivePreset(snapshot);
    if (g_effectSelection >= preset.effectCount) {
        return;
    }
    const apo::EffectInstance& effect = preset.effects[g_effectSelection];
    if (effect.type != apo::EffectType::Noise) {
        return;
    }
    if (!apo::EffectEnabled(effect)) {
        MessageBoxW(g_window,
                    L"Turn this Noise effect on before capturing. The profile "
                    L"is measured where this effect sits in the chain.",
                    L"Better Micro APO", MB_ICONWARNING);
        return;
    }

    const std::wstring id = endpoint.id;
    const int presetIndex = snapshot.presetIndex;
    const int effectId = effect.id;

    // Capture does not touch settings or bindings, so keep any pending edits
    // and keep the audition stream running.
    g_preserveDirtyEffects = true;
    StartWork([id, presetIndex, effectId] {
        std::wstring error;
        const bool ok = apo::RequestStageCapture(
            id, presetIndex, effectId, kNoiseCaptureMs, error);

        apo::Result result;
        result.ok = ok;
        result.summary = ok ? L"Noise profile captured."
                            : L"Noise profile capture failed.";
        if (!ok) {
            result.details = error;
        }
        return result;
    }, L"Capturing noise profile...");
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
    if (item.iItem == 0 && checkChanged) {
        // Input is always on. Restore the glyph instead of changing settings.
        g_effectLoading = true;
        ListView_SetCheckState(g_effectList, 0, TRUE);
        g_effectLoading = false;
        return;
    }
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
    if (ValidEffectIndex()) {
        gui::SaveLastDevice(g_endpoints[g_effectIndex].id);
    }

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
            const bool checkChanged =
                (item->uChanged & LVIF_STATE) != 0 &&
                ((item->uOldState ^ item->uNewState) & LVIS_STATEIMAGEMASK) != 0;
            if (item->iItem == 0 && checkChanged &&
                ((item->uNewState & LVIS_STATEIMAGEMASK) >> 12) != 2) {
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
        const std::wstring startupDeviceId = gui::LoadLastDevice();
        ui::CreateMenuBar(window, g_showAllDevices);
        CreateControls(window);
        ui::ApplyNativeThemeToChildren(window);
        ui::ApplyListStateImages(g_effectList);
        Refresh(startupDeviceId);
        RepaintAll();
        SetTimer(window, kRefreshTimer, kRefreshPollMs, nullptr);
        SetTimer(window, kStageMonitorTimer, kStageMonitorPollMs, nullptr);
        return 0;
    }

    case WM_NCACTIVATE: {
        LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        // Windows repaints the menu bar border with a bright inactive color.
        // Force another non-client paint so our divider always wins.
        RedrawWindow(window, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        return result;
    }

    case WM_ACTIVATE:
        // A key released after the window loses focus would otherwise fire a
        // stale tap or long press.
        if (LOWORD(wParam) == WA_INACTIVE) {
            CancelLooperKeyGestures();
        }
        return 0;

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

    case kLooperGestureMessage:
        ApplyLooperGesture(static_cast<int>(wParam),
                           static_cast<LooperGesture>(lParam));
        return 0;

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
        case IDC_MAIN_ENABLE:
            if (HIWORD(wParam) == BN_CLICKED) {
                OnMainEnableClicked();
            }
            break;
        case IDC_INPUT_TREAT_MONO:
            if (HIWORD(wParam) == BN_CLICKED) {
                MarkSettingsDirty();
            }
            break;
        case IDC_EFFECT_ADD:
            if (HIWORD(wParam) == BN_CLICKED) {
                DoAddEffect();
            }
            break;
        case IDC_MAIN_ENABLE_ICON:
        case IDC_DYN_LOOKAHEAD_ICON:
        case IDC_EQ_LOOKAHEAD_ICON:
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
        case IDC_EQ_DYNAMIC_COMBO:
        case IDC_REVERB_ALGORITHM_COMBO:
        case IDC_ECHO_MODE_COMBO:
        case IDC_TREMOLO_WAVEFORM_COMBO:
        case IDC_VIBRATO_WAVEFORM_COMBO:
        case IDC_PITCH_ALGORITHM_COMBO:
        case IDC_NOISE_FFT_SIZE_COMBO:
        case IDC_TUNER_ALGORITHM_COMBO:
        case IDC_TUNER_SAMPLE_SIZE_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                MarkSettingsDirty();
            } else if (HIWORD(wParam) == CBN_CLOSEUP) {
                RepaintAll();
            }
            break;
        case IDC_DYN_INPUT_SLIDER:
        case IDC_DYN_THRESHOLD_SLIDER:
        case IDC_DYN_RATIO_SLIDER:
        case IDC_DYN_ATTACK_SLIDER:
        case IDC_DYN_RELEASE_SLIDER:
        case IDC_DYN_HOLD_SLIDER:
        case IDC_DYN_OUTPUT_SLIDER:
        case IDC_DYN_LOOKAHEAD_SLIDER:
        case IDC_INPUT_GAIN_SLIDER:
        case IDC_GAIN_SLIDER:
        case IDC_NOISE_REDUCTION_SLIDER:
        case IDC_EQ_FREQUENCY_SLIDER:
        case IDC_EQ_Q_SLIDER:
        case IDC_EQ_THRESHOLD_SLIDER:
        case IDC_EQ_RATIO_SLIDER:
        case IDC_EQ_RANGE_SLIDER:
        case IDC_EQ_LEVEL_SLIDER:
        case IDC_EQ_ATTACK_SLIDER:
        case IDC_EQ_RELEASE_SLIDER:
        case IDC_EQ_HOLD_SLIDER:
        case IDC_EQ_LOOKAHEAD_SLIDER:
        case IDC_REVERB_LEVEL_SLIDER:
        case IDC_REVERB_MAX_LENGTH_SLIDER:
        case IDC_REVERB_SIZE_SLIDER:
        case IDC_REVERB_DAMPING_SLIDER:
        case IDC_REVERB_WIDTH_SLIDER:
        case IDC_REVERB_DENSITY_SLIDER:
        case IDC_REVERB_PREDELAY_SLIDER:
        case IDC_ECHO_TIME_SLIDER:
        case IDC_ECHO_FEEDBACK_SLIDER:
        case IDC_ECHO_DAMPING_SLIDER:
        case IDC_ECHO_LEVEL_SLIDER:
        case IDC_CHORUS_VOICES_SLIDER:
        case IDC_CHORUS_RATE_SLIDER:
        case IDC_CHORUS_DEPTH_SLIDER:
        case IDC_CHORUS_WIDTH_SLIDER:
        case IDC_CHORUS_LEVEL_SLIDER:
        case IDC_TREMOLO_RATE_SLIDER:
        case IDC_TREMOLO_DEPTH_SLIDER:
        case IDC_TREMOLO_SPREAD_SLIDER:
        case IDC_VIBRATO_RATE_SLIDER:
        case IDC_VIBRATO_DEPTH_SLIDER:
        case IDC_VIBRATO_SPREAD_SLIDER:
        case IDC_PITCH_SHIFT_SLIDER:
        case IDC_PITCH_WINDOW_SLIDER:
        case IDC_PITCH_LEVEL_SLIDER:
        case IDC_PITCH_DRY_LEVEL_SLIDER:
        case IDC_DIST_DRIVE_SLIDER:
        case IDC_DIST_CURVATURE_SLIDER:
        case IDC_DIST_OUTPUT_SLIDER:
        case IDC_TUNER_SMOOTHING_SLIDER:
        case IDC_TUNER_REFERENCE_SLIDER:
        case IDC_LOOPER_VOLUME_1:
        case IDC_LOOPER_VOLUME_2:
        case IDC_LOOPER_VOLUME_3:
        case IDC_LOOPER_VOLUME_4:
        case IDC_LOOPER_VOLUME_5:
            if (ui::IsSliderChange(HIWORD(wParam))) {
                MarkSettingsDirty();
            }
            break;
        case IDC_LOOPER_FREE_LENGTH_1:
        case IDC_LOOPER_FREE_LENGTH_2:
        case IDC_LOOPER_FREE_LENGTH_3:
        case IDC_LOOPER_FREE_LENGTH_4:
        case IDC_LOOPER_FREE_LENGTH_5:
        case IDC_LOOPER_STOP_END_1:
        case IDC_LOOPER_STOP_END_2:
        case IDC_LOOPER_STOP_END_3:
        case IDC_LOOPER_STOP_END_4:
        case IDC_LOOPER_STOP_END_5:
            // The button's own BN_CLICKED is ignored; gestures are delivered as
            // kLooperGestureMessage by the subclass.
            if (HIWORD(wParam) == BN_CLICKED) {
                MarkSettingsDirty();
            }
            break;
        case IDC_LOOPER_BUTTON_1:
        case IDC_LOOPER_BUTTON_2:
        case IDC_LOOPER_BUTTON_3:
        case IDC_LOOPER_BUTTON_4:
        case IDC_LOOPER_BUTTON_5:
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

        // A failed operation may have changed nothing, so keep unsaved effect
        // edits instead of reloading over them. Noise capture sets this before
        // starting because it never changes settings.
        if (!g_result.ok) {
            g_preserveDirtyEffects = true;
        }
        Refresh();

        // A settings edit can arrive while an apply is running. Resume the
        // live save now that the worker is done; if the live channel is
        // missing, the edit stays pending for the normal Apply path.
        if (ValidEffectIndex() &&
            g_endpoints[g_effectIndex].settingsDirty) {
            g_liveApplyPending =
                SetTimer(g_window, kLiveApplyTimer, kLiveApplyDelayMs,
                         nullptr) != 0;
        }

        // Restart the audition stream if it was active before Apply. The
        // endpoint id is checked so a disabled or removed device does not
        // spring back to life.
        if (g_resumeListen) {
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
        } else if (wParam == kDeviceChangeTimer) {
            KillTimer(window, kDeviceChangeTimer);
            CheckForExternalChanges();
        } else if (wParam == kRefreshTimer) {
            CheckForExternalChanges();
        } else if (wParam >= kLooperKeyLongPressBase &&
                   wParam < kLooperKeyLongPressBase + apo::kLooperTrackCount)
        {
            HandleLooperKeyTimer(
                static_cast<int>(wParam - kLooperKeyLongPressBase), true);
        } else if (wParam >= kLooperKeyTapBase &&
                   wParam < kLooperKeyTapBase + apo::kLooperTrackCount)
        {
            HandleLooperKeyTimer(
                static_cast<int>(wParam - kLooperKeyTapBase), false);
        } else if (wParam == kStageMonitorTimer) {
            PollStageMonitor();
        }
        break;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL ||
            wParam == DBT_DEVICEREMOVECOMPLETE) {
            // Notifications arrive in bursts while the audio stack settles.
            // Collapse them into one re-check shortly after the last event.
            SetTimer(window, kDeviceChangeTimer, kDeviceChangeDebounceMs,
                     nullptr);
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
        KillTimer(window, kRefreshTimer);
        KillTimer(window, kDeviceChangeTimer);
        KillTimer(window, kStageMonitorTimer);
        for (int track = 0; track < apo::kLooperTrackCount; ++track) {
            KillTimer(window, kLooperKeyLongPressBase + track);
            KillTimer(window, kLooperKeyTapBase + track);
        }
        CancelLooperKeyGestures();
        g_liveApplyPending = false;
        g_stageMonitor.Close();
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

bool AnyComboDropped(std::initializer_list<HWND> combos)
{
    for (HWND combo : combos) {
        if (combo != nullptr && IsWindow(combo) &&
            SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0)
        {
            return true;
        }
    }
    return false;
}

// Keyboard shortcuts act only while Looper is the selected effect. A drop-down
// keeps its normal keyboard handling, and modifier keys stay available for menu
// and system shortcuts.
int LooperKeyTrack(const MSG& message)
{
    switch (message.wParam) {
    case '1': return 0;
    case '2': return 1;
    case '3': return 2;
    case '4': return 3;
    case VK_SPACE: return 4;
    default: return -1;
    }
}

bool LooperKeyboardReady()
{
    return !g_busy && !g_effectLoading && HasEffectSelection() &&
           g_effectType == apo::EffectType::Looper &&
           !AnyComboDropped({
               g_effectDeviceCombo, g_effectPresetCombo, g_noiseFftSizeCombo,
               g_eqDynamicCombo, g_distFunctionCombo, g_reverbAlgorithmCombo,
               g_echoModeCombo, g_tremoloWaveformCombo, g_vibratoWaveformCombo,
               g_pitchAlgorithmCombo, g_tunerAlgorithmCombo,
               g_tunerSampleSizeCombo});
}

void HandleLooperKeyTimer(int track, bool longTimer)
{
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return;
    }
    LooperKeyGestureState& state = g_looperKeyGestures[track];
    if (longTimer) {
        if (!state.down || state.longFired || state.consumed ||
            state.timerGeneration != state.generation)
        {
            return;
        }
        ++state.generation;
        state.timerGeneration = state.generation;
        state.longFired = true;
        state.tapPending = false;
        state.lastTapTick = 0;
        KillTimer(g_window, kLooperKeyLongPressBase + track);
        KillTimer(g_window, kLooperKeyTapBase + track);
        if (state.provisional && !state.provisionalFinish) {
            ApplyLooperGestureFromState(track, state.originalState,
                                        LooperGesture::LongPress);
        } else if (!state.provisional) {
            ApplyLooperGesture(track, LooperGesture::LongPress);
        }
        // A provisional finish already did the long-press action.
        state.provisional = false;
        state.provisionalFinish = false;
        return;
    }

    if (!state.tapPending || state.timerGeneration != state.generation) {
        return;
    }
    ++state.generation;
    state.timerGeneration = state.generation;
    state.tapPending = false;
    state.lastTapTick = 0;
    if (state.provisional && !state.provisionalFinish) {
        // The press was a tap: keep the recording that already started and
        // make its undo snapshot authoritative.
        if (PublishLooperCommand(apo::LooperCommand::CommitProvisional, track)) {
            UpdateLooperUndoAfterCommand(
                apo::LooperCommand::StartRecordProvisional, track,
                state.originalState);
        }
    } else if (!state.provisional) {
        ApplyLooperGesture(track, LooperGesture::Tap);
    }
    // A provisional finish was already committed on the keydown.
    state.provisional = false;
    state.provisionalFinish = false;
}

bool HandleLooperKeyboard(const MSG& message)
{
    if (message.message != WM_KEYDOWN && message.message != WM_KEYUP) {
        return false;
    }

    const int track = LooperKeyTrack(message);
    if (track < 0) {
        return false;
    }
    if (!LooperKeyboardReady()) {
        CancelLooperKeyGestures();
        return false;
    }

    EnsureLooperLocalStates(g_endpoints[g_effectIndex],
                            apo::ActivePreset(g_endpoints[g_effectIndex].settings)
                                .effects[g_effectSelection]
                                .id);
    LooperKeyGestureState& state = g_looperKeyGestures[track];
    if (message.message == WM_KEYDOWN) {
        constexpr DWORD_PTR kRepeatMask = DWORD_PTR{1} << 30;
        if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 ||
            GetKeyState(VK_SHIFT) < 0)
        {
            CancelLooperKeyGestures();
            return false;
        }
        if ((static_cast<DWORD_PTR>(message.lParam) & kRepeatMask) != 0) {
            // Auto-repeat must not turn a held key into repeated taps.
            return true;
        }
        if (state.down) {
            return true;
        }

        const DWORD now = GetTickCount();
        KillTimer(g_window, kLooperKeyLongPressBase + track);
        KillTimer(g_window, kLooperKeyTapBase + track);
        ++state.generation;
        state.timerGeneration = state.generation;
        state.down = true;
        state.downTick = now;
        state.longFired = false;

        if (state.tapPending && state.lastTapTick != 0 &&
            now - state.lastTapTick <= kLooperDoubleTapMs)
        {
            state.consumed = true;
            state.tapPending = false;
            state.lastTapTick = 0;
            if (state.provisional) {
                // An immediate finish already moved the track to Playback;
                // treat the double tap from there, not from the pre-finish
                // Recording state, so Stop at loop end resolves correctly.
                const apo::LooperState gestureState =
                    state.provisionalFinish ? g_looperLocalStates[track]
                                            : state.originalState;
                ApplyLooperGestureFromState(track, gestureState,
                                            LooperGesture::DoubleTap);
            } else {
                ApplyLooperGesture(track, LooperGesture::DoubleTap);
            }
            state.provisional = false;
            state.provisionalFinish = false;
            return true;
        }

        state.consumed = false;
        state.tapPending = false;
        state.lastTapTick = 0;

        // Keyboard start and finish must not wait for the double-tap window;
        // sending them on keydown keeps the loop boundary aligned with the
        // press. A later double tap or long press still overrides a new take.
        const apo::LooperState current = g_looperLocalStates[track];
        if (current == apo::LooperState::Empty ||
            current == apo::LooperState::Playback)
        {
            if (PublishLooperCommand(apo::LooperCommand::StartRecordProvisional,
                                     track))
            {
                state.provisional = true;
                state.provisionalFinish = false;
                state.originalState = current;
                state.originalUndoValid = g_looperLocalUndoValid;
                state.originalUndoTrack = g_looperLocalUndoTrack;
                state.originalUndoState = g_looperLocalUndoState;
                SetLooperLocalState(track, apo::LooperState::Recording);
            } else {
                state.consumed = true;
                return true;
            }
        } else if (current == apo::LooperState::Recording) {
            // Finish the take on keydown so the loop boundary is not pushed
            // 250 ms later by the double-tap window. A later double tap still
            // sends Stop; a long press already means FinishRecord.
            if (PublishLooperCommand(apo::LooperCommand::FinishRecord, track)) {
                state.provisional = true;
                state.provisionalFinish = true;
                state.originalState = current;
                SetLooperLocalState(track, apo::LooperState::Playback);
            } else {
                state.consumed = true;
                return true;
            }
        } else {
            state.provisional = false;
            state.provisionalFinish = false;
        }

        SetTimer(g_window, kLooperKeyLongPressBase + track,
                 kLooperLongPressMs, nullptr);
        return true;
    }

    if (!state.down) {
        return false;
    }
    state.down = false;
    KillTimer(g_window, kLooperKeyLongPressBase + track);

    if (state.consumed) {
        state.consumed = false;
        state.tapPending = false;
        state.lastTapTick = 0;
        ++state.generation;
        return true;
    }
    if (state.longFired) {
        state.longFired = false;
        state.tapPending = false;
        state.lastTapTick = 0;
        ++state.generation;
        return true;
    }

    const DWORD now = GetTickCount();
    const DWORD held = now - state.downTick;
    state.timerGeneration = state.generation;
    if (held >= kLooperLongPressMs) {
        ++state.generation;
        state.lastTapTick = 0;
        if (state.provisional && !state.provisionalFinish) {
            ApplyLooperGestureFromState(track, state.originalState,
                                        LooperGesture::LongPress);
        } else if (!state.provisional) {
            ApplyLooperGesture(track, LooperGesture::LongPress);
        }
        // A provisional finish already did the long-press action.
        state.provisional = false;
        state.provisionalFinish = false;
        return true;
    }
    // A provisional take has already started, so a release before the long
    // press threshold is a tap regardless of the 400 ms tap-max guideline.
    if (state.provisional || held < kLooperTapMaxMs) {
        state.tapPending = true;
        state.lastTapTick = now;
        SetTimer(g_window, kLooperKeyTapBase + track, kLooperDoubleTapMs,
                 nullptr);
    } else {
        state.tapPending = false;
        state.lastTapTick = 0;
    }
    return true;
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
        if (HandleLooperKeyboard(message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

