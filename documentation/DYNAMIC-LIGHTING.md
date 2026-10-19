# Windows Dynamic Lighting

Open **Modules > Windows API > Dynamic Lighting**. Enable the submodule to discover
compatible HID LampArray devices. Use the colour picker, brightness slider, Apply
and Off buttons. This is independent of the Corsair SDK; it does not add support
for arbitrary RGB hardware that Windows does not expose.

For background control, use WHAT's installed edition. In **Windows Settings >
Personalisation > Dynamic Lighting**, enable Dynamic Lighting and move WHAT to the
top of Background light control for the devices you want it to control. Windows
may give a foreground application priority. WHAT respects that choice and shows
when control is unavailable. Debug and portable processes do not have the package
identity required for background lighting control.

Enable **Allow Home Assistant to control lighting** to expose one light per device.
It supports RGB colour, brightness and on/off. Cloud control is a separate opt-in,
off by default. The same entities work over WHAT's HA API and MQTT transports.
Device identifiers are hashes, not raw Windows device paths. Turning off the Windows
module or Dynamic Lighting releases its device handles. Revoking remote/cloud
permission cancels the current helper session too.

Windows does not provide a colour readback for other lighting applications here.
HA shows the last colour WHAT successfully applied; the initial state is unknown.
Losing Windows control clears that remembered state. Turning lights off sets their
brightness/colour to zero while keeping WHAT in control; disable the submodule to
hand control back. Native Windows global effects and priority settings are managed
in Windows Settings, not overwritten through undocumented registry settings.
Avoid controlling the same device through iCUE and Dynamic Lighting simultaneously.

Device discovery and changes run in a persistent C++/WinRT helper process. Qt uses
asynchronous pipes, bounded requests and a timeout. No shell commands or console
windows are used. Discovery refreshes every five seconds. Unplugging devices and
Windows ownership changes are reflected at the next refresh. The helper exits on
parent-pipe EOF, releasing ownership even when WHAT closes unexpectedly.

The MSIX manifest registers WHAT as a `com.microsoft.windows.lighting` provider.
Release builds compile the helper automatically. For direct source/Qt Creator
builds, run `scripts/build-windows-bridge.cmd` from an x64 Native Tools command
prompt and copy `build/WHAT.WindowsHelper.exe` into the build's `helpers` folder.
The helper includes compatibility declarations for the public LampArray availability
interface, allowing it to build with the existing Windows 22000 SDK.

Reference: [Microsoft Dynamic Lighting documentation](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/lighting-dynamic-lamparray).
