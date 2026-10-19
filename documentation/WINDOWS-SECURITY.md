# Windows Security

Enable **Modules > Windows > Windows Security** to collect read-only health from Windows Security Center. It is off by default. Select its three readings in the sensor browser to share them with Home Assistant using the usual Local/Cloud permissions.

- Antivirus health (Microsoft Defender or the antivirus registered with Windows)
- Firewall health
- Automatic-update settings health

The native `WscGetSecurityProviderHealth` API runs on the existing slow background worker and is cached for 60 seconds. Statuses are Good, Not monitored, Poor or Snoozed. A stopped Security Center service, failed query or unknown result makes the reading unavailable; it is never reported as healthy. Disabling collection removes these readings from the browser and publishing policy.

These are category health summaries, not Defender-specific threat lists, scan history, signature versions or pending-update counts. WHAT cannot change antivirus, firewall or update settings. The module provides a shortcut to Windows Security.

API reference: https://learn.microsoft.com/en-us/windows/win32/api/wscapi/nf-wscapi-wscgetsecurityproviderhealth
