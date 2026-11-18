# GUI overview

The GUI is a native C++ Win32 program (`gui/main.cpp`). It installs the APO,
binds devices, edits settings, and provides a listen monitor. There is one
screen: device selector, **Enable**, preset selector, effects editor, **Listen
to this device**, and status.

## Startup and window classes

`wWinMain` (`gui/main.cpp`) sets system DPI awareness, reads the system DPI,
handles `--dump` before anything else, and holds the named mutex
`Local\better_micro_apo_gui_instance`. A second launch restores and flashes the
first window instead of opening another.

Two window classes are registered:

- `BetterMicroApoInstaller` — the top-level window and message loop, handled
  by `WindowProc`.
- A page class handled by `PageProc`, used as the background panel for all
  main-screen controls. Keeping controls on a child panel gives them a stable
  background and makes repaints a single pass.

`WM_CREATE` creates the fonts, loads `show_all_devices` and `last_device`, builds
the menu, calls `CreateControls`, applies native theming to children, sets up the
list's state images, refreshes state with the remembered device as the preferred
selection, repaints, and starts the 1200 ms refresh timer.

## Message handling

All routing is in `WindowProc` (`gui/main.cpp`); `PageProc` handles the panel
messages and `HandleNotify` handles list-view notifications.

| Message | Handling |
| --- | --- |
| `WM_CREATE` | Build the UI, load preferences, initial refresh, start refresh timer. |
| `WM_SIZE` | `ui::Layout` for the new client size, then `RepaintAll`. |
| `WM_COMMAND` | Menu and control commands (install, uninstall, enable, presets, effects, listen, dialogs). |
| `WM_NOTIFY` | `HandleNotify`: effect-list state, `NM_CLICK` on the `×` column, slider changes. |
| `WM_TIMER` | Four timers, listed below. |
| `WM_DEVICECHANGE` | Debounce into the device-change timer (device notifications arrive in bursts). |
| `WM_SETTINGCHANGE` / `WM_THEMECHANGED` | Rebuild theme palette, menu, list glyphs, title bar, info icons; repaint. |
| `WM_NCPAINT` / `WM_NCACTIVATE` | Repaint the menu divider that Windows paints with an inactive color. |
| `WM_GETMINMAXINFO` | Minimum window size. |
| `WM_CLOSE` | Blocked while busy (beep), otherwise `DestroyWindow`. |
| `WM_DESTROY` | Save preferences, stop monitor, join worker, destroy fonts/subclasses, shut down theme, `PostQuitMessage`. |
| `WM_APP_WORK_DONE` | Join the worker, clear busy state, apply the result, refresh UI, possibly resume listening. |

Timer ids and intervals (`gui/main.cpp`):

| Id | Name | Interval | Purpose |
| --- | --- | --- | --- |
| 1 | `kMonitorTimer` | 100 ms | Sample peak/RMS accumulators and update the meter; stop the checkbox if the monitor died. |
| 2 | `kLiveApplyTimer` | 200 ms | Debounced settings-only apply. |
| 3 | `kRefreshTimer` | 1200 ms | Poll for external install/device changes. |
| 4 | `kDeviceChangeTimer` | 200 ms | Debounced device-change re-check. |

`CheckForExternalChanges` compares a `StateSignature` of install status and
endpoint id/name/state/binding flags. Per-device settings are deliberately
excluded so an automatic refresh cannot clobber unsaved edits.

## Background work

Install, uninstall, binding, applying effects, and noise-profile capture all
run on a worker `std::thread`. `StartWork` sets `g_busy`, disables the relevant
controls via `UpdateChromeUi`, and the worker posts `WM_APP_WORK_DONE` when
finished. There are no confirmation dialogs; the cursor is never changed to a
busy shape.

`g_busy` is checked by command handlers and by the list's `LVN_ITEMCHANGING`
handler, so the effect list stays visible but read-only during work.

Listening is stopped only when an operation actually restarts audio. The GUI
remembers the endpoint and restarts the monitor afterwards. Settings-only
applies and noise-profile captures leave it running.

## Module ownership

| Area | Files |
| --- | --- |
| App logic, messages, commands, worker, meters | `gui/main.cpp` |
| Install, registry, binding, backup, isolation | `gui/apo_ops.cpp`, `gui/apo.h`, `gui/apo_internal.h` |
| Endpoint enumeration and file status | `gui/apo_status.cpp` |
| Listen monitor | `gui/apo_monitor.cpp/.h` |
| Noise capture request/result channel and tap | `src/stage_capture.cpp/.h` |
| `show_all_devices` and `last_device` preferences | `gui/preferences.cpp/.h` |
| Control ids and shared HWNDs | `gui/ui/ui_state.*` |
| Control tree and geometry | `gui/ui/structure.cpp/.h` |
| Widget factories and custom drawing | `gui/ui/widgets.cpp/.h` |
| Effect graph control | `gui/ui/graph.cpp/.h` |
| Menu bar | `gui/ui/menu.cpp/.h` |
| Palette and native theming | `gui/ui/theme.cpp/.h` |
| Effect picker and Info dialogs | `gui/ui/effect_dialog.cpp`, `gui/ui/info_dialog.cpp` |

To add a control: add its id and `HWND` to `ui_state`, create it in
`structure.cpp::CreateControls`, add layout in `LayoutEffects`/`Layout`, and
wire its command in `main.cpp`. To change how a control looks, edit its widget
factory or subclass in `widgets.cpp`.

## More detail

- [Views and layout](views.md)
- [Widgets and controls](widgets.md)
- [Theming and repainting](theming.md)
- [DLL ↔ GUI settings sharing](../runtime-sharing.md)
