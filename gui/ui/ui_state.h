#pragma once

#include <windows.h>

// Shared UI state. The definitions live in ui_state.cpp so the view
// construction, layout, and widget modules can all reach the same controls
// without threading a large context object through every call.

// Control identifiers. Keeping them in one place lets a view add a control
// without touching an enum in the window procedure.
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

enum class View : int {
    Install = 0,
    Devices,
    Effects,
};

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

constexpr wchar_t kPageClass[] = L"BetterMicroApoPage";

extern HINSTANCE g_instance;
extern HWND g_window;

// Always-visible chrome.
extern HWND g_title;
extern HWND g_refresh;
extern HWND g_tabs;
extern HWND g_pageBackground;
extern HWND g_status;
extern HWND g_logButton;

// Install view.
extern HWND g_groupFiles;
extern HWND g_dllLabel;
extern HWND g_dllValue;
extern HWND g_dllStatus;
extern HWND g_comLabel;
extern HWND g_comValue;
extern HWND g_comStatus;
extern HWND g_apoLabel;
extern HWND g_apoValue;
extern HWND g_apoStatus;
extern HWND g_overall;
extern HWND g_filesNote;
extern HWND g_install;
extern HWND g_uninstall;

// Devices view.
extern HWND g_groupDevices;
extern HWND g_list;
extern HWND g_isolate;
extern HWND g_hint;
extern HWND g_devicesUnsaved;
extern HWND g_devicesApply;
extern HWND g_devicesReset;

// Effects view.
extern HWND g_effectDeviceLabel;
extern HWND g_effectDeviceCombo;
extern HWND g_effectListGroup;
extern HWND g_effectList;
extern HWND g_effectGroup;
extern HWND g_effectHeading;
extern HWND g_effectsUnsaved;
extern HWND g_effectsApply;
extern HWND g_effectsReset;
extern HWND g_monoNote;

extern HWND g_gateThresholdLabel;
extern HWND g_gateThresholdEdit;
extern HWND g_gateRangeLabel;
extern HWND g_gateRangeEdit;
extern HWND g_gateAttackLabel;
extern HWND g_gateAttackEdit;
extern HWND g_gateHoldLabel;
extern HWND g_gateHoldEdit;
extern HWND g_gateReleaseLabel;
extern HWND g_gateReleaseEdit;
extern HWND g_gateLookaheadLabel;
extern HWND g_gateLookaheadEdit;
extern HWND g_compThresholdLabel;
extern HWND g_compThresholdEdit;
extern HWND g_compRatioLabel;
extern HWND g_compRatioEdit;
extern HWND g_compAttackLabel;
extern HWND g_compAttackEdit;
extern HWND g_compReleaseLabel;
extern HWND g_compReleaseEdit;
extern HWND g_makeupLabel;
extern HWND g_makeupEdit;
extern HWND g_dynamicExtend;
extern HWND g_dynFrequencyLabel;
extern HWND g_dynFrequencyEdit;
extern HWND g_dynQLabel;
extern HWND g_dynQEdit;
extern HWND g_dynThresholdLabel;
extern HWND g_dynThresholdEdit;
extern HWND g_dynRatioLabel;
extern HWND g_dynRatioEdit;
extern HWND g_dynRangeLabel;
extern HWND g_dynRangeEdit;
extern HWND g_dynAttackLabel;
extern HWND g_dynAttackEdit;
extern HWND g_dynReleaseLabel;
extern HWND g_dynReleaseEdit;
extern HWND g_noiseReductionLabel;
extern HWND g_noiseReductionEdit;
extern HWND g_noiseCapture;
extern HWND g_noiseNote;
extern HWND g_listen;
extern HWND g_listenNote;
extern HWND g_meter;

extern HFONT g_font;
extern HFONT g_boldFont;
extern HFONT g_titleFont;
extern int g_dpi;

extern View g_view;
extern EffectKind g_effectKind;

const wchar_t* EffectName(EffectKind kind);
