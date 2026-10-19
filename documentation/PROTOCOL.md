# WHAT Protocol v2

## Transports and timing

The v2 entity protocol below remains shared by direct and MQTT transports. MQTT adds an authenticated outer envelope and per-session topics; it does not create MQTT Discovery entities. The internal `_publishing`, `_normal_ms`, `_fast_ms` and `_event_ids` fields exist only between the GUI and the network worker and are removed before serialization. Scope filtering and rounding happen before per-peer delta comparison. See [MQTT protocol and setup](MQTT.md).

Peers negotiate `command_deadline_v1` in auth/hello. Commands then carry
`deadline_ms`, relative to this authenticated session, instead of a Unix expiry.
Both sides use monotonic clocks. Each transmitted snapshot or delta includes
`session_ms`, sampled from the PC's command-session clock immediately before
serialization. Home Assistant re-anchors its deadline calculation from that sample,
preventing frequency differences from accumulating over a long connection. Transit
time reduces the available deadline rather than extending it. Peers that omit the
sample retain the initial handshake anchor.
The PC rejects expired deadlines and deadlines more than 60 seconds ahead and still
rejects duplicate IDs. Old peers retain the checked `expires_at` field and need
synchronised clocks. Update both WHAT and the integration for relative deadlines.

Snapshots and deltas also include `sent_at`, the PC's Unix time in seconds at
transmission. Notification-action freshness compares this with the event's PC
timestamp, not Home Assistant's wall clock. Deduplication uses Home Assistant's
monotonic arrival time and survives transport handovers. The first snapshot after
integration startup establishes a baseline without replaying stored actions.

Remote commands require authentication and the corresponding module permissions.

## Authentication and snapshots

Use compatible Windows app and integration builds. Protocol v1 is not supported.

Local WHAT API connections use TLS 1.2+ WebSockets (`wss://PC:48765/`). Home Assistant initiates connections and
pins the manually copied SHA-256 certificate fingerprint BEFORE sending the
random 256-bit token. Normal Windows profiles encrypt the token/private key with
DPAPI. Pairing secrets are not sent over plaintext WebSockets. Remote commands
require the applicable WHAT permissions.

Authenticate within five seconds:
`{"type":"auth","protocol":2,"token":"<64 hex characters>"}`.
The server sends `hello` (protocol, device_id, name, app_version), then a full snapshot. Changed readings are pushed promptly; the configured
1 to 300 second interval is a heartbeat. Maximum eight clients.

Snapshot fields:
- `type: snapshot`, `protocol: 2`, `device_id`: persistent PC UUID.
- `healthy`: HWiNFO freshness only; never global PC/Windows availability.
- `hwinfo_enabled`: user source setting; false is not a HWiNFO problem.
- `windows_healthy`: background Windows collector has completed recently.
- `status`: HWiNFO state or a disabled explanation.
- `sample_time`: snapshot Unix time; `sample_age`: HWiNFO age, or -1.
- `publish_seconds`, `license`: self-reported unknown/free/pro, `sensors`: list.

Each selected reading has `id`, `source` (windows/hwinfo), `group`, `name`, `unit`,
legacy integer `type`, `kind` (number/text/binary/timestamp), `available`, `value`,
and optional `state_class` (empty/measurement/total_increasing).
Numbers must be finite; binary values are JSON booleans; strings are at most 255
characters. Timestamps are ISO 8601 with a timezone. Unavailable values MUST be
null. Non-numeric readings have empty units/state classes. Additional UI metadata
such as `recommended` is permitted. Limits: 8 MiB/message and 10,000 readings.

HA creates numeric/text/timestamp sensor entities and binary_sensor entities.
Per-reading availability is independent of HWiNFO health. An empty text value is
valid. Wrong types, invalid timestamps, NaN, duplicate IDs and identity changes
are rejected. Changing an established reading's kind/unit makes it unavailable
to avoid corrupting its history. Full snapshot payloads are complete replacements.
Unselected/missing readings become unavailable, never zero.

HWiNFO IDs remain three 8-digit hexadecimal components: sensor ID, instance and
reading ID. Windows IDs have win_ prefixes; volume and adapter identities use
hashes of Windows volume/interface GUIDs, never the drive letter. App diagnostics
use app_ prefixes. HA unique IDs additionally include the PC UUID. Volume IDs
survive unplugging, reinsertion and app restart; formatting a volume may give it
a new identity. Catalog metadata is retained locally without sampled values.

No deltas: reconnect receives a full snapshot. HA detects silent servers after
max(30, 3 × publish_seconds + 10) seconds and reconnects with exponential backoff
up to 60 seconds plus jitter. HWiNFO freshness uses its poll timestamp; unchanged
values are not stale. Windows freshness uses monotonic completion age, allowing
at least 30 seconds or three collection intervals.

mDNS `_what._tcp.local.` advertises id, version=2, tls=1. Discovery is only a
hint: it never conveys credentials or silently changes a paired address/pin.
Use manual pairing or reconfiguration where multicast/DHCP changes require it.

Service Stop and port changes close existing clients. Start/Restart keep identity and credentials. Reconfigure the paired HA port after changing it.


## Power-mode identifiers

`controls.power.mode_keys` optionally reports `ac` and `dc` using `efficiency`,
`balanced` or `performance`. New integrations send these keys in `power_mode_ac`
and `power_mode_dc` commands. Apps also accept the original English labels and
continue publishing `controls.power.modes` for older integrations. Entity IDs and
the labels shown in Home Assistant are unchanged.

## Optional ui_feedback_v1 extension

HA advertises `ui_feedback_v1: true` in auth; WHAT confirms it in hello. Only
negotiated peers receive `ui_probe` frames. Each has a random 32-character hex
`id` and a `keys` array containing at most 16 source identifiers (256 characters
each). An empty array is a connection check. WHAT sends a probe every five
seconds and keeps at most one outstanding probe per peer. If a telemetry write
is still pending, the due probe sends when it drains instead of waiting for the
next five-second tick. This check is separate from sensor publishing rates.

HA replies with `type: "ui_feedback"`, the same `id`, and an `entities` array of
at most 32 full entity IDs. Lookup uses the paired PC's unique ID and config
entry; it reads current HA registry names and cannot change entities or run
commands. Changing the expanded sensor invalidates the previous lookup. Late
or replayed replies do not update the displayed communication time.

The same frames travel over authenticated API and signed MQTT sessions. UI
frames never extend HA's telemetry freshness deadline. Peers without this
extension continue using the existing stream and command protocol.

## Optional delta_v1 extension

Clients request delta streaming with `"delta_v1": true` in auth. The server confirms it in hello.
Without confirmation, the client accepts the original full-snapshot format;
without an auth request, the server sends the original format. Protocol stays 2.

For negotiated streams the first snapshot has `sequence: 1`. Every subsequent
frame increments sequence by one. A delta has `type: "delta"`, `sequence`,
`base_sequence` equal to the preceding sequence, `sensors` containing only changed
or newly selected rows, and `removed` containing IDs no longer shared. Other
envelope fields retain their snapshot meanings; `publish_seconds` is the heartbeat.
A heartbeat can be a delta with empty sensors/removed arrays.

HA validates each frame before applying it. Missing baselines, gaps, malformed
rows, invalid removals and an accumulated catalog above 10,000 readings cause a
disconnect and a fresh authenticated connection/full snapshot. Reconnect does not
erase entity identity/history. Each client has its own last-sent baseline, so
skipping telemetry while a slow socket drains cannot corrupt its next delta.

Telemetry uses a non-restarting 50 ms batching timer. Native events bypass that
timer and have a maximum of 32 snapshots queued per client. A client exceeding
that bound is disconnected for resynchronisation; this is a state stream, not a
durable event journal. The producer-to-network event queue is also bounded. No
delivery guarantees apply across disconnects, suspension or app shutdown.
There is at most one outstanding WebSocket frame per client. A blocked client
is disconnected after 30 seconds. Frame count/bytes/delta count are logged about
once per minute while clients are receiving data; no sensor values are logged.

## Optional display configuration

Protocol 2 snapshots/deltas may carry a `displays` object: boolean `enabled`, `available`, `control_enabled`, `busy`, `pending`; string `status`, `active_layout`; `monitors` (at most 32) and `layouts` (at most 32). Monitor IDs are 24-character lowercase hex hashes of device paths. Monitor rows contain name, connected/enabled/primary booleans, group ID, mode (`extended`, `mirrored`, `disconnected`), integer x/y/width/height/rotation, and numeric refresh in Hz. Layout options contain UUID id, name and available. The server keeps full saved layouts private.

Command action `display`, args `operation` plus relevant strings: `extend`, `disconnect`, `primary` take `monitor`; `mirror` takes `monitor` and `source`; `restore` takes `layout_id`; `keep` and `revert` need no additional fields. Named save/delete are local GUI only. All remote display commands require the Displays submodule and separate display permission, checked before execution with the authenticated session lease. No shell expansion or arbitrary native configuration payloads are accepted. Manual changes require Keep within 20 seconds; exact saved-layout restores do not. Ordinary command responses acknowledge readback; use subsequent metadata for actual state.

## Windows notifications

Optional `controls.notifications_enabled` boolean advertises the separate PC-side permission without changing the existing permissions object. Command `notify` accepts string `title` (1 to 63 UTF-16 units, one line), `message` (1 to 255 units), and optional `level` (`information`, `warning`, `error`). Reject unknown fields, NULs and whitespace-only content. Authenticated session and Windows-module permission are checked immediately before submission on the GUI thread. One request/second maximum; no queued retries. Success acknowledges submission to the local tray notification API, not visible delivery.

## Keyboard/mouse commands

Optional controls.input_enabled is a boolean (absent means unsupported/disabled). The existing permissions object retains its original keys. Command action keyboard accepts exactly one of keys (1-6 supported unique key names) or text (1-1024 UTF-16 units). Command action mouse accepts integer physical desktop x/y, optional button (none/left/right/middle), and optional clicks (1/2, only with a button). Both require the independent input permission. Standard command replies and session cancellation apply; never retry automatically.


## Connection Choices

Snapshots include `connection_options` with boolean `remote_enabled` and
`mqtt_enabled` fields. HA applies new permissions only over the certificate-pinned
direct connection. Remote and MQTT sessions can revoke existing permissions but
cannot enable another route. This metadata contains no URLs, broker credentials
or pairing secrets. An unchanged choice does not reload the integration.

## Parent App State and Commands

Snapshots include an optional `app` object with `version: 1`. Its booleans are
`control_enabled`, `logging` and `restart_available`. `publishing_interval_ms`
is an integer from 100 to 60000; `high_interval_ms` is from 50 to that normal
interval. `uptime_seconds`, `cpu_percent` and `memory_bytes` describe WHAT.exe.
CPU is normalized across logical processors. Memory is the working set. CPU and
memory can be null before the first successful sample. Samples run every five seconds.

`app.update` contains `installed`, nullable `latest`, nullable `release_url`,
nullable UTC `checked_at`, `channel` (`stable` or `beta`) and `busy`. The latest
checked release is independent of the release selected for installation. Failed
checks keep the previous result and timestamp; changing channel clears them.
Credentials, authentication source and download URLs are never included.

The existing authenticated command envelope accepts `app_logging` with a boolean
`args.value`, `app_publish_interval` with an integer `args.value`, and `app_restart`
with empty arguments. They require `controls/what`, enabled locally in Pairing and
Security. This permission is independent of Windows module and power controls.
Publishing intervals below the current high-priority interval are rejected.
Settings replies mean applied in memory; normal asynchronous YAML persistence follows.

Restart launches the same executable and profile. The replacement waits up to
30 seconds for the previous process to exit before acquiring the profile lock.
The old instance acknowledges before quitting, rechecking connection and permission
before shutdown. HA never retries a command automatically. Test mode accepts restart
without actually restarting. Installation and update downloads remain local actions.

## Multiple Installations

The Windows configuration assigns each installation a stable local ID, name,
transport, enable switch and separate pairing token. Pairing credentials are
never included in telemetry. The PC device ID remains unchanged. Protocol version
stays 2.

Incoming TLS authentication selects the installation by token. Outbound API and
MQTT sessions belong to that installation. Each owns retry timers, command leases,
sequence numbers and delivery baselines on the existing network thread. Changing
identity or permissions, or deleting an installation, revokes its queued command
leases before the worker processes the change. Renaming and editing connection
settings keep the existing route alive during handover. Global policy revocations
still invalidate every affected stream.

`connection_options` describes only the receiving installation. Cleanup requests
are sent only to the chosen installation; its authenticated session must return
the preview and removal replies. UI entity lookups retain their installation name.

Remote-only onboarding stores explicitly supplied pairing credentials and enables
the selected receiver. It creates no trusted readings until a full authenticated
hello and validated snapshot arrive. MQTT HMAC checks use that installation's token,
including when several installations share a broker and PC topic namespace.

Snapshots and deltas can include a `connections` array describing all named
installations. The network worker attaches the current report to every outgoing
reading or event frame. Each row contains only:

| Field | Meaning |
| --- | --- |
| `id` | Stable installation ID, 32 lowercase hexadecimal characters |
| `name` | User-supplied name, 1 to 80 characters |
| `type` | `api` or `mqtt` |
| `status` | `connected`, `disabled`, `stopped`, `waiting`, `connecting`, `retrying` or `error` |
| `scope` | `none`, `local`, `cloud` or `hybrid` |
| `connected_since` | UTC Unix milliseconds, or 0 while disconnected |
| `last_reply` | UTC Unix milliseconds, or 0 before the first confirmed reply |

The array has unique IDs and uses the existing 10,000-entry protocol safety limit.
Unknown row fields, invalid values and duplicate IDs reject the frame. Older peers
can omit the array. There are no addresses, usernames, credentials or raw error
messages in this report.

The integration creates one diagnostic status sensor per installation under the
PC device. Renaming keeps its entity ID. Removed installations become unavailable;
cleanup leaves diagnostic registrations and history intact. All reported states
become unavailable when the receiving Home Assistant loses contact with WHAT.

### Coordinated Transport Changes

Peers negotiate `transport_switch_v1` during authentication. Each installation
keeps its API and MQTT configurations with one selected transport. Its frames
include `connection_config`: a fresh 32-character hex `revision`, `preferred`
(`api` or `mqtt`), boolean `remote_enabled` and `mqtt_enabled`, and the PC's
`direct_host`, `direct_port` and SHA-256 `fingerprint`. This profile describes only
the receiving installation. It contains no token or broker credentials.

The integration applies this PC-authenticated configuration without reloading its
entry. A manually changed pairing or credential still triggers the normal reload.
For an initial MQTT-only pairing, the profile supplies the direct endpoint and
certificate pin; an existing manually configured direct address stays unchanged.

A replacement receives one snapshot for validation and cannot issue commands or
receive the ongoing stream yet. After draining outstanding command replies, Home
Assistant sends `transport_activate` with a fresh 32-character hex `id` and the
profile `revision`. WHAT checks that the candidate matches the current selection,
revokes the old peers and their command leases, and replies with a `command_result`.
Home Assistant then adopts the candidate. Commands are never replayed.

Failed validation or a changed revision leaves the current stream active. After
30 seconds without activation, WHAT restores its previous transport selection and
reports the failure. Disabling or deleting the installation cancels the handover.
Older peers can keep their existing connection; automatic handover requires both
updated components.

## Leaving launched applications running

Saved commands have a **Leave Launched Apps Running** option in WHAT's command
editor. The `what.run_command` action accepts `leave_running: true` for inline
PowerShell, PowerShell 7 and CMD commands. It is off by default and is not supported
for WSL or custom sensors. Set the option in WHAT for named commands; Home Assistant
cannot override their saved definitions.

The shell must exit successfully within its timeout. Only then may its child
applications remain running. Failure, cancellation, permission revocation before
completion and timeout still terminate the command and its children. After a
successful release, WHAT stops collecting output and cannot cancel those apps.

For example, `Start-Process notepad` with `leave_running: true` keeps the launched
application open after PowerShell exits. A command that never finishes still times out.
