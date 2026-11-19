#pragma once

#include <windows.h>

#include "apo_settings.h"
#include "stage_monitor.h"

// Small owner-drawn curve display for the effect panel. It shares its transfer
// math with the DSP stages (dsp/transfer.h), so the picture always matches what
// the stage applies. The control only displays; the sliders remain the input.

namespace ui {

// True when this effect type has a curve worth drawing.
bool GraphSupported(apo::EffectType type);

// Creates the shared graph child window.
HWND Graph(HWND parent, int id);

// Copies the current editor values and repaints. Safe to call on every drag.
void SetGraphEffect(HWND graph, const apo::EffectInstance& effect);

// Live per-stage telemetry from the running APO. Missing or stale telemetry is
// cleared by the caller so meters and overlays never imply a running stream.
void SetGraphMonitor(HWND graph, const apo::StageMonitorSnapshot& monitor);
void ClearGraphMonitor(HWND graph);

} // namespace ui
