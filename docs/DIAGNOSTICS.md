# Diagnostics and bug reports

Open **Diagnostics > Prepare bug report**. Formatting runs on a worker thread.
Use **Refresh report** for a new snapshot, review the preview, then copy or save
it. Exports are plain text. WHAT does not upload them or enable ongoing logging
when you prepare a report.

The live view covers Windows, HWiNFO, Core Temp, NVIDIA, BEACN, iCUE RGB,
iCUE Automation and Wallpaper Engine. It shows worker activity, recent sample
ages, collection and control permissions, and local/cloud sensor selections.
Connection details list each installation's type, enabled state, control permission
and authenticated connection state. Debug messages identify the installation. Selected counts include unavailable sensors;
disabled modules do not publish.

App, driver and SDK versions come from cached module state, executable metadata
and Windows installation records. Installation checks run in the diagnostics
worker and are cached for up to a minute. A missing version is shown as
"Not reported". Shared-memory and API versions are labelled separately from
application versions. Windows components use the Windows and WHAT versions.

## Report contents

Reports include versions, module and connection state, collection intervals,
worker errors, HWiNFO memory health, and cached GPU, iCUE and display inventory.
The sensor tree shows availability, units and sharing state. Reports also include
recent activity with private operation details omitted. Preparing a report does
not enable disabled modules or refresh their cached inventory.

Sensor values, command input and output, notification content and arbitrary
settings are excluded. Only approved numeric and boolean settings are included.
Known pairing secrets, PC identity, account names and profile paths are redacted.
Common URLs, IP addresses, email addresses, UUIDs and serial-like codes are masked.

Custom labels can contain personal text that the redaction rules do not recognise.
Review the report before sharing it. The live diagnostics view and terminal output
contain more detail; use the prepared report for bug reports.

## Local logging

Open **Diagnostics > Logging and Crash Reports**, then enable
**Save diagnostic and crash logs locally**. The folder
contains these files:

| File | Contents and limits |
| --- | --- |
| `activity.log` | Redacted activity from WHAT and WindowsHelper, with module, process, PID, thread and severity. Stops at 4 MiB and rotates to `activity.previous.log` at the next logging session. |
| `session-*.log` | Version and health snapshots. Five files are retained, approximately 256 KiB each. |
| `crash-*.txt` | Native exception code, faulting module, stack addresses, build version, process and thread. Startup cleanup retains recent reports. |
| `crash-*.dmp` | Optional memory dumps. Disabled by default and controlled separately from logging. |

Logging settings apply to running helpers through internal policy files. Change
the settings through WHAT's controls. Turning logging off stops new records and
keeps existing files. Nothing is uploaded automatically.

Activity writes and session disk flushes run on background workers. Session writes
are ordered and limited to 256 KiB, including queued data. Shutdown gives accepted
writes a bounded period to drain. Sensitive operation text is omitted, common
addresses and identifiers are redacted, and numeric failure codes are retained.

## MQTT testing

Debug builds automatically show `MQTT debug:` lines in Qt Creator's Application
Output and WHAT's Diagnostics activity log. In an installed Release build, enable
**Save diagnostic and crash logs locally** before switching connections. This
also saves the trace in `activity.log`. No separate logging setting is needed.

The trace records safe connection settings, broker error codes, subscription
confirmation, protocol authentication and the active MQTT/API routes. It records
the first submitted sensor snapshot and the first Home Assistant application
reply, then reports cumulative traffic totals every 15 seconds while connected
and when disconnecting. Totals restart on a connection attempt or when tracing
is enabled.

`full` and `delta` count sensor frames. `requests` counts signed incoming control
requests; `result_ok` and `result_no` count their outcomes. `app_replies` and
`round_trip_ms` measure Home Assistant's application replies, independently of
sensor publishing. An age or round-trip value of `-1` means no response yet.
`write_completions` only means Paho finished the local QoS 0 write. It does not
prove that Home Assistant received it. Invalid messages are counted by reason,
with only the first occurrence logged individually, to avoid flooding the GUI.

Broker addresses, account names, certificate paths, pairing material, session
identifiers and message bodies are excluded from this trace. It runs on the
network worker and does not format individual sensor values for logging.

For a test, keep the existing local connection while saving MQTT settings, switch
to MQTT, change a shared reading and try an allowed control, then switch back to
Home Assistant API. Look for subscription confirmation, accepted protocol
authentication, a full snapshot, application replies and the route counts
changing. If handover stalls, enable debug logging for WHAT in Home Assistant's
integration menu too. See [MQTT setup](MQTT.md) for pairing, scope and broker ACLs.

## Crash reports and memory dumps

**Memory dumps can contain credentials and other private memory.** Enable them
only when needed, and review where you send them. Text-report redaction does not
apply to memory dumps.

Crash reporting runs in the faulting process and may fail after force-termination,
fail-fast, severe heap or stack corruption, or a hang. WHAT does not take automatic
hang dumps. The parent also records helper exits and timeouts.

If a dump cannot be written, the text report and activity log include the failed
operation and its Windows error code. Temporary partial-read failures are retried
up to three times; permanent errors are reported immediately. Incomplete dumps
are removed so they cannot be mistaken for usable crash reports.

Native stack addresses need matching build symbols to resolve function names and
line numbers. A module label identifies the reporting component; it does not
establish the cause of the crash.
