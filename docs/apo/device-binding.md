# Device binding

Binding is a GUI operation (`gui/apo_ops.cpp`). It writes registry values under
the endpoint's `FxProperties` key and then restarts audio once so the engine
rebuilds the effect chain.

Registry root for capture endpoints:

```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{endpoint-guid}\FxProperties
```

The property prefix is `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}`. Constants live
in `gui/apo.h`.

## Two kinds of devices

The GUI checks whether the driver publishes composite FX entries. That decides
which key holds our CLSID:

| Device | Property | Index | Type | Value |
| --- | --- | --- | --- | --- |
| No effect pack | `PKEY_FX_EndpointEffectClsid` | `...,7` | `REG_SZ` | The APO CLSID. |
| Effect pack | `PKEY_CompositeFX_EndpointEffectClsid` | `...,15` | `REG_MULTI_SZ` | The CLSID appended to the driver's endpoint chain. |

On effect-pack devices the driver's endpoint entries are preserved while the
APO is enabled and restored from the backup on disable. Legacy slots are no
longer used for new binds.

## Why endpoint effects

Windows applies endpoint effects even to raw streams, but skips stream effects
on them. Chromium/Electron apps such as Discord and WhatsApp open the
microphone raw when the endpoint reports raw-processing support, so a
stream-slot binding silently stopped processing there.

This change came from commit `f027c3a` ("Bind as an endpoint effect and add a
dynamic effects list"): every device is now bound as an endpoint effect. Older
builds used the PreMix slot (`...,1`) and later the composite stream chain
(`...,13`). The bind path removes this APO from those old slots:

- `PKEY_FX_DefaultEffect` / PreMix — `...,1` (`REG_SZ`)
- stream — `...,5` (`REG_SZ`)
- composite stream — `...,13` (`REG_MULTI_SZ`)

An endpoint found only through an old slot still counts as *enabled* in the UI,
but is marked pending migration: **Enable** moves it to the endpoint slot, and
**Install / Update** runs the same migration while audio is stopped. Without
this, a migrated device would have the APO in two chains and be processed
twice.

## Isolation

While the APO is enabled on a device, other effects are ignored
unconditionally. There is no per-device option and no migration of an old
setting: `ApoSettings::isolateEffects` is always forced on by `ClampSettings`.

Isolation is a desired state, not just a transition. On every apply the GUI:

- keeps this APO's entry in the composite endpoint chain (`...,15`);
- drops the driver's other entries from that chain;
- clears the other composite chains: mode (`...,14`), keyword detector
  (`...,16`–`...,18`), and offload (`...,19`–`...,20`).

If a driver republishes those chains, the next apply clears them again.
Disabling or uninstalling rebuilds the original chains from the backup.

## Backup and restore

Before changing any slot, the GUI exports the whole `FxProperties` key with
`reg.exe` to one canonical file per endpoint:

```
C:\ProgramData\better_micro_apo\backup\FxProperties-{endpoint}.reg
```

While that file exists, binds reuse it instead of taking another snapshot. A
successful restore moves it into `backup\old_backups` with a timestamp so the
next bind records the freshly restored driver state. Older builds left one
timestamped file per bind; the first bind adopts the best of those (one that
still contains a driver chain, then one with no chain, never one holding only
this APO) and archives the rest.

Restore parses the backup, puts back the original endpoint, PreMix, and stream
values plus every composite chain, and removes only this APO's CLSID from
anything it did not write.

## Restart behavior

Binding, unbinding, isolation changes, and migration require a restart so the
engine reloads the endpoint's effect chain. A settings-only change does not:
it is published through the [runtime snapshot](../runtime-sharing.md). The
first settings apply on a device with no prepared snapshot still needs a
restart, because a stream locked before the file existed has no mapping to
poll.
