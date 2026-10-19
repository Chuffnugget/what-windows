# Using WHAT

## Install, start and pair

1. Use the Setup EXE from the Windows repository's Releases page. This is the recommended edition for Windows notification access and in-app update installation. Portable and Qt Creator copies can check releases but cannot install an MSIX update themselves.
2. Install the current companion WHAT Home Assistant integration. With the HA API service running, look for WHAT under Home Assistant's discovered integrations, or add it manually using the address and port under **Overview > Pair this PC**. Pair using the token and certificate fingerprint shown there.
3. Enable the modules you need under **Modules**, then select readings in the sensor browser. Collection, Local/Cloud sharing and remote-control permissions are separate choices.
4. Settings includes startup, connection and update preferences. A new profile starts minimised by default; click the tray icon to open it. Closing the window keeps WHAT running; **Quit WHAT** stops it. Enter confirms the quit prompt and Escape cancels.
5. Keep the existing Windows profile and Home Assistant entry when updating. Choose releases through the installed WHAT app; the release picker supports explicit upgrades and downgrades. Back up your profile before downgrading.

## Home Assistant Installations

Open **Settings > Home Assistant Installations** to add, name, edit, enable,
disable or delete connections. Each installation uses **Home Assistant API** or
**MQTT**. Home Assistant API is recommended for most users because no broker is
needed. MQTT can share an existing broker with tools such as Zigbee2MQTT.
Enabled installations run independently and receive the same selected readings for their Local or Cloud scope.

The existing connection keeps its pairing token and PC identity. A previously
configured alternate route is retained as a disabled entry with a separate token.
Review and pair it before enabling it for a different Home Assistant installation.

- **Home Assistant API:** leave the address blank for Home Assistant to connect to
  the PC directly, or enter its HTTPS address for an outbound connection from WHAT.
- **MQTT:** enter the broker used by that Home Assistant installation. Choose Local
  sharing only for a trusted broker. Cloud requires TLS. See [MQTT](MQTT.md).
- **Allow Controls From This Installation:** turn off for readings only. Turning
  it on still requires the relevant module and command permissions in WHAT.

You can also add and edit installations from **Overview > Pair This PC**. Adding
an installation selects it automatically; editing keeps its token and identity.
Choose an installation, then use **Copy Pairing Details**. In Home Assistant, add
WHAT and choose **WHAT API** or **MQTT** to match the selected connection.
For WHAT API, enter the PC address and port for local access, or leave the PC
address blank and configure Home Assistant's HTTPS address in WHAT for remote
access. MQTT uses the broker already configured in Home Assistant's MQTT integration.
Both methods use the copied PC device ID, name, token and fingerprint.
Readings appear after the PC authenticates.
Configure both routes in the same installation and choose **Active Connection**.
Only the selected route carries the live stream and accepts commands. WHAT checks
a replacement before releasing the existing route, with a 30-second deadline.
Failure retains the previous connection and its settings. The integration
applies the change without re-pairing, reloading or changing entity IDs. Use one
entry per Home Assistant server; different servers may share an MQTT broker.

The overview lists each installation and its last response. Each has its own ECG
trace, using the same colour as its status dot. Colours persist through renames
and restarts. Each trace overwrites only its own history as it sweeps. Sensor
collection runs once; networking and per-installation filtering remain on the
network worker thread.

Every paired Home Assistant also shows a **Connection - Name** diagnostic for each
configured installation under the WHAT PC device. Its attributes include the
transport, sharing scope, connection start and last reply. If that Home Assistant
loses contact with WHAT, these reported states become unavailable until it
reconnects. Addresses and credentials are not shared.

Disabling preserves settings. Deleting revokes that installation's token and
closes its connections, but keeps its Home Assistant entities and history.
Renaming does not change entity IDs. Start and Stop control all installations.
Cleanup requires a selected installation with a Local connection and affects only
that installation's registry.

To update existing pairing credentials, select the installation in **Pair This PC**
and use **Replace Token**. Only its token changes. Copy its new pairing details,
then use **Reconfigure** on the existing WHAT entry in that Home Assistant. Keep
the entry to retain its devices, entities and settings. **Replace All Pairing
Tokens** in Settings > Pairing and Security still replaces every installation's token.

**Renew Shared Certificate** changes this PC's pairing certificate for all
installations, using either WHAT API or MQTT. Update the fingerprint in each
Home Assistant's existing WHAT entry. Tokens, the PC identity and sensor selections
are kept. This does not renew MQTT broker or client certificates.

Configuration exports contain connection settings, but no tokens or passwords.
Imported connections start disabled for review.

## Home Assistant Web

Module settings show **Unsaved Changes** until you click **Apply**.
**Revert Changes** restores the saved options. Site permissions and toolbar zoom
changes save immediately, as described beside those controls.

Enable **Modules > Home Assistant Web**, enter your dashboard's HTTP or HTTPS
address, then select **Apply**. Use a local address or your Home Assistant Cloud
address, whichever this PC can reach. Sign in through the dashboard itself.
This login is separate from WHAT's integration pairing.

Choose **Display Mode** in the module settings:

- **Inside WHAT** shows the dashboard on its own page.
- **Fullscreen Overlay** opens above your other apps on your chosen monitor, or
  the monitor under the pointer when set to Automatic.
- **Desktop (Keep Taskbar)** starts with WHAT, covers the wallpaper and desktop
  icons on one monitor, and stays behind other apps. The taskbar stays accessible.
  With Automatic monitor selection, **Open Dashboard** moves it to the monitor
  under the pointer and remembers that monitor. Changing mode or disabling the module reveals
  your normal desktop. WHAT does not change your wallpaper or delete any icons.
- **Picture-in-Picture** opens a smaller window above your other apps. Drag its
  title bar to move it, or any edge or corner to resize it. Its size and position
  are saved in settings.yaml and kept within the available monitor area when reopened.
  The up arrow collapses the toolbar to a thin strip; click the strip to reveal it.
  The minus button rolls the window up to just its title bar. Restore brings back
  its previous size without reloading the dashboard. Navigation, zoom, audio,
  Web settings and window controls are directly on the title bar.

The global hotkey defaults to **Ctrl+Alt+H**, including while WHAT is hidden in
the tray. It toggles picture-in-picture when that mode is selected, otherwise
fullscreen. **Escape** gives Home Assistant's dialogs and menus priority, then
closes the overlay. The close button or hotkey always dismisses it. Desktop mode
returns the dashboard to your desktop afterwards. All views share one browser
session. Moving it keeps the current page unless you set a different dashboard
for that view under **Dashboards by View**.
Change the hotkey if another application already uses it, or leave it blank to
disable it. Desktop mode runs while WHAT is running, so enable WHAT's startup
option if you want it to appear when you sign in to Windows.

**Picture-in-Picture Hotkey** defaults to **Ctrl+Alt+P** and always toggles PiP,
regardless of Display Mode. Both shortcuts are configurable; use distinct
combinations or leave either blank to disable it. An unavailable shortcut shows
a warning in the module settings.

The system tray menu includes **Home Assistant Web** when this module is enabled.
It can open any view, hide an overlay, or minimise/restore PiP. Choosing a view
there does not change the saved startup mode. **WHAT Tray Icon Click**, in the
Web module settings, assigns a Web view to a left click on WHAT's main icon.
The default is **Open WHAT**, which also remains in the right-click menu. If Web
is disabled or has no address, left-clicking opens WHAT.

**Render Scale** changes dashboard zoom from 50% to 200% without resizing WHAT's
controls. The Web toolbar also has a zoom control; changes apply immediately and
are saved in the same setting without reloading the dashboard.
PiP uses its own **PiP Dashboard Zoom**, adjustable in **Display and Audio** or
with the title bar's minus and plus buttons. **PiP Window Size** changes the
floating window separately: 100% means 640 by 480, adjusted to fit the monitor
and selected aspect ratio. Manual resizing is remembered until you change this
setting again.

Click WHAT's navigation edge to collapse it to icons or restore its saved width.
The edge thickens when hovered and can also be dragged to resize navigation.
Width and collapsed state are saved in settings.yaml. When focused, the edge
accepts arrow keys to resize and Enter or Space to toggle. Collapse and expand
use the panel animation timing.
Home Assistant's desktop sidebar inside Web has its own draggable edge. Its
width is saved in WHAT; Home Assistant's menu button still controls expansion.
Narrow layouts keep Home Assistant's normal slide-out menu. This adjustment
depends on Home Assistant's frontend layout and leaves other websites alone.
**Clear Cache** removes cached files and service workers, then reloads
the dashboard while keeping your login. **Clear Login and Site Data** asks for
confirmation before also removing cookies and local storage. Neither operation
changes WHAT settings, sensor selections or integration pairing.

The module uses Microsoft's Edge WebView2 Runtime. If it is missing or damaged,
use the **WebView2 Runtime** link in the module settings to install or repair it.
Desktop mode opens the browser at startup. Other modes start it when first opened.
When hidden, WHAT mutes the browser and asks WebView2 to suspend it. **Keep Audio
Playing When Hidden** leaves it running instead, which can use more resources.
**Mute Dashboard Audio** and the toolbar mute button silence it in every view.
Disabling the module closes the browser. Login data stays in WHAT's profile under
`home-assistant-web`; it is excluded from configuration exports.

Additional settings:

- **Connection and Recovery** can use an HTTPS cloud address when the primary
  dashboard has a network or server failure. It does not switch on certificate
  or authentication errors. This is independent of WHAT's API and MQTT routes.
  Cloud and local addresses may require separate sign-ins. Reload or Home retries
  the primary address. Stalled navigation stops after 30 seconds. Automatic
  recovery tries after 2, 5 and 15 seconds, then
  stops until you reload. A minute of stable browsing resets the retry allowance.
- **Display and Audio** selects desktop and overlay monitors, PiP edge snapping
  and an optional window aspect ratio. A disconnected monitor falls back to an
  available one without forgetting your preference. Auto-hide removes the toolbar
  after 2.5 seconds; move to the dashboard's top edge or press **F6** to reveal it.
  Toolbar keyboard focus keeps it visible.
- **Dashboards by View** accepts paths such as `/dashboard-desktop/home` or full
  HTTP(S) addresses for desktop, fullscreen and PiP. Blank uses the main dashboard.
  Relative paths follow the current local/cloud address. Full addresses on another
  server stay on that server.
- **Site Permissions** reads saved microphone, camera, location, notification and
  clipboard decisions, plus other permissions requested by sites. Ask, Allow and
  Block are saved immediately in WebView2's profile. **Reset to Ask** removes saved
  decisions without clearing your login. Reload the page after changing permissions.
- **Browser Diagnostics** shows the runtime version, current host, visible/hidden/
  suspended state, recovery attempts and last error. The main diagnostics copy
  includes browser state and version without the dashboard hostname.

User-initiated links to other sites open in your default browser. Dashboard links
and sign-in redirects stay inside WHAT. **Open in Browser** opens the current page
externally; that browser uses its own login. Automatic pop-up windows are blocked.

## Appearance

Choose **Settings > Appearance > Theme** for Blue (Default), Dark Grey, Slate,
Forest or Plum. The additional palettes use muted colours, while the WHAT logo
and warning colours stay the same. Changes apply immediately and are saved in
`settings.yaml` through QSettings, including in configuration exports.

### UI timing

WHAT's main window and prompts open and close immediately. Prompts keep their
glass backgrounds. Sections and sensor rows slide open and closed, and each
page slides vertically together with its title and warning banners.

The contents of sections, sensor rows, menus and helper text soften as
they close and become sharp as they open. Outer backgrounds, borders and sensor
selection strips stay sharp. Blur follows the existing animation timing and
switches off at rest. It requires hardware rendering.

Open **Animation Timings** to adjust durations in milliseconds. Panels and list
rows use Expand / Collapse and List Movement. Page Changes controls the whole
page, including its title. Tooltips use **Helper Text Open / Close**, which defaults
to 350 ms. Higher values make the animation slower; the 600 ms hover delay is
separate. **Reset Defaults** resets every timing, including custom values, and
returns helper text to 350 ms. **Menus Open** and **Menus Close** adjust menu timing.
Minimising and maximising use Windows' own animations. Separate colour and file
dialogs, native tray menus and embedded browser content keep their own rendering.
Software rendering uses Standard transitions because it cannot run shader effects.

Zero makes a transition immediate. Disable **Enable animations** to turn off
motion throughout WHAT. Effects run only during a transition, with no shader
layer kept at rest. Sensor collection intervals and network timeouts are
separate from these visual durations. Menus open in 250 ms and close
in 180 ms by default. Sections expand and collapse in 250 ms. Custom timings
remain unchanged.

Opening a settings section or module closes the other sections at the same
level. Nested sections leave their parent open. Search filters the list without
opening every match. The sensor list's accent strip becomes pale when a row is
expanded, so the active row remains easy to find.

Prompts appear over a still glass background. The sensor list shows a loading indicator while its first rows are being prepared; existing rows stay visible during refreshes. Keyboard navigation shows a focus outline; mouse clicks do not.

Tooltips use the Helper Text Open / Close timing for their entry and exit animation. Expanded
section headers have a subtle background highlight in every theme. Expanding
sections scroll into view as they open. Drag page backgrounds or list rows to
scroll; text selection and slider gestures keep their usual behaviour.

Saved commands show the time since their last run in the current WHAT session.
Hover over that time for the date and time of the run. The Local, Cloud and
Can Run badges describe Home Assistant access; they do not block local runs.

Settings and Modules keep their search controls above the scrolling content.
Mouse-wheel scrolling uses the same behaviour across pages. Nested output panes
scroll first, then pass the wheel to the page when they reach an edge.

On Taskbar, click an icon row to open its settings. Opening another row closes
the previous editor. The two default WHAT icons remain at the top.

Overview shows the time since Home Assistant last answered WHAT, with an ECG pulse for each
confirmed reply. Replies are checked every five seconds over the paired API or
MQTT connection. The graph fills the row beside the response label. A wider window
shows more history at the same sweep speed, with at least 15 seconds per pass.
The tooltip shows the current duration; resizing applies it on the next pass. The old
trace stays in place until the tip clears and overwrites it on the next pass.
Missed replies draw a flat line; the sweep keeps moving without inventing pulses.
Beat shapes vary slightly for appearance; their spacing follows actual reply timing.
After 12 seconds without a reply the trace dims. Disconnecting dims it further
while retaining its colour; the sweep overwrites existing beats with a flat line. This requires
the companion integration with UI feedback support; sending readings alone does
not count as a Home Assistant response.

**Pair This PC** starts with the address and port hidden. **Hide Address** only
changes their visibility for the current session and is not saved in settings.

## Windows categories

Sensor categories and subcategories start collapsed each time WHAT launches.
Your expansion choices last for the current session only. Click and drag a row
to scroll, or use the mouse wheel or scrollbar.

The sensor browser uses compact rows. Click a reading to expand its details and controls inside the list; click it again to close them. Opening another row closes the previous one. Local, Cloud and Fast Updates can be changed without opening the row. The star marks favourites without changing sharing. Search, module filters and the sharing filter narrow the list; **Share…** opens bulk actions. Colour changes in an expanded lighting row are sent when you click **Apply Colour**. Row expansion follows the expanding-panel duration in UI timing.

Expanded entries show their registered HA entity IDs. Click an ID to copy it;
the label briefly confirms the copy. IDs come from HA's registry, including any
renames. An unshared entry or a control used only on this PC may have no HA entity.

Search text is cleared when WHAT starts. Use the **×** at the right of the search box to clear it during a session. **Pair this PC** on Overview also starts collapsed and opens when you click its heading.

| Category | Readings / behaviour |
| --- | --- |
| System | UTC timestamp, local timezone/offset, uptime including sleep, awake uptime, estimated last kernel boot, OS edition/display version/build/UBR/architecture |
| Storage | Local mounted volume capacity, used/free/user-available bytes, used %, filesystem, label, mount, removable and connected state |
| Memory | Physical RAM, commit used/limit/peak, actual page-file allocated/current/peak usage per file and totals |
| Audio | Default playback endpoint, master volume and mute; optional microphone endpoint/volume/mute, without recording |
| Session | Windows user, idle time, lock and remote-session state; optional foreground process and separately enabled window title |
| Power | AC, battery presence/charge/charging and estimated remaining time when Windows supplies them |
| Network | Adapter connectivity, IPs, link speed, byte counters/rates, Wi-Fi SSID/signal when permitted by Windows |
| Windows Security (optional) | Registered antivirus, firewall and automatic-update settings health; read-only, refreshed once a minute |
| Events (optional) | System/Application critical/error counts over the last 24 hours, latest source/ID/time, optional message excerpt |
| Media (optional) | Current system media session application/playback state, optional track title/artist |
| Notifications (optional) | Current toast count, latest app/time, separately enabled title/body excerpt; Windows package/permission required |
| App health | App uptime, client count, repeat connections, last snapshot queued, per-source health and console display state |

Storage is rescanned continuously and after device-change notifications. New
drives automatically share connected/used-% readings by default; turn this off
in Settings if unwanted. Local volume GUIDs preserve identity across drive-letter
changes/restarts. Unplugged drives report disconnected; their capacities become
unavailable. Network shares without local volume GUIDs are excluded. No physical
disk SMART/partition management is provided by the Windows collector.

New profiles poll Windows every 1 second; Modules supports 500 to 60000 ms for Windows polling.
Existing polling preferences are retained. HWiNFO is checked every 100 ms, but
its own sensor-update period still limits freshness. Lock/display/output-volume
events are pushed immediately; other changes are batched within 50 ms. The
connection heartbeat defaults to 5 seconds and does not delay changed readings.
Storage has its own worker and refreshes every 10 seconds and on device changes.
Event logs refresh about every 60 seconds; media/notifications about every 15 to 20
seconds. Microphone and foreground app/title remain polled, subject to privacy settings.
Slow collectors and network I/O use separate QThreads.
Event scans cap at 2,000 matching events per channel: counts become unavailable
with a truncation status instead of silently undercounting. Windows/permission
failures are unavailable, never invented zeros. Fast Startup can preserve kernel
uptime. Console display state waits for a Windows power-state notification.
The default audio endpoint is not a sum of individual application volumes.
Page-file usage and commit are different measurements. RAM/page-file readings
start unselected to avoid automatically duplicating HWiNFO metrics.

Windows titles, notification contents, event messages and track metadata are
off by default. Optional collectors are individually enabled. Enabling collection
does not select every reading for HA; select the desired rows in Sensors.
Once transmitted, HA may retain sensitive values in recorder history. Turning
an option off masks cached/in-flight values and stops further sharing; it cannot
erase existing HA history.

## Optional notification installation

Most features work in the portable EXE. Windows notification access needs package
identity and explicit permission. See `INSTALLATION.md`. Run the matching Setup.exe
to install the app; Setup explains the signing certificate and requests administrator
approval if it is not already trusted. The in-app updater never imports certificates.
Media access may also depend on Windows capabilities/session permissions; its status
explains unavailability.

## Page Navigation

Each page, including its heading and warning banners, slides vertically as one
unit. Adjust **Page Changes** in Animation Timings, set it to zero for an immediate
change, or turn off animations.

## HWiNFO setup and licence

Open **Modules > HWiNFO > HWiNFO Setup** for the shared-memory setup steps.

WHAT is free and open source. Its developer receives no money from HWiNFO
licences. HWiNFO imposes its own free-edition shared-memory limit; this app does
not sell, activate, bypass or automatically reset a HWiNFO licence.

For optional hardware readings: open HWiNFO's startup window → Settings →
General / User Interface → tick Shared Memory Support → OK. Choose Sensors-only
and Start / Run, or open Sensors from its main window. Keep HWiNFO running;
minimising is fine. If it skips startup, exit from its tray icon and relaunch.
This is the general setting, not the sensor-customisation gear.

Free HWiNFO sharing expires after 12 hours; manually re-enable it or use HWiNFO
Pro. Selecting Pro immediately hides all licence reminders, as does disabling
HWiNFO. Real source errors remain visible. Shared memory has no reliable Free/Pro
indicator: the edition is self-reported. A reader error is not evidence that a
licence purchase is needed. See https://www.hwinfo.com/licenses/.

## Corsair iCUE module

In Modules, enable **Corsair iCUE**. Start iCUE and enable software integrations in its Settings →
SDK page (Software and Games on some versions). Allow WHAT when iCUE lists
it. The module reports connection errors and device counts directly on its card.
Then enable the separate HA control checkbox to allow lighting/property changes.

HA discovers Corsair devices beneath the PC, with one whole-device RGB light and
a Return lighting to iCUE button. Individual LED light entities start disabled:
open the Corsair device in HA, show disabled entities and enable the LED IDs you
need. IDs come directly from the SDK; they are not guaranteed to be printed key
names. Whole-device colour reports the average of readable LED colours.

Battery, microphone, surround, sidetone, equalizer and layout data are shown when
reported by the SDK. Device/LED counts appear in the sensor browser. Writable
microphone/surround/sidetone properties become switches; a writable equalizer
preset becomes a numbered selector. Unsupported properties are omitted. New or
removed devices are detected every two seconds, independently of Windows/HWiNFO.

Changing a colour overlays the active iCUE profile using the SDK's shared
lighting layer. Turning a light off sets black; **Return lighting to iCUE** clears
WHAT's layer. Disabling the module/control permission or exiting the app
also releases the layer. Other applications with higher priority or exclusive
control can prevent the requested colour from appearing; errors are logged.

The supported public SDK does not expose every saved-profile or device setting. WHAT also offers a separate experimental **Profiles & device controls** submodule; available controls depend on the installed iCUE version. See [iCUE](ICUE.md) for the supported and experimental paths.

Release scripts fetch Corsair SDK 4.0.84 once into build/dependencies and verify
its pinned checksum. The SDK DLL is included automatically in generated releases
and installers. Qt Creator/CMake builds also run this preparation
automatically in both Debug and Release. The required Microsoft x64 runtime
DLLs are copied from the builder's Visual Studio redist directory alongside the
SDK, so users do not need a separate Visual C++ runtime installation. Release
builds verify that the SDK loads using those bundled files. The developer machine
needs the Visual Studio C++ workload already required for the WinRT helper.

## Back up and recover settings

Quit WHAT before making a backup. Copy the entire profile folder listed in Diagnostics to a private backup location. The usual path is `%LOCALAPPDATA%\WHAT\WHAT`, but an isolated or packaged launch may use a different location - use the displayed path. Keep an existing backup before restoring.

For recovery on the same Windows account, quit WHAT, restore the complete profile to that same location, then reopen WHAT and check pairing, selected sensors and permissions. DPAPI-protected credentials are tied to the Windows account; copying the folder to a different PC/account is not a supported credential migration. Re-enter credentials and re-pair on the new account. For portable settings, use **Settings > Configuration backup** to export/import YAML with a preview; see [Configuration import and export](CONFIGURATION.md). This excludes credentials and is separate from a complete private profile backup. Never attach this profile to a support issue; use the redacted diagnostic report instead.

## Profiles and security

Normal profile: `%LOCALAPPDATA%/WHAT/WHAT` (Qt AppLocalDataLocation).
The persistent UUID, certificate, selection/catalog and settings survive app
upgrades. Token/private key are protected by Windows DPAPI for the current user.
Secrets are never silently saved in plaintext if DPAPI fails. Preserve this
profile; never send it as a support bundle. The log contains no sampled values
or tokens and retains only 500 entries. Export it through Diagnostics.

The direct HA API connection requires TLS 1.2+ and certificate fingerprint pinning. mDNS carries
only discovery hints. Remote Windows controls require explicit permissions on
Modules. Shell execution has its own off-by-default permission. Replacing the pairing token
disconnects clients and requires updating HA credentials.

## Home Assistant controls

Install matching WHAT Windows and integration builds. In Modules, expand Windows
and enable the required Remote control permissions. These save immediately;
all groups default to off. Disabling Windows also blocks its commands.

- Media: default output volume/mute and the current Windows media session's
  play, pause, stop, previous/next, seek, shuffle and repeat controls. Playback
  features appear only when the media app supports them through Windows.
- Power: sleep, hibernate, restart, shutdown and lock buttons.
- Power plans/modes: installed power plans and, where supported by Windows,
  separate plugged-in and battery power-mode selectors.
- Web URLs: an HA text entity opens an HTTP/HTTPS URL in the PC's browser.
- Monitor: a button requests display sleep; input or another app may wake it.

Media permission shares playback and volume state even if those individual
sensors are unchecked. Titles and artist information still require the separate
media-title privacy option. URL contents and pairing secrets are not logged.

Commands require the existing authenticated TLS connection. They run on a
dedicated QThread worker and are never automatically retried. Sleep, hibernate,
restart and shutdown acknowledge acceptance before Windows disconnects; this
does not confirm completion. Check Diagnostics for local failures. Hibernate
requires Windows support and configuration; power modes can be unavailable on
some Windows versions or hardware. Windows may block shutdown for open apps.

Startup logs show the full path to the QSettings YAML configuration file, which
holds app and module settings, sensor selections, custom sensors, saved commands
and icon settings. They also show the pending import-file location and any
configured MQTT certificate/key paths, without printing their contents. The
executable, profile, worker details and service status are included too.
Diagnostics also records command acceptance, completion,
rejection and Windows errors. Console output uses the same log; a normal GUI
launch does not create a separate console window.

### Custom Sensors and Saved Commands

Open **Sensors > Add Custom Sensor** to choose what to monitor. Sources include
an app's executable name, Windows service name, file property, English
performance-counter path, WMI SELECT query, or a script. The editor explains each
source and gives examples. Use **Manage Custom Sensors** to edit saved definitions,
or expand a custom reading in the browser and choose **Edit Sensor**.

Open **Commands > New Command** for a named task. Add a name, an optional
description and the command to run. **Run Once** executes a local command without
saving it. Both editors support Windows PowerShell, installed PowerShell 7, CMD
and the default WSL distribution. Recent HA-supplied commands and their output
are under **Commands > HA Command Activity**.

Click a saved command's row to view its script or last output, edit it, or remove
it. **Run** is available directly on each row. The **Home Assistant Access** column shows
whether HA can trigger it locally or through a cloud connection.

Permission badges use grey for blocked access, a blue outline for reading
sharing, a blue fill for allowed controls, and red with an exclamation mark for
Home Assistant-supplied shell commands. Hover a badge for details. Local and Cloud identify the connection
route; sharing readings does not grant command permission.

**Test Sensor** and **Run and Test** execute the real source before saving. They
show the resulting readings or command output, exit code and duration. Scripts
run as your Windows user and are not sandboxed. A sensor script should only read
data. **Advanced** contains the script timeout and sensor output options. Failed
sources back off and a source never overlaps its previous poll. Two collection
workers keep scripts and Windows queries off the GUI; saved commands use a
separate worker.

A source can produce up to 32 readings. Text output is a single value; JSON can
provide several readings using paths such as `disk.free` or `disks.0.free`.
Outputs support number, text, binary and timestamp types. Timestamps need a time
zone. Numeric outputs can have units and an optional HA statistics class.
Process presence and file existence are binary; file size is bytes; file
modification time is a timestamp. A Windows service returns a text state.

Use **Local** and **Cloud** in the sensor browser to choose which readings HA
receives. Sharing a reading does not let HA edit its source or supply scripts.
Locally configured sensor scripts still run on their schedule.

Saved commands also appear in the sensor browser with **Run Command** and
**Edit Saved Command**. Local runs do not require HA permission. Remote access
requires the Custom module to be enabled, plus:

- **Allow Home Assistant to Run Saved Commands** on the Commands page.
- **Allow Home Assistant to Run This Saved Command** in each command's editor.
- For cloud execution, **Also Allow Cloud Connections** for that command and
  the Custom module's cloud permission. Cloud readings still need their own
  Cloud selection in the browser.

Each command starts local-only. Commands saved before these per-command switches
were added also need explicit permission before HA can run them. Their IDs and
scripts are kept. Disabling permission makes existing HA buttons unavailable.

**Allow Home Assistant to Send Shell Commands** is a separate permission for
commands supplied by HA scripts, automations or its UI. It grants much broader
access to the Windows account. Saved command definitions cannot be edited through
the command API, but a compromised HA instance could still trigger any allowed
command. Built-in controls retain their own module permissions.

Names can change without replacing entity IDs. To change an output's type, unit
or statistics, add a new output so existing HA history keeps its meaning.

Definitions are saved in `settings.yaml`. Command text, source paths and raw
results stay on the PC. They are included in a full private profile backup, but
not in the portable configuration export. Do not put secrets in shared readings.
Disabling a source leaves its selected readings unavailable; it does not remove
their HA registrations. Deleting an output also keeps its old history and
selection until you explicitly unshare and clean it up.

### Notification Buttons, Images and Replies

Enable **Windows > Notifications from HA**, then use `what.send_notification`.
Short text works in portable builds. The installed app also supports messages up
to 2000 Windows text units, images, up to four buttons, replies, silent delivery
and an expiry of 1-3600 seconds. Windows notification settings still apply.

Each action has an `id` and `title`. An optional `url` opens an HTTP/HTTPS page.
Other actions fire `what_notification_action` in HA, carrying `device_id`,
`pc_id`, `notification_id`, `action`, `reply` and a unique `event_id`. Clicking
the notification body uses the reserved action `OPEN`. Set `reply: true` and
include an action without a URL to submit a reply. WHAT must be running for
action replies; they are not restored after the app restarts.

Reuse `notification_id` to replace a notification and invalidate its old
buttons. Each notification accepts one action. HA suppresses repeated frames
during transport changes and does not replay old actions after an integration
reload. New action events expire after 60 seconds if HA is disconnected.
Reply text is sent to HA but omitted from WHAT's activity log.

Images must be reachable by the PC over HTTP/HTTPS, at most 2 MiB and 4096 pixels
per side. WHAT decodes them on its worker and keeps a bounded temporary cache.

### Speech and Audio Announcements

Enable **Windows > Speech and Audio from Home Assistant**. Use `what.speak` for
local Windows speech, with optional installed `voice`, `rate` (0.5-2) and
`volume` (0-1). Voice IDs are listed in that module section.

The separate **Announcements** HA media player supports `tts.speak`, media-source
audio and HTTP/HTTPS audio URLs. Its volume affects WHAT's player only. A new
announcement replaces the current one. Play, pause, stop and mute are available;
this player does not queue announcements or control another application's audio.
Asynchronous playback failures appear in its `last_error` attribute.

The PC must be able to reach the audio URL. Reverse WebSocket connections use
HA's external HTTPS URL for HA-hosted audio. MQTT prefers that external URL when
configured. Windows speech is generated locally and does not need an audio URL.

### PowerShell, CMD and WSL commands

Enable **Allow Home Assistant to Send Shell Commands** under
**Commands > Home Assistant Permissions** or **Modules > Windows**.
In HA use the **WHAT: Run Windows command** action and
select one PC. Choose Windows PowerShell (`powershell`), separately installed
PowerShell 7 (`pwsh`), Command Prompt (`cmd`) or WSL (`wsl`). WSL may specify an
installed distribution; otherwise it uses the default. This action requires HA
administrator access for user-initiated calls; internal automations are allowed.

Commands run non-interactively, hidden, from the Windows user's home directory,
with null standard input. PowerShell profiles are not loaded. WSL uses the
distribution's configured user and requires the Linux `timeout` utility.
The runner never requests elevation; running WHAT elevated also elevates its
Windows commands. This permission grants arbitrary code execution, including
access to files available to that account. It is not a security sandbox.

Use 1 to 300 seconds for the deadline (default 30) and at most 2,048 command
characters. HA receives stdout, stderr, exit_code, timed_out, cancelled and
truncated. Each output stream retains at most 64 KiB, decoded as UTF-8. Programs
that override their output encoding may display replacement characters. A
nonzero exit code is returned normally for your automation to inspect.

By default, Windows child processes stop when the command finishes. To keep an
application open, enable **Leave Launched Apps Running** in the saved command's
Advanced section, or set `leave_running: true` in a `what.run_command` action.
The shell must still exit successfully within its timeout. Use `Start-Process`
in PowerShell or `start "" "app.exe"` in CMD to launch an app without waiting for
it. Invoking an app directly can keep the shell waiting until WHAT's timeout. Failure, cancellation
and permission/session revocation before completion stop the running Windows job.
After a successful release, WHAT stops monitoring the child applications and
cannot cancel them later. Home Assistant cannot override this setting for a
named command; edit it in WHAT.

For example:

```yaml
action: what.run_command
data:
  device_id: YOUR_WHAT_DEVICE_ID
  shell: powershell
  command: Start-Process notepad
  timeout: 30
  leave_running: true
```

This option is unavailable for custom sensors and WSL. WSL receives a guest-side
deadline with a short kill grace period; detached Linux processes or services
started through external brokers are not guaranteed to stop. Cancellation cannot
undo changes already made. Disconnects never replay a command.

Command text and output are not persisted in Windows Diagnostics. HA action
configuration and automation traces can contain both, so avoid embedding secrets.


## Certificate lifetime

Overview warns when the HA TLS certificate has fewer than 90 days left, has
expired, or could not be loaded. Use **Renew Certificate** there or under
**Settings > Pairing and Security**. A successful renewal shows buttons to copy
the new fingerprint and existing pairing token.

In Home Assistant, open **Settings > Devices & services > WHAT** and use
**Reconfigure** on the existing PC entry. Enter the new fingerprint and existing
token. Keep the existing entry to preserve its device and entity IDs, custom
names and settings. The Windows PC ID and sensor selections are also retained.
No rebuild is required.

The certificate and private key are stored together in the DPAPI-protected
`identity.dpapi` file and replaced atomically during renewal.


## Modules

Enabled modules show a **Needs Attention** warning on Overview and on their
Modules card, even when collapsed. The warning gives the reported cause;
**Module Settings** on Overview opens the affected card. Warnings clear when
the source recovers or is disabled. Initial connection attempts, unsupported
optional readings and idle media sessions do not count as failures.
Use the warning's × button to hide it on that page until its message changes or
WHAT restarts. This keeps the module enabled and leaves its status visible.

Modules is the home for HWiNFO and Windows sources/privacy. Enable switches apply
immediately and persist. Disabled modules stop collection and disappear from the
sensor browser. Their HA entities become unavailable, preserving entity history;
re-enabling restores the saved sensor selections and favourites. The WebSocket
connection remains running even if both modules are off.

Windows polling supports 500 to 60000 ms; HWiNFO shared-memory checks support
100 to 10000 ms. Edits save automatically after a short pause or when committed; no Apply button is needed. These are polling periods,
not cache expiry. HWiNFO's own sampling period still limits measurement freshness.
Settings retains the heartbeat (seconds), port and startup options. Specs retains
its existing cache until Refresh specs is clicked.

Expand **Some details are unavailable** on Specs to see affected devices and
fields. WHAT includes CIM, Windows and driver query errors when reported. A
missing field is listed as unreported when Windows gives no specific reason.

Sensor values retain the raw number/unit on the right; a separate label to its
left converts large byte values (KiB/MiB/GiB) and bit rates (kb/s/Mb/s/Gb/s).
For example, 10,000,000,000 bit/s is 10 Gb/s, not 10 GB/s. This is display-only.

Right-click a reading or control to use it for an app or system tray icon.
**Set App Icon** and **Set System Tray Icon** replace WHAT's default icons;
the **Add** options create extra icons. Each opens the Taskbar picker with that
entry selected. Confirm there to apply the change, or cancel to keep your icons.

Icon text works best with 1-2 characters, such as a two-digit number. You can
choose any reading, but longer text or values can be difficult to read at
taskbar and system tray sizes.


## Finding module settings

Search Modules by provider or option (for example, notifications or temperature).
Results open automatically while searching. Clear the search to restore the
collapsed/expanded state. Use the chevron to open a module's settings and its
switch to enable/disable collection. Disabled module filters and readings disappear
from Sensors, including rows belonging to disabled optional Windows collections.

The last row on Modules opens a GitHub feature request for another API or device.

Settings is anchored at the bottom of navigation. Valid port changes take effect
automatically after a short pause; invalid values leave the listener unchanged.
A busy port is rejected and the previous value restored. Reconfigure the port in
Home Assistant after changing it. Polling and heartbeat edits also save without
buttons; startup and privacy switches save immediately.

### Local iCUE controls in Sensors

Enable iCUE in Modules, then open Sensors with the iCUE source checked. Click a lighting row to expand its colour and brightness controls, then click **Apply Colour** to send the change. Expand Individual LEDs for per-LED edits; Clear Lighting Override clears the lighting overlay. Supported writable headset settings appear inside their rows. Local controls need neither Home Assistant nor remote-control permission. Lighting-only rows are not shared telemetry and do not increase the sensor count. Choose **All Readings** to see them; stars can bookmark them.

### Saved Colours

The iCUE, Dynamic Lighting and icon colour pickers share Previously Used Colours
and Favourites. The last 16 colours are kept without duplicates, newest first.
iCUE adds a colour after a successful change; the other pickers add it when you
confirm the selection. Cancelled selections are not added.

Use Add Favourite to keep a colour, or select a favourite and remove it with the
same button. You can also right-click a favourite to remove it. Up to 32 favourites are kept separately from recent colours. Saving
a favourite does not apply it to a device. Both lists are saved in `settings.yaml`
through QSettings and included in configuration exports. Icon colours keep their
opacity; lighting uses their RGB values.

### Windows submodules and command stream

Modules → Windows contains separate switches for Commands, Media, Power actions, Power modes, Monitors, Web URLs, Session, Foreground application, Microphone, Windows events and Notifications. Switches save immediately. Commands is off by default; previous permissions remain unchanged on upgrade. Media has a separate permission for HA control. Turning Media off stops its collection and blocks media control.

Expand Commands to see requests, live stdout/stderr, rejection reasons and completion status. Follow keeps the newest output visible; Clear empties the memory-only buffer. It retains up to 65,536 characters and is not written to settings or diagnostic exports. Programs may buffer their own output.

LEDs are nested under their device in Sensors. Identical models have a short device suffix to distinguish them. Keyboard letters and digits use SDK key names where available; other LEDs use their order by SDK ID, with the raw ID available on hover. These fallback numbers do not claim to identify a physical fan or zone. Searching and favourites retain the device ancestor; expand Individual LEDs to view the controls.

### LINK hub lighting

Click a lighting row to edit its colour inline. When iCUE provides valid channel device metadata, hubs contain a separate subtree and whole-device colour control for each connected lighting device, with individual LEDs below it. Names reflect the SDK device type and channel position, not custom iCUE labels. Incomplete or inconsistent channels stay in Unassigned LEDs. Diagnostics logs how many LEDs could be mapped. Mapping follows Corsair's documented LED groups and channel properties: https://corsairofficial.github.io/cue-sdk/ .

## WHAT App Controls in Home Assistant

The parent PC device, such as **WHAT · DESKTOP-PC**, includes:

- **Diagnostic Logging**: changes WHAT's activity logging. Memory dumps remain a separate local setting.
- **Publishing Interval**: the normal sensor publishing interval in milliseconds, from 100 to 60000. It cannot be shorter than the high-priority interval. This does not change collection rates or the connection heartbeat.
- **WHAT Update**: installed version, latest checked release, update channel, last check time and a GitHub release link. It follows the app's update checks. Install updates from WHAT on the PC.
- **App Uptime** and **Last Seen**: WHAT's running time and the time HA last received data. Last Seen remains visible after disconnection.
- **WHAT CPU Usage** and **WHAT Memory Usage**: sampled every five seconds for WHAT.exe. CPU uses a 0-100% scale across all logical processors; memory is the working set in MiB. Helper processes are excluded.
- **Restart WHAT**: restarts the app, keeping its profile and settings. It does not restart Windows. Restart is unavailable during an update operation.

To allow setting changes and restarts, enable **Allow Home Assistant to Manage WHAT**
in the Windows app under **Settings > Pairing and Security**. It is off by default
and applies to authenticated local, remote and MQTT connections. Diagnostics do not
need this permission. Module permissions and transport setup remain in the app.

Update both WHAT and the integration to use these entities. App Uptime retains
its existing unique ID when moved from the Windows module to the parent PC.
