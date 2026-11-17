# Documentation

Better Micro APO has two halves: the capture APO DLL (`src/`) and the Win32 GUI
that installs, binds, and configures it (`gui/`). These docs describe both and
the channel they share at runtime.

## Start here

- [APO overview](apo/overview.md) — what the DLL does, its interfaces, formats,
  and where its files live.
- [GUI overview](gui/overview.md) — window lifecycle, events, background work,
  and module responsibilities.
- [Effect reference](effects/index.md) — the stage interface and every effect
  with its parameters and implementation.

## APO

- [Installation and registration](apo/installation.md) — embedding, COM
  registration, file locations, hashes, update, and uninstall.
- [Device binding](apo/device-binding.md) — legacy and effect-pack registry
  keys, raw-stream behavior, isolation, backup/restore, and migration.
- [APO operation](apo/operation.md) — `Initialize`, `LockForProcess`,
  `APOProcess`, buffers allocated at bind time, and the DSP chain.

## GUI

- [Views and layout](gui/views.md) — the single screen's control tree, dialogs,
  and geometry flow.
- [Widgets and controls](gui/widgets.md) — factories, custom-drawn controls,
  graph, icons, tooltips, and where to change their appearance.
- [Theming and repainting](gui/theming.md) — dark-mode workarounds and the
  update/redraw pitfalls behind the current design.

## Shared state

- [DLL ↔ GUI settings sharing](runtime-sharing.md) — the per-device runtime
  snapshot file, seqlock, and live apply path; also covers the INI and noise
  profile formats.

## Background

- [Custom APO Installer Guide](https://gist.github.com/godlikeswan/5152c5594d79e50980ad2cf831d3f06d)
  — general Windows APO installation notes that predate this project.
