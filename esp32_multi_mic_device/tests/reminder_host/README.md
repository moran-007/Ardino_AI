# Reminder Host Tests

Run from the workspace with PowerShell:

```powershell
./esp32_cloud_ota_device/tests/reminder_host/run.ps1
```

The runner compiles the real memo and alarm managers with an in-memory filesystem,
Preferences stub, and the installed ArduinoJson library. It does not build or flash
ESP32 firmware, access the device, or change production reminders.

Coverage includes Chinese prefix/suffix creation commands, compound todo/reminder
commands, missing clock and storage failures, persistence, per-type ordinals, and
escaped HTML rendered through the production PageStream. Generated fixtures are
written to `output/reminders.html` and `output/cases.json`.

The page fixture uses the production admin CSS and memo controls. Browser checks
can exercise tabs, keyboard navigation, completion forms, reload selection, and
mobile overflow with intercepted form submissions. Physical alarm playback and
voice recognition still require device verification after a firmware upgrade.
