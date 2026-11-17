#pragma once

#include <windows.h>

#include "apo_settings.h"

// Shared UI state. The definitions live in ui_state.cpp so the view
// construction, layout, and widget modules can all reach the same controls
// without threading a large context object through every call.

// Control identifiers. Keeping them in one place lets a view add a control
// without touching an enum in the window procedure.
enum : int {
    IDC_TITLE = 100,
    IDC_REFRESH,

    // Main view.
    IDC_EFFECT_DEVICE_LABEL,
    IDC_EFFECT_DEVICE_COMBO,
    IDC_EFFECT_PRESET_LABEL,
    IDC_EFFECT_PRESET_COMBO,
    IDC_EFFECT_LIST_GROUP,
    IDC_EFFECT_LIST,
    IDC_EFFECT_ADD,
    IDC_EFFECT_GROUP,
    IDC_EFFECT_HEADING,
    IDC_EFFECT_GRAPH,
    IDC_MAIN_ENABLE,
    IDC_MAIN_HINT,
    IDC_MONO_NOTE,

    // Effect parameters.
    IDC_GATE_THRESHOLD_LABEL,
    IDC_GATE_THRESHOLD_SLIDER,
    IDC_GATE_RANGE_LABEL,
    IDC_GATE_RANGE_SLIDER,
    IDC_GATE_ATTACK_LABEL,
    IDC_GATE_ATTACK_SLIDER,
    IDC_GATE_HOLD_LABEL,
    IDC_GATE_HOLD_SLIDER,
    IDC_GATE_RELEASE_LABEL,
    IDC_GATE_RELEASE_SLIDER,
    IDC_GATE_LOOKAHEAD_LABEL,
    IDC_GATE_LOOKAHEAD_SLIDER,
    IDC_COMP_THRESHOLD_LABEL,
    IDC_COMP_THRESHOLD_SLIDER,
    IDC_COMP_RATIO_LABEL,
    IDC_COMP_RATIO_SLIDER,
    IDC_COMP_ATTACK_LABEL,
    IDC_COMP_ATTACK_SLIDER,
    IDC_COMP_RELEASE_LABEL,
    IDC_COMP_RELEASE_SLIDER,
    IDC_MAKEUP_LABEL,
    IDC_MAKEUP_SLIDER,
    IDC_DYNAMIC_EXTEND,
    IDC_DYN_FREQUENCY_LABEL,
    IDC_DYN_FREQUENCY_SLIDER,
    IDC_DYN_Q_LABEL,
    IDC_DYN_Q_SLIDER,
    IDC_DYN_THRESHOLD_LABEL,
    IDC_DYN_THRESHOLD_SLIDER,
    IDC_DYN_RATIO_LABEL,
    IDC_DYN_RATIO_SLIDER,
    IDC_DYN_RANGE_LABEL,
    IDC_DYN_RANGE_SLIDER,
    IDC_DYN_ATTACK_LABEL,
    IDC_DYN_ATTACK_SLIDER,
    IDC_DYN_RELEASE_LABEL,
    IDC_DYN_RELEASE_SLIDER,
    IDC_DIST_FUNCTION_LABEL,
    IDC_DIST_FUNCTION_COMBO,
    IDC_DIST_DRIVE_LABEL,
    IDC_DIST_DRIVE_SLIDER,
    IDC_DIST_CURVATURE_LABEL,
    IDC_DIST_CURVATURE_SLIDER,
    IDC_DIST_OUTPUT_LABEL,
    IDC_DIST_OUTPUT_SLIDER,
    IDC_DISTORTION_NOTE,
    IDC_REVERB_MIX_LABEL,
    IDC_REVERB_MIX_SLIDER,
    IDC_REVERB_SIZE_LABEL,
    IDC_REVERB_SIZE_SLIDER,
    IDC_REVERB_DAMPING_LABEL,
    IDC_REVERB_DAMPING_SLIDER,
    IDC_REVERB_PREDELAY_LABEL,
    IDC_REVERB_PREDELAY_SLIDER,
    IDC_NOISE_REDUCTION_LABEL,
    IDC_NOISE_REDUCTION_SLIDER,
    IDC_NOISE_CAPTURE,
    IDC_NOISE_NOTE,
    IDC_LISTEN,
    IDC_METER,
    IDC_PARAM_RESET,

    // Shared status bar.
    IDC_STATUS,
};

// Small ↺ button beside each editable effect parameter.
enum ParamReset : int {
    ParamResetNoiseReduction,
    ParamResetGateThreshold,
    ParamResetGateRange,
    ParamResetGateAttack,
    ParamResetGateHold,
    ParamResetGateRelease,
    ParamResetGateLookahead,
    ParamResetCompThreshold,
    ParamResetCompRatio,
    ParamResetCompAttack,
    ParamResetCompRelease,
    ParamResetMakeup,
    ParamResetDynExpand,
    ParamResetDynFrequency,
    ParamResetDynQ,
    ParamResetDynThreshold,
    ParamResetDynRatio,
    ParamResetDynRange,
    ParamResetDynAttack,
    ParamResetDynRelease,
    ParamResetReverbMix,
    ParamResetReverbSize,
    ParamResetReverbDamping,
    ParamResetReverbPredelay,
    ParamResetDistFunction,
    ParamResetDistDrive,
    ParamResetDistCurvature,
    ParamResetDistOutput,
    ParamResetCount,
};

// Main menu commands. Kept above the control range so they never collide.
enum : int {
    IDM_FILE_INSTALL = 40000,
    IDM_FILE_UNINSTALL,
    IDM_FILE_EXIT,
    IDM_VIEW_SHOW_ALL_DEVICES,
    IDM_ABOUT_INFO,
    IDM_ABOUT_OPEN_LOG,
};

constexpr wchar_t kPageClass[] = L"BetterMicroApoPage";

extern HINSTANCE g_instance;
extern HWND g_window;

// Always-visible chrome.
extern HWND g_title;
extern HWND g_refresh;
extern HWND g_pageBackground;
extern HWND g_status;

// Main view.
extern HWND g_effectDeviceLabel;
extern HWND g_effectDeviceCombo;
extern HWND g_effectPresetLabel;
extern HWND g_effectPresetCombo;
extern HWND g_effectListGroup;
extern HWND g_effectList;
extern HWND g_effectAdd;
extern HWND g_effectGroup;
extern HWND g_effectHeading;
extern HWND g_effectGraph;
extern HWND g_mainEnable;
extern HWND g_mainHint;
extern HWND g_monoNote;
extern HWND g_paramResetButtons[ParamResetCount];

extern HWND g_gateThresholdLabel;
extern HWND g_gateThresholdSlider;
extern HWND g_gateRangeLabel;
extern HWND g_gateRangeSlider;
extern HWND g_gateAttackLabel;
extern HWND g_gateAttackSlider;
extern HWND g_gateHoldLabel;
extern HWND g_gateHoldSlider;
extern HWND g_gateReleaseLabel;
extern HWND g_gateReleaseSlider;
extern HWND g_gateLookaheadLabel;
extern HWND g_gateLookaheadSlider;
extern HWND g_compThresholdLabel;
extern HWND g_compThresholdSlider;
extern HWND g_compRatioLabel;
extern HWND g_compRatioSlider;
extern HWND g_compAttackLabel;
extern HWND g_compAttackSlider;
extern HWND g_compReleaseLabel;
extern HWND g_compReleaseSlider;
extern HWND g_makeupLabel;
extern HWND g_makeupSlider;
extern HWND g_dynamicExtend;
extern HWND g_dynFrequencyLabel;
extern HWND g_dynFrequencySlider;
extern HWND g_dynQLabel;
extern HWND g_dynQSlider;
extern HWND g_dynThresholdLabel;
extern HWND g_dynThresholdSlider;
extern HWND g_dynRatioLabel;
extern HWND g_dynRatioSlider;
extern HWND g_dynRangeLabel;
extern HWND g_dynRangeSlider;
extern HWND g_dynAttackLabel;
extern HWND g_dynAttackSlider;
extern HWND g_dynReleaseLabel;
extern HWND g_dynReleaseSlider;
extern HWND g_distFunctionLabel;
extern HWND g_distFunctionCombo;
extern HWND g_distDriveLabel;
extern HWND g_distDriveSlider;
extern HWND g_distCurvatureLabel;
extern HWND g_distCurvatureSlider;
extern HWND g_distOutputLabel;
extern HWND g_distOutputSlider;
extern HWND g_distortionNote;
extern HWND g_reverbMixLabel;
extern HWND g_reverbMixSlider;
extern HWND g_reverbSizeLabel;
extern HWND g_reverbSizeSlider;
extern HWND g_reverbDampingLabel;
extern HWND g_reverbDampingSlider;
extern HWND g_reverbPredelayLabel;
extern HWND g_reverbPredelaySlider;
extern HWND g_noiseReductionLabel;
extern HWND g_noiseReductionSlider;
extern HWND g_noiseCapture;
extern HWND g_noiseNote;
extern HWND g_listen;
extern HWND g_meter;

extern HFONT g_font;
extern HFONT g_boldFont;
extern HFONT g_titleFont;
extern int g_dpi;

// Selected row in the effect list and the type of that row. The list mirrors
// the selected endpoint's effect array, so the row index is also the effect
// index. g_effectSelection is -1 when the list is empty.
extern int g_effectSelection;
extern apo::EffectType g_effectType;
