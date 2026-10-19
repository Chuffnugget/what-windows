# BEACN module

Enable Modules > BEACN in WHAT. Keep the official BEACN app running. Enable
Home Assistant control separately when you want remote commands. Cloud state
and control are disabled unless explicitly enabled. The module defaults off.

Native app controls target BEACN 1.4.2.12 with the 6.16.0.0 x64 driver (API 5.16).
The module shows the detected app and driver versions, including unsupported
installations.

Unrecognised builds undergo native compatibility checks, followed by live mixer
checks. Compatible builds can work and show a warning with the result. A changed
executable interface still needs a bridge update: WHAT does not call guessed
addresses. Driver readings, such as sample rate, remain available independently.
Failed live checks name the affected control groups in the module and Overview.

Auto-detect prefers the running app over an older copy elsewhere. Missing files,
unreadable files, unsupported versions, process access failures and unrecognised
live mixer layouts have separate messages. Reconnect repeats file verification.
Connection and verification changes are recorded once in the activity log.

Supported controls: linked Personal/Audience volume, knob mute, audience button,
submix, previous/next knob page, Personal A/B devices and active selection,
Audience output, live profile, four default-device assignments, automatic default
reset, screen theme, screen/button brightness, and dim timeout. Physical/app
channel reordering is reflected using stable source IDs. Assignment selections
remain independent of automatic reset; observed Windows defaults are separate.

Connection checks, readings and commands run on a worker thread. WHAT checks
the connection and permissions again before applying a command. A busy module
rejects additional commands. Failed reads reconnect after three seconds. Commands
are not automatically repeated; check BEACN after an uncertain result before
using Reconnect.

Session logs are stored in the profile's `logs/beacn` folder for seven days, with
a limit of 30 files. Per-second volume CSV and state JSON exports are written
only when verbose BEACN logging is enabled.

The `beacn_probe` build target checks the installed bridge without changing
controls. For a deliberate hardware test, `beacn_probe report.json --roundtrip`
briefly lowers Game volume by 0.25 percentage points and changes screen/button
brightness by one step, restoring each original value and checking readback.
`beacn_probe report.json --inspect path/to/BEACN.exe` checks file compatibility
without starting the driver or loading that executable.

The HA integration adds number, switch, select and button entities dynamically,
plus a connection status sensor. Channel readbacks and observed Windows defaults are normal browser sensors, shared individually using their Local and Cloud checkboxes.
Keep the Windows app and Home Assistant integration compatible. After updating
the integration, restart Home Assistant. Existing pairing details are retained.

Independent unlinked fader writes, Aux mapping, mute configuration editing,
per-app routing, microphone DSP/EQ, and other unmapped settings are not supported.
Unlinked channel volumes remain visible but their linked-volume control is unavailable.

Snapshots older than 15 seconds become unavailable; a fresh readback restores them.

## Where to find BEACN

Modules > BEACN contains connection status, editable application and driver paths,
Browse, Auto-detect and Reconnect, permissions and logging information. Discovery
fills the path fields automatically. Manual edits are verified before connecting.
The cyan Connected badge means live BEACN app controls are ready; amber means
discovering or unavailable, and grey means disabled. All module headers use text
and colour together for connection status.

Sensors has a BEACN filter, inline mixer/app controls and readbacks. Channel
readbacks include independent Personal/Audience levels, mute state and linking;
app readings include page, dimming, sample rate and observed Windows defaults.
Use the normal Local/Cloud checkboxes, favourites and category controls for
readings. Home Assistant number/switch/select/button entities use the module's
separate control permission. The module's cloud permission gates all BEACN data
on cloud transports, including individually selected readings.

## Installed bridge loading

The official BEACN process cannot load WHAT's hook directly from WindowsApps on affected installations. WHAT stages the exact bundled DLL under its per-user data directory, `bridges/beacn/<SHA-256>/`, verifies the copied bytes, and loads that copy for both volume and global controls. Different helper versions use separate directories. No vendor binaries are copied and no elevated bridge process is required. Missing or corrupted copies are repaired from the bundled helper; a verification failure prevents dispatch.
