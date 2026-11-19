#pragma once

#include <windows.h>

#include "apo_settings.h"

// Shared UI state. The definitions live in ui_state.cpp so the view
// construction, layout, and widget modules can all reach the same controls
// without threading a large context object through every call.

// Control identifiers. Keeping them in one place lets a view add a control
// without touching an enum in the window procedure.
enum : int {
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
    IDC_MAIN_ENABLE_ICON,
    IDC_MONO_NOTE,

    // Effect parameters.
    IDC_DYN_MODE_LABEL,
    IDC_DYN_MODE_COMBO,
    IDC_DYN_INPUT_LABEL,
    IDC_DYN_INPUT_SLIDER,
    IDC_DYN_THRESHOLD_LABEL,
    IDC_DYN_THRESHOLD_SLIDER,
    IDC_DYN_RATIO_LABEL,
    IDC_DYN_RATIO_SLIDER,
    IDC_DYN_ATTACK_LABEL,
    IDC_DYN_ATTACK_SLIDER,
    IDC_DYN_RELEASE_LABEL,
    IDC_DYN_RELEASE_SLIDER,
    IDC_DYN_HOLD_LABEL,
    IDC_DYN_HOLD_SLIDER,
    IDC_DYN_OUTPUT_LABEL,
    IDC_DYN_OUTPUT_SLIDER,
    IDC_DYN_LOOKAHEAD_LABEL,
    IDC_DYN_LOOKAHEAD_SLIDER,
    IDC_DYN_LOOKAHEAD_ICON,
    IDC_INPUT_GAIN_LABEL,
    IDC_INPUT_GAIN_SLIDER,
    IDC_GAIN_LABEL,
    IDC_GAIN_SLIDER,
    IDC_EQ_DYNAMIC_LABEL,
    IDC_EQ_DYNAMIC_COMBO,
    IDC_EQ_FREQUENCY_LABEL,
    IDC_EQ_FREQUENCY_SLIDER,
    IDC_EQ_Q_LABEL,
    IDC_EQ_Q_SLIDER,
    IDC_EQ_THRESHOLD_LABEL,
    IDC_EQ_THRESHOLD_SLIDER,
    IDC_EQ_RATIO_LABEL,
    IDC_EQ_RATIO_SLIDER,
    IDC_EQ_RANGE_LABEL,
    IDC_EQ_RANGE_SLIDER,
    IDC_EQ_LEVEL_LABEL,
    IDC_EQ_LEVEL_SLIDER,
    IDC_EQ_ATTACK_LABEL,
    IDC_EQ_ATTACK_SLIDER,
    IDC_EQ_RELEASE_LABEL,
    IDC_EQ_RELEASE_SLIDER,
    IDC_EQ_HOLD_LABEL,
    IDC_EQ_HOLD_SLIDER,
    IDC_EQ_LOOKAHEAD_LABEL,
    IDC_EQ_LOOKAHEAD_SLIDER,
    IDC_EQ_LOOKAHEAD_ICON,
    IDC_DIST_FUNCTION_LABEL,
    IDC_DIST_FUNCTION_COMBO,
    IDC_DIST_DRIVE_LABEL,
    IDC_DIST_DRIVE_SLIDER,
    IDC_DIST_CURVATURE_LABEL,
    IDC_DIST_CURVATURE_SLIDER,
    IDC_DIST_OUTPUT_LABEL,
    IDC_DIST_OUTPUT_SLIDER,
    IDC_REVERB_ALGORITHM_LABEL,
    IDC_REVERB_ALGORITHM_COMBO,
    IDC_REVERB_LEVEL_LABEL,
    IDC_REVERB_LEVEL_SLIDER,
    IDC_REVERB_MAX_LENGTH_LABEL,
    IDC_REVERB_MAX_LENGTH_SLIDER,
    IDC_REVERB_SIZE_LABEL,
    IDC_REVERB_SIZE_SLIDER,
    IDC_REVERB_DAMPING_LABEL,
    IDC_REVERB_DAMPING_SLIDER,
    IDC_REVERB_WIDTH_LABEL,
    IDC_REVERB_WIDTH_SLIDER,
    IDC_REVERB_DENSITY_LABEL,
    IDC_REVERB_DENSITY_SLIDER,
    IDC_REVERB_PREDELAY_LABEL,
    IDC_REVERB_PREDELAY_SLIDER,
    IDC_REVERB_PREDELAY_ICON,
    IDC_NOISE_REDUCTION_LABEL,
    IDC_NOISE_REDUCTION_SLIDER,
    IDC_NOISE_FFT_SIZE_LABEL,
    IDC_NOISE_FFT_SIZE_COMBO,
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
    ParamResetNoiseFftSize,
    ParamResetDynamicsMode,
    ParamResetDynamicsInput,
    ParamResetDynamicsThreshold,
    ParamResetDynamicsRatio,
    ParamResetDynamicsAttack,
    ParamResetDynamicsRelease,
    ParamResetDynamicsHold,
    ParamResetDynamicsOutput,
    ParamResetDynamicsLookahead,
    ParamResetInputGain,
    ParamResetGain,
    ParamResetDynMode,
    ParamResetDynFrequency,
    ParamResetDynQ,
    ParamResetDynLevel,
    ParamResetDynThreshold,
    ParamResetDynRatio,
    ParamResetDynRange,
    ParamResetDynAttack,
    ParamResetDynRelease,
    ParamResetDynHold,
    ParamResetDynLookahead,
    ParamResetReverbAlgorithm,
    ParamResetReverbLevel,
    ParamResetReverbMaxLength,
    ParamResetReverbSize,
    ParamResetReverbDamping,
    ParamResetReverbWidth,
    ParamResetReverbDensity,
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
extern HWND g_pageBackground;
extern HWND g_status;
extern HWND g_infoTooltip;

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
extern HWND g_mainEnableIcon;
extern HWND g_monoNote;
extern HWND g_paramResetButtons[ParamResetCount];

extern HWND g_dynamicsModeLabel;
extern HWND g_dynamicsModeCombo;
extern HWND g_dynamicsInputLabel;
extern HWND g_dynamicsInputSlider;
extern HWND g_dynamicsThresholdLabel;
extern HWND g_dynamicsThresholdSlider;
extern HWND g_dynamicsRatioLabel;
extern HWND g_dynamicsRatioSlider;
extern HWND g_dynamicsAttackLabel;
extern HWND g_dynamicsAttackSlider;
extern HWND g_dynamicsReleaseLabel;
extern HWND g_dynamicsReleaseSlider;
extern HWND g_dynamicsHoldLabel;
extern HWND g_dynamicsHoldSlider;
extern HWND g_dynamicsOutputLabel;
extern HWND g_dynamicsOutputSlider;
extern HWND g_dynamicsLookaheadLabel;
extern HWND g_dynamicsLookaheadSlider;
extern HWND g_dynamicsLookaheadIcon;
extern HWND g_inputGainLabel;
extern HWND g_inputGainSlider;
extern HWND g_gainLabel;
extern HWND g_gainSlider;
extern HWND g_eqDynamicLabel;
extern HWND g_eqDynamicCombo;
extern HWND g_eqFrequencyLabel;
extern HWND g_eqFrequencySlider;
extern HWND g_eqQLabel;
extern HWND g_eqQSlider;
extern HWND g_eqThresholdLabel;
extern HWND g_eqThresholdSlider;
extern HWND g_eqRatioLabel;
extern HWND g_eqRatioSlider;
extern HWND g_eqRangeLabel;
extern HWND g_eqRangeSlider;
extern HWND g_eqLevelLabel;
extern HWND g_eqLevelSlider;
extern HWND g_eqAttackLabel;
extern HWND g_eqAttackSlider;
extern HWND g_eqReleaseLabel;
extern HWND g_eqReleaseSlider;
extern HWND g_eqHoldLabel;
extern HWND g_eqHoldSlider;
extern HWND g_eqLookaheadLabel;
extern HWND g_eqLookaheadSlider;
extern HWND g_eqLookaheadIcon;
extern HWND g_distFunctionLabel;
extern HWND g_distFunctionCombo;
extern HWND g_distDriveLabel;
extern HWND g_distDriveSlider;
extern HWND g_distCurvatureLabel;
extern HWND g_distCurvatureSlider;
extern HWND g_distOutputLabel;
extern HWND g_distOutputSlider;
extern HWND g_reverbAlgorithmLabel;
extern HWND g_reverbAlgorithmCombo;
extern HWND g_reverbLevelLabel;
extern HWND g_reverbLevelSlider;
extern HWND g_reverbMaxLengthLabel;
extern HWND g_reverbMaxLengthSlider;
extern HWND g_reverbSizeLabel;
extern HWND g_reverbSizeSlider;
extern HWND g_reverbDampingLabel;
extern HWND g_reverbDampingSlider;
extern HWND g_reverbWidthLabel;
extern HWND g_reverbWidthSlider;
extern HWND g_reverbDensityLabel;
extern HWND g_reverbDensitySlider;
extern HWND g_reverbPredelayLabel;
extern HWND g_reverbPredelaySlider;
extern HWND g_reverbPredelayIcon;
extern HWND g_noiseReductionLabel;
extern HWND g_noiseReductionSlider;
extern HWND g_noiseFftSizeLabel;
extern HWND g_noiseFftSizeCombo;
extern HWND g_noiseCapture;
extern HWND g_noiseNote;
extern HWND g_listen;
extern HWND g_meter;

extern HFONT g_font;
extern HFONT g_boldFont;
extern int g_dpi;

// Selected row in the effect list and the type of that row. The list mirrors
// the selected endpoint's effect array, so the row index is also the effect
// index. g_effectSelection is -1 when the list is empty.
extern int g_effectSelection;
extern apo::EffectType g_effectType;
