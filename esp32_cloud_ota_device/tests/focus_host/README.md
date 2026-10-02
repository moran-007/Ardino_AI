# Focus timer host verification

Run `./run.ps1` from PowerShell. The suite compiles the production `focus_timer.cpp`
against the existing read-only Arduino/NVS host fixtures and installed ArduinoJson.
It covers spoken commands and Chinese numbers, focus/break/long-break transitions,
pause/resume/skip/stop, delayed polling, `millis()` rollover, atomic write failures,
configuration updates, persisted completion counts, reboot pause recovery, and
rejection of corrupt state without overwriting the record.

The generated `output/focus.html` is a standalone administration UI fixture. Its
`adminFetch('/focus')` API needs the normal firmware integration or a browser test
mock. Run `node verify_ui.mjs output/focus.html` for the DOM behavior checks.

NVS progress checkpoints are limited to one per minute while running. Reboot
restores the latest saved progress as **paused**; power-off time is never counted.
Skipped focus phases do not count as completed sessions. An overdue poll performs
one transition and gives the new phase its full duration instead of fabricating
completed sessions for unobserved time. No real audio or device hardware is tested
by these host checks.

`GET /focus` returns `FocusTimer::toJson()`. Authenticated `POST /focus` accepts
`action: start|pause|resume|stop|skip|configure|state`. `start` and `configure` may
include `focus_minutes`, `short_break_minutes`, `long_break_minutes`, and
`rounds_before_long_break`; validate JSON integer types/ranges **before** casting
to `Config`. Successful responses return the state plus `ok: true` and `message`;
rejected/persistence-failed responses return `ok: false`, `message`, and a failure
HTTP status. Route glue belongs in the main sketch, outside this module's scope.
