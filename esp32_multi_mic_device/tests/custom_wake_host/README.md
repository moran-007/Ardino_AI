# Experimental custom wake verification

Run `./run.ps1` in PowerShell. It builds the real `custom_wake.cpp` and
`offline_wake.cpp` with local filesystem, microphone and RTOS test seams.
`baseline.ps1` compiles the existing fixed-model lookup and observes rejection
of the arbitrary label `小默小默` before acoustic enrollment was implemented.

The feature is speaker dependent acoustic template matching, not speech-to-text
and not retraining Espressif models. One three-second foreground recording stores
one trimmed, normalized 12-band log-mel spectral sequence. A maximum of three
sequences are persisted in `fs::FS` with a checksum and atomic temporary/backup
replacement. Raw audio is never persisted or sent for enrollment/listening.
Changing the descriptive label invalidates old templates. The feature starts
disabled and requires at least one valid sample before it can be enabled.

Host checks cover actual FFT/VAD/DTW matching under gain/tempo/noise variation,
unrelated sounds, reversed order, silence/noise, reload, corrupt data, silence,
clipping, duration limits, microphone failure, queued/in-flight cancellation,
pause timeout, short writes, rename/write failure, three-sample limit, label
invalidation, incomplete backup deletion, and explicit repair after corruption.
An existing primary is authoritative: a corrupt primary fails closed rather
than resurrecting older enabled biometric templates. Only a missing primary
with a valid backup and successful rename is recovered as an interrupted save.
If old backup deletion fails after reset, the new empty primary stays active,
the operation reports failure/retained-data status, and explicit retry remains
available. Host Arduino headers expose PI/constrain macros like the real build.
Offline listener checks run in both model-present and
model-missing modes, verifying custom dispatch/one event/pause and official
WakeNet restoration without changing its chunk size or model count, plus exact
official -1/0/+1 dispatch behavior. The UI's
actual JavaScript is exercised for queue/record/cancel/success/enable/error/reset
and polling only while the voice panel is open or training is pending.

These synthetic sound checks demonstrate functioning acoustic enrollment and
matching; they do not measure real wake-word accuracy. Before device acceptance,
record the intended phrase three times on the actual microphone, test 20 repeats
at different distances and gains, and test ordinary conversation/TV/background
noise for false triggers. Adjust the conservative matching threshold based on
that evidence. Verify recording cancellation and Web status on the actual ESP32.
Whole-firmware build and microphone/RTOS timing are validated by the parent task;
this slice does not flash, upload, commit or alter official model bundles.
