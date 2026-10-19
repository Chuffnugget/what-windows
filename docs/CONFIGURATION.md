# Settings and configuration backups

## Local settings

WHAT stores its live profile in `settings.yaml`, using QSettings with a YAML backend.
The startup log shows its location. Window state, module permissions, pairing identity,
tray icons, display recovery data and updater preferences all use this file. Existing
background saving and atomic file replacement are retained.

On Overview or the Modules page, drag the dotted grip beside a module to change its
position. Both pages use the same order.
You can also focus the grip and press Alt+Up or Alt+Down. The order is saved under
`ui/module_order`, restored on startup and included in configuration backups.

On first startup after upgrading, WHAT converts `settings.ini` and renames the original
to `settings.ini.migrated` after checking the YAML. A queued configuration import is
moved too. The archived INI is never used as a fallback. Once converted, a missing or
invalid YAML file stops startup with an error instead of resetting the profile.

Quit WHAT before editing the file. It uses normal YAML mappings and lists under the
`settings` key; slash-separated names are QSettings groups. Keep `format: WHAT settings`
and `version: 1`. WHAT rewrites the file when saving, so custom comments are not retained.
Binary values use YAML's `!!binary` notation. Pairing tokens and MQTT passwords keep
their existing Windows account protection; changing the file format does not decrypt them.

The local file contains private identity and cached data. Use **Export Configuration**
for a portable backup that omits those values. A complete profile backup should also
include the other files in the profile directory, such as its TLS identity.

## Import and export

Open **Settings > Configuration backup** to export a `.yaml` file or import one. Imports are previewed before you confirm them. Large previews are limited to 64 KiB and explicitly ask you to review the full file in a text editor. Nothing is uploaded.

1. Choose **Export configuration** and save the file somewhere private.
2. To restore, choose **Import configuration** and review the listed values.
3. Tick **Also restore module, collection, sharing and control permissions** only if you want those settings and shared sensor selections restored too. Otherwise your existing permissions and sharing selections stay unchanged.
4. Choose **Import on next start**, then quit WHAT from its tray menu and reopen it. Closing the window only hides it and does not apply the import. Settings listed in the import replace their saved values; other settings remain unchanged.

Only one import can be queued; confirming another replaces it. The file is validated before it is queued and again before applying. Invalid imports leave the current settings unchanged and report the reason in the activity log.

## What is included

- Module enablement, collection options and Home Assistant control permissions (restored only with the permissions checkbox).
- Selected sensors, per-sensor Local/Cloud/Fast update policies, favourites and collapsed categories.
- Sampling/publishing intervals, startup presentation, animation timings and update preferences.
- Tray sensor assignments, colours and fonts.
- Valid Home Assistant remote addresses and MQTT broker host, port, TLS choice, scope and username.

Defaults that have never been explicitly saved may be absent. Sensor IDs refer to the original hardware; importing onto another PC does not recreate hardware or guarantee that all saved sensor IDs exist.

## What stays private or local

The file excludes passwords, pairing tokens, device identity, TLS keys, signing identities, certificate/key paths, executable paths, live readings and diagnostic caches. Windows sign-in registration, saved display layouts and iCUE LED-group definitions are not migrated. This is a portable settings export, not a complete profile clone.

Imported Home Assistant remote connections are disabled until explicitly enabled again. Imported MQTT settings are disconnected, with the existing MQTT password removed and certificate paths cleared, so old credentials cannot be sent to an imported broker. Enter the required credentials and certificate paths and select MQTT again when ready. Existing pairing identity is preserved; a new PC still needs its own pairing.

The YAML is not encrypted. It contains addresses, usernames and sensor names, so review it before sharing. Before applying an import, WHAT saves `settings.yaml.before-import` beside the current settings. That rollback copy contains the original local profile settings, including Windows-protected credential blobs, and must remain private. For rollback, quit WHAT, remove any pending `settings.yaml.import.yaml`, and restore the backup as `settings.yaml` on the same Windows account. Keep a separate copy before replacing either file.

## File format

Schema version 1 uses a deliberately limited YAML 1.2 format, with JSON flow syntax for values. Keep the three header lines in order and quote setting names. Arrays and objects are written on one line; strings use JSON double quotes and escaping. Comments and blank lines are accepted. General YAML features such as aliases, tags, block strings and unquoted strings are not supported. Unknown settings, duplicate names, invalid types, out-of-range values and files over 4 MiB are rejected.

```yaml
format: "WHAT configuration"
version: 1
settings:
  "start_minimised": true
  "ui/timing/panels": 180
  "favourites": ["example_sensor_id"]
```

Prefer editing a WHAT-generated export so the setting names match this version. Re-importing changes only the listed supported settings. Export/import work runs on a worker thread; the queued settings merge occurs during startup before modules and connections are initialised.
