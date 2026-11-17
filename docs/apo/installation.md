# Installation and registration

The GUI owns installation. There are three entry points:

- **File → Install / Update** — manual maintenance.
- Checking **Enable** on a device — installs or updates automatically, then
  binds the device.
- Unchecking **Enable** on the last enabled device — uninstalls automatically.

The GUI requests elevation; all registry and service operations need it.

## Packaging

CMake stages the built `better_micro_apo.dll` into `build/apo_embed/`, generates
a resource script from `gui/apo_dll.rc.in`, and touches it after every DLL build
so the resource compiler picks up the new bytes. The DLL is embedded in
`better_micro_apo_gui.exe` as an `RCDATA` resource (`IDR_APO_DLL`), so the EXE
is the only file that needs to be distributed. A standalone DLL is still built
for debugging. The DLL uses the static MSVC runtime and has no third-party
dependencies.

## Files and identity

| Item | Value |
| --- | --- |
| CLSID | `{6A8BE200-CD89-4E77-97F5-D51E8F16EF90}` |
| Install directory | `C:\Program Files\better_micro_apo` |
| Installed DLL | `C:\Program Files\better_micro_apo\better_micro_apo.dll` |
| Log | `C:\ProgramData\better_micro_apo\better_micro_apo.log` |
| Companion guide | [Custom APO Installer Guide](https://gist.github.com/godlikeswan/5152c5594d79e50980ad2cf831d3f06d) |

`FilesStatus` (`gui/apo_status.cpp`) tracks:

- `dllPresent` — the installed file exists.
- `dllUpToDate` — its size and FNV-1a hash match the embedded payload.
- `comRegistered` — the CLSID is registered.
- `apoKeyPresent` — the APO registration key exists.
- `registeredPath` — the path the COM class points at.
- `embeddedHash` / `installedHash` — 64-bit FNV-1a hashes, also shown in
  **About → Info**.

`FullyInstalled()` requires the DLL, the COM class, and the APO key.
`DllStale()` is true when a DLL exists but differs from the embedded build.

## Install / Update

`apo::InstallFiles()` (`gui/apo_ops.cpp`):

1. Checks whether this is a fresh install or an update.
2. Creates the install directory and extracts the embedded DLL to the installed
   path (with retries for file locks).
3. Calls `DllRegisterServer` on the installed DLL to register the COM class.
4. Re-reads `FilesStatus` and fails if registration did not complete.
5. Migrates bindings written by older builds into the endpoint slot
   (see [Device binding](device-binding.md)).
6. Restarts the audio service so the engine reloads effect chains.

Audio stops for about a second. The GUI keeps effect settings, backups, and
logs under `C:\ProgramData\better_micro_apo`, so updating the DLL does not lose
configuration.

## Uninstall

`apo::UninstallFiles()`:

1. Restores the registry state of every device that has a canonical backup and
   removes leftover legacy/endpoint values.
2. Calls `DllUnregisterServer` and removes the CLSID key.
3. Deletes the install directory.
4. Restarts audio.

Settings, noise profiles, runtime snapshots, backups, and logs stay in
ProgramData. A later **Enable** installs the DLL again and uses those settings.

## Audio service handling

`StopAudio`/`StartAudio` open the service manager, collect `Audiosrv`
dependents, stop them in dependency order, and start them again afterwards, with
state waits and a scope-exit guard so a failed step still restores the service.
This is why binding, install, uninstall, and noise-profile capture briefly
interrupt audio. Settings-only applies avoid the restart by going through the
[runtime snapshot](../runtime-sharing.md) instead.

## Status and log

Setup calls append to `C:\ProgramData\better_micro_apo\better_micro_apo.log`
and to `OutputDebugStringW`. `APOProcess` never logs. **About → Open log** opens
that file; **About → Info** shows a snapshot of hashes, install path and DLL
file name, CLSID, registry key states, and `Applied to N devices`.
