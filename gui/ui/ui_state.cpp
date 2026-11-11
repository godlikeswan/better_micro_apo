#include "ui_state.h"

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;

HWND g_title = nullptr;
HWND g_refresh = nullptr;
HWND g_tabs = nullptr;
HWND g_pageBackground = nullptr;
HWND g_status = nullptr;
HWND g_logButton = nullptr;

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
HWND g_isolate = nullptr;
HWND g_hint = nullptr;
HWND g_devicesUnsaved = nullptr;
HWND g_devicesApply = nullptr;
HWND g_devicesReset = nullptr;

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

HFONT g_font = nullptr;
HFONT g_boldFont = nullptr;
HFONT g_titleFont = nullptr;
int g_dpi = 96;

View g_view = View::Install;
EffectKind g_effectKind = EffectKind::Noise;
