#pragma once

#include <windows.h>

#include "apo_settings.h"

// Modal "Add effect" window. Kept separate from the main window so the type
// picker and its theme handling do not complicate the effects view.

namespace ui {

// Shows the picker and returns true when the user confirms. Types that already
// reached their per-type instance cap are left out of the list.
bool ShowAddEffectDialog(HWND owner, const apo::ApoSettings& settings,
                         apo::EffectType& type);

} // namespace ui
