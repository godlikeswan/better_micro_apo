#pragma once

#include <windows.h>

#include "apo_settings.h"

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

} // namespace ui
