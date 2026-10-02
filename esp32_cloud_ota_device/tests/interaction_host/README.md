# Interaction gesture host proof

Run `./run.ps1` in PowerShell. This compiles the actual interaction and servo
modules with Arduino/NVS/PWM seams, then runs the UI's actual JavaScript.
`baseline.ps1` demonstrates that the original servo module had no greeting
gesture or corresponding PWM movement.

Checks prove default-disabled behavior, no movement during initialization or
reload, one atomic JSON configuration entry, validation and failed-write
rollback, eligible mood filtering, disabled-PWM and out-of-range skips, actual
one-degree steps at no faster than 40 ms, one-channel ownership, return to the
original instructed angle, explicit cancellation, manual target changes,
released output, timeout, PWM failure, and the maximum calibrated 30-degree
range. Normal high→low→original travel takes at most 60 steps (2.4 seconds);
late polling does not cause a rapid catch-up sweep and is cut off after 4 seconds.

The servo angle getter reports the last instructed target, not measured physical
position. No host test establishes linkage clearance or physical torque. The
feature remains opt-in: calibrate one channel's safe range on the actual mechanism
before enabling it. The parent integrates focus suppression, response dispatch,
polling during playback, stop before microphone capture, and authenticated UI
routes, and independently verifies the final firmware.
