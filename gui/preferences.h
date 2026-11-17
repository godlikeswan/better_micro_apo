#pragma once

namespace gui {

// The menu choice is persisted but does not change the layout yet. Simple is
// the default so a fresh install opens the compact view.
enum class InterfaceMode {
    Simple,
    Advanced,
};

// C:\ProgramData\better_micro_apo\gui.ini
InterfaceMode LoadInterfaceMode();
void SaveInterfaceMode(InterfaceMode mode);

} // namespace gui
