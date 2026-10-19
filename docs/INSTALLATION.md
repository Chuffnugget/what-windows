# Installing WHAT

Choose the installer for your PC from the Windows repository's release:

- Intel or AMD: `WHAT-<version>-Windows-x64-Setup.exe`.
- Windows on ARM: `WHAT-<version>-Windows-arm64-Setup.exe`, when provided.

x64 builds require Windows 11 (build 22000 or newer). Native ARM64 builds require
Windows 11 22H2 (build 22621 or newer). Portable builds have the same supported
Windows requirements; Windows 10 is not a supported target. The Web module also
needs Microsoft's WebView2 Runtime for the installed app's architecture.

Double-click it and click Install. If WHAT is running in your Windows session,
choose Close app to end the monitor and install, Ignore to try without closing, or
Cancel to return. Close app asks WHAT processes in your session to quit, waits up to ten seconds
for each, then force-closes any that do not respond;
you can instead quit from the tray before retrying.
An administrator must approve certificate trust on the first installation unless
the included certificate is already trusted. Without that approval, use the
portable build or ask your administrator about installation. There is no ZIP to extract,
terminal command to run or certificate file to import manually.

The wizard contains the full signed app package and its public signing certificate.
It explains the certificate trust change before Install is clicked. The wizard
runs as your user; only certificate import requests elevation. The package is then
registered for your user, preserving the existing profile and updater identity.
Open WHAT with the wizard's Open app button or from Start. Close hides the app
to its tray; Quit exits it. A new profile opens the main window. To start in the
tray instead, enable Settings > Startup > Start Minimised. Packaged copies use a
Windows startup task, which Windows removes on uninstall. Existing WHAT Run entries
are migrated on the next launch. If Windows has disabled startup, enable WHAT in
Windows Settings > Apps > Startup; WHAT does not override that choice.

The MSIX is signed with the configured signing identity; the Setup and application EXEs are unsigned. A self-signed
identity is not publicly trusted: Windows can still show an Unknown publisher or
SmartScreen warning when opening Setup. A publicly trusted code-signing service
or certificate is required to establish public certificate trust. Naming the
publisher WHAT does not establish that trust or guarantee SmartScreen reputation.

## Updates and removal

Installed copies use the existing MSIX updater; the direct MSIX and update.json
remain release assets for this purpose. Use Setup.exe for a first installation.
After an in-app update completes, WHAT opens an Update complete prompt showing
the previous and current versions, release notes and a link to the GitHub release.
Dismiss it with Done. The prompt requires an update started from a version that
supports this confirmation; manual installations do not show it.

Overview and Settings > Application Updates warn when the installed app's signing
certificate is within 90 days of expiry or has expired. This certificate is managed
by the developer and is separate from Home Assistant pairing. An expired certificate
does not stop the installed app running, and a valid timestamped update may still work.

If Windows cannot verify an update or its signing certificate has changed, WHAT
keeps the current installation and links to its official GitHub releases. Download
the latest Setup.exe for your PC and run it over the existing installation. With
the same WHAT package identity and publisher, no uninstall or settings reset is
needed. Do not bypass signature checks or renew Home Assistant pairing to fix an
app-signing warning.

Windows Settings > Apps > Installed apps > WHAT > Uninstall removes the app.
Your normal WHAT settings/profile are retained, including pairing files and sensor
selections. There is no keep-settings checkbox; retention is the default. The Setup
wizard does not add another duplicate uninstall entry. Certificate trust is not removed automatically,
since other installations signed by the same identity may depend on it. If no
remaining installation uses that certificate, an administrator can remove that
specific certificate in `certlm.msc` under Trusted People > Certificates. Match its
thumbprint with the installer log; do not remove certificates based on the name
WHAT alone.

## Notification access

The installed app has the Windows package identity needed for notification access.
Enable Modules > Windows > Notification Collection, then click Request notification access and
respond to Windows. Notification contents and window titles remain separate opt-ins.
Portable copies still support normal Windows/HWiNFO collection.

Windows may ask for precise location access when WHAT reads Wi-Fi details. If you
decline, Wi-Fi network name and signal readings become unavailable. Other network
readings and Home Assistant connections continue working. WHAT reports the denied
access on Overview and in the Windows module while Wi-Fi Details is enabled.
Use Modules > Windows > Wi-Fi Details > Open Windows Location Settings to change
the decision; Windows does not repeat its one-time consent prompt after a denial.
Readings recover on the next collection after permission is restored. You can also
disable Wi-Fi Details there without disabling the other Windows readings.
The package declares location access for these Wi-Fi readings so Windows can
show its privacy controls at installation. WHAT does not collect GPS coordinates.
Windows policies or disabled system-wide location access can still deny access.

SmartScreen may show **Windows protected your PC** for a new download. If you
have verified its source and trust it, **More info > Run anyway** may be available.
Organisation policy can prevent this override. Smart App Control can block an
unsigned build that lacks a positive reputation; trusting WHAT's MSIX certificate
does not give its executables public trust or bypass that policy.

## Local connections and firewall

For local WHAT API connections, Home Assistant needs inbound TCP access to the
port shown in Pair This PC (48765 by default). On a trusted home network, allow
WHAT on the Private network profile in Windows Firewall. Public networks normally
block unsolicited incoming connections. Do not change an untrusted network to
Private or forward this port on your router just to make pairing work.

Discovery uses mDNS on UDP 5353. If multicast is blocked between networks, enter
the PC address and port manually. Remote WHAT API and MQTT connections are
outbound from the PC and do not need the inbound PC-port rule.

The repository's `scripts/Allow-Private-Firewall.ps1` is an optional administrator
command for a specific executable and port. Pass `-Executable` with the full path
to WHAT.exe and `-Port` if changed. It only allows the local subnet on Private
networks. It is never run automatically. For a versioned WindowsApps path,
run it again after each update with `-Refresh -Executable <new WHAT.exe path>`.
This updates the existing rule while keeping its executable, port, Private-network
and local-subnet restrictions. A port change also needs a refresh.

## Troubleshooting

The wizard shows installation output and has a Copy error button. Logs are retained
under `%LOCALAPPDATA%\WHAT\installer\<run>\install.log`. Setup keeps the five most
recent completed log folders; active runs and unexpected files are left alone.
Embedded package payloads are removed after the attempt. Windows certificate policy restrictions,
UAC cancellation, a running application or package registration errors are reported
instead of presenting a successful finish page.

A profile copied from another Windows account cannot decrypt that account's pairing
secrets. WHAT keeps the PC identity, selections and installation entries, and shows
which credentials need attention. Use Replace Token for the affected installation,
re-enter its broker password if used, and reconfigure its existing Home Assistant
entry. If the TLS private key is missing, restore the profile backup or use Renew
Certificate and copy the new fingerprint. WHAT does not silently replace a saved
certificate or delete Home Assistant entities.

## Build

Run `scripts/Build-Release.cmd`, then `scripts/Build-Installers.cmd`. The latter uses
Windows' .NET Framework C# compiler to create the wizard without an additional
installer-tool download. See [Building and publishing](RELEASE.md) for prerequisites and outputs. Build checks
validate the embedded payload without installing anything; interactive installation
and UAC still require testing on a normal Windows session.
