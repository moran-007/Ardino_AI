# Voice capture 1.4.2 verification

## 1.4.3 source-only follow-up, 2026-10-01

### Dialogue controls and billing

Added device web AI settings backed by per-device server profiles: reply style, persona, memory and a search checkbox defaulting off. The global prompt and built-in policies are readable. Added confirmed local chat clear with optional authenticated server-memory clear; neither removes photos, reminders or usage records. Busy conversations reject these operations. Firmware has not been compiled or uploaded for this change; the user will compile manually.

Cloud tests cover default-off search, per-device isolation, genuine DashScope search usage, search-not-executed fallback, memory-clear confirmation, official price context tiers/cache/peak-holiday rules, manual prices and immutable snapshots, CSRF and failed-call accounting. Browser fixtures verify profile save and desktop/mobile overflow. Host UI checks include the production dialogue header; network transport, filesystem deletion and physical-device interaction still require manual-upload acceptance.

### Memory upload optimization

Later live samples isolated the delay: 2.57 s audio / 2.585 s capture / 4.917 s storage; 5.36 s audio / 5.407 s capture / 10.009 s storage; 2.28 s audio / 2.284 s capture / 4.261 s storage. These observations prove latency in the recording persistence phase, not defective flash hardware or a particular garbage-collection mechanism. The user requested optimizing this delay while deferring intermittent write-failure diagnosis.

All dialogue entry points now opt into retaining the existing PSRAM PCM buffer and uploading it directly with the installed Arduino HTTPClient byte-buffer overload. The filesystem mode remains the default for other callers. No extra PCM copy, flash write/read/delete, fallback to internal RAM, or automatic retry after an ambiguous send failure is added. Upload retains bytes for the existing pre-connection-failure retry; after submit returns, bytes are released before waiting for the response. The dialogue scope guard also clears on early returns. The production maximum remains 800,000 bytes. Server diagnostics continue to retain one latest WAV, without cloud changes.

RED after the API scaffolding ran the actual production driver and failed with `memory recording still depends on file storage`. GREEN: 166 capture/session checks, including injected 1,000 ms file-write delays and unavailable file open with successful memory recording and zero writes; first-sample preservation; silence replacing a preceding take; cancellation/allocation failure without retained bytes; repeated cleanup; manual/PTT stop; and the full 25-second cap. Existing file-mode behavior is still covered. Settings/UI regression passes 260 checks, and wake-event/timestamp checks pass.

Self-review traced buffer ownership, request retries, cancellation, no-speech, all three capture branches and cleanup before playback against installed HTTPClient 3.3.12. Its byte-buffer overload synchronously sends until payload size is reached or returns an error; it does not take ownership. No new integration fixture covers real HTTPS sends. No ESP32 firmware build/upload, photo deletion, formatting, partition changes or intermittent file-error repair was attempted. User-side compilation and live upload/timing verification remain required; `recorded` now starts with `PSRAM直传`, status reports `recording_backend=PSRAM`, and capture `storage_ms` should be zero.

The sections below this follow-up describe the historical 1.4.2 release, not the current source. The WakeNet `clean()` call and 50 ms scheduling cancellation guards were removed after device failures. No ESP32 firmware compilation or upload was performed for this follow-up, per the user's instruction.

- Latest diagnostic sample: 43,360 mono frames at 16 kHz (2.71 seconds), RMS 1828, peak 32768; the device recording stage lasted about 6.515 seconds including preparation/storage. This difference is not a measured DMA-overflow count. Recognition was only partly accurate; hardware acoustic improvement is not claimed.
- Acquisition now buffers PCM in PSRAM, then writes the original temporary file after capture. A meaningful RED failed with `storage writes still block microphone acquisition`; GREEN separates 1,600 ms acquisition from 1,750 ms injected storage delay and preserves all 51,200 bytes. Silence does not write flash. Allocation, I/O and cancellation failures remain explicit.
- Web manual recording: authenticated start queues a single turn; the main loop starts it outside the HTTP handler. Finish saves for recognition without requesting abort. Cancel clears the queue and aborts the active pipeline. Manual capture ignores hardware-button release and silence endpoints, resets finish each turn, and retains the 25-second production cap. Saved continuous-conversation preference and normal wake/PTT paths are unchanged.
- UI RED failed with `web manual recording controls missing` before implementation. `tests/voice_settings_host/run.ps1` now passes 260 checks and parses both production scripts. `tests/voice_capture_host/run.ps1` passes 146 checks, including manual finish/save, absent button, stale finish, silence, maximum duration, cancellation and existing hands-free behavior. NS/VAD/I2S/storage remain test doubles, not physical hardware.
- Browser checks use the production-generated HTML and a local HTTP fixture, not the ESP32: start/finish/cancel states, busy 409, 390- and 320-pixel mobile layouts, and desktop screenshots. All manual buttons are visible on a 320-pixel viewport with no overlap or page overflow. Artifacts: `output/playwright/manual-capture-desktop.png` and `manual-capture-mobile.png`. Existing conversation timestamp regression also passes.
- Independent reviewer `/root/capture_timing_review` reran the 146 capture checks and traced auth, main-loop dispatch, capture callback, finish/save, cancellation and single-turn lifecycle. A queued-recording physical-cancel edge was found and fixed by clearing `pendingWebRecording` in the loop's abort branch; read-only re-review returned Review Gate PASS with no remaining blockers.
- The main `.ino` network/route orchestration has no dynamic host integration fixture. Its queue/cancel wiring was reviewed directly; the browser fixture cannot establish actual response latency, firmware compile/link success, DMA continuity or ASR quality.

Checkpoint: isolation checked on `main` with existing user work preserved; meaningful RED, focused GREEN, browser proof and independent review complete. No release package, OTA publication, partition/model change or additional photo deletion was made. Next / upcoming task: user compiles/uploads the current source, then opens Voice and Volume, waits for the recording state after Start, speaks a fixed phrase, and clicks Finish and Send; compare recognized text and the single latest diagnostic recording.

## Scope and isolation

Work started in `E:/moran_project/arduino`, branch `main`, with existing dirty cloud files and an untracked firmware project. No user changes were reverted; no commit or push is authorized. Original firmware inputs are retained under `.codex_tmp/voice-capture-before-20260930`; deployed cloud baselines under `.codex_tmp/voice-deploy-20260930/remote-before`.

Runtime changes: capture settings/NVS and web controls, ESP-SR NS/VAD, no-speech rejection, wake-buffer reset, opt-in conversation lifecycle/cancellation, ASR punctuation rejection, authenticated firmware history and compatibility metadata. Existing display, alarms, memo data, keys, partition layout and wake models are preserved.

## Observed tests

- Baseline production capture RED: synthetic non-speech returned success with 80128 samples, RMS 102, peak 300, 160256 bytes; old caller accepted it for upload.
- New capture GREEN: same noise family yields RMS 104, peak 302, speech false, fileBytes 0; temporary audio removed.
- `tests/voice_capture_host/run.ps1`: 91 production-driver/session checks passed. Includes 190/200 ms speech boundary, clicks, VAD noise, weak energy, first-syllable samples, NS output/bypass, clipping, partial I2S reads, initialization/I/O failures, manual release, web cancellation, full RX drain, ten dialogue rounds, timeout/failure/cancel and clock wrap.
- Slow callback tests RED then GREEN: one 51 ms callback and repeated 25 ms callbacks previously uploaded disrupted audio; now reject and remove the take. A callback over 50 ms also stops playback. Total capture wall-time lag is bounded before reading more samples.
- `tests/voice_settings_host/run.ps1`: 259 settings checks passed, plus two embedded scripts parsed. Actual ArduinoJson parsing, ranges/types/false values, corrupt NVS and failed/short writes are covered.
- `.\pc_server\.venv\Scripts\python.exe -m unittest discover -s cloud_server/tests -q`: 85 tests passed on 2026-10-01. Punctuation ASR tests and history tests were first observed failing before their implementation.
- Playwright real browser: save numeric/boolean settings, busy 409 recovery, stop request, desktop 1280x900 and mobile 390x844. History mobile download links remain within viewport; no page horizontal overflow. Screenshots in `output/playwright/voice-*.png` and `history-*.png`.

## Independent review

Reviewer `/root/voice_diagnosis` checked production capture/SDK contracts, dialogue and wake lifecycle, auth, signed historical downloads and partition compatibility. Findings prompted a 25-second capture cap, per-call and cumulative scheduling guards, cancellation checks before local commands, and a single firmware publish entry with preserved compatibility default. Independent cloud subset: 19 tests passed. Final incremental verdict on 2026-10-01: no blockers; independently reran 91 capture checks and 9 history/update checks. Signed build evidence belongs in the release packet.

The cancellation-entry change uses direct control-flow review plus the full embedded compilation; the monolithic network/local-command path has no host integration fixture. This is an explicit practical test gap, not a claimed dynamic cancellation proof.

## Limits and device acceptance

NS/VAD, I2S, flash and button inputs are test doubles in host capture tests. The real SDK must still compile/link. Acoustic quality, very short-word retention, CPU/memory headroom, actual casing attenuation and DMA timing require the ESP32-S3 device. No physical upgrade or acoustic improvement is claimed by these tests.

After installing: confirm version 1.4.2; start with gain 5, sensitivity 50, noise suppression on; compare fixed-distance phrases with casing on/off and NS on/off; check 30 seconds of silence produces no response; try at least five continuous rounds; stop during recording/playback; reboot and confirm settings persisted. Lower gain when clipped samples rise. This is half-duplex suppression, not acoustic echo cancellation or speaker identification.

## Delivery contract

User authorization: "修复后如果可以通过远程上传升级，服务器端保留升级包，用户可以查到每一个版本". Authorized: package/sign 1.4.2, deploy scoped cloud changes, retain signed historical packages, publish compatible remote OTA. Not authorized or attempted: Git commit/push, changing credentials, erasing device storage, automatic USB flashing.

Minimum source version for 1.4.2 is 1.4.1. Old 1.3.x/1.4.0 requests must receive no manifest; 1.4.1 receives 1.4.2. Never distribute the known-bad 1.1.8. Missing historical binaries are not fabricated.

Rollback: retain previous cloud release `/opt/esp32-voice/releases/20260930-key-usage-040237` and the current.json backup before switching; restore the symlink and restart `voice-gateway` for code rollback. Withdrawing the new release stops new downloads; it does not downgrade already-updated devices. Device application downgrade is not automatic; retain pre-update slot and use USB recovery if needed. No database schema migration is part of this change.

Server staging initially failed because the previous release carried outdated tests and omitted the firmware-update fixture. No production switch occurred on this failure. Copying the current test suite to the staged release resulted in all 85 tests passing on the server. Then the server symlink was switched and health was observed ready; public history requires login and unauthenticated package access returns 401. Previous firmware data is backed up at `/var/lib/esp32-voice/release-backups/20261001-voice-capture/firmware-before`.

Checkpoint: implementation, browser checks and independent review complete; cloud code deployed with passing staged/runtime checks. Next / upcoming task: final embedded build, signature inspection, then compatible firmware publication and authenticated download verification.

## 2026-10-01 Latest Checkpoint (Supersedes Build Plan Above)

- User now compiles/uploads manually; no ESP32 build, flash, signing or new OTA publication was performed for these edits. Source 1.4.3 remains pending real-device acceptance; 1.4.2 remains withdrawn.
- Fixed `DialogueWeb::appendControls` to accept both Arduino String and the production `WebAdmin::PageStream`. Regression uses the actual PageStream header; 263 settings checks and three embedded script parse checks passed.
- Local rule replies for bounded weather/memo queries, Beijing date/time and two-number arithmetic preserve audio segmentation and optional memory without any LLM usage row. Stale, cross-city, complex and modifying requests fall back rather than being guessed or executed by these rules. Local rule replies can operate without any configured LLM, while ASR/TTS still require the server.
- Cloud server: 117 tests passed in Linux staging; local suite passed with one platform-specific skip. Release `/opt/esp32-voice/releases/dialogue-billing-20261001-130323` is active and healthy. Encrypted config/database backup retained under `/var/lib/esp32-voice/release-backups/dialogue-billing-20261001-130323`.
- Key provisioning initially replaced the encrypted file with root ownership; startup failed and code automatically rolled back. Restored voice-gateway ownership with mode 600; old service recovered, then the verified new release deployed successfully. Provisioning helper now explicitly restores service ownership. No credential is printed in verification output or persisted in plaintext files.
- User authorized the provided speech Key and a short connectivity test. Cloud ASR and TTS currently use local-first routing; existing LLM provider order and user-edited profile/search state were preserved. Sambert zhida v1 returned 42400 bytes / 1.325 s of 16k PCM; Qwen Audio 3.1 ASR returned `今天天气怎么样？` on the server in a 1.86 s synthesis-and-recognition test. These are synthetic connectivity checks, not microphone/casing accuracy acceptance.
- Speech calls are separate from LLM Token accounting. Qwen Audio 3.1 Token-based speech pricing is currently marked unpriced because this adapter does not extract its billable tokens; no second-based price is silently substituted. Latest diagnostic user recording was not replaced with synthetic test audio.
- Playwright desktop 1280x900 and mobile 390x844: independent ASR/TTS routes save and reload, Key remains blank, custom external/local model protocol and primary/backup selection persist, no page horizontal overflow. Fixture-only interactions, no production configuration changed by the browser checks.
- Public runtime health passed; unauthenticated device API returns 401 and billing requires admin login. Positive live device API probe was skipped because the device token is only in NVS. No storage deletion or acoustic improvement is claimed.

## 2026-10-01 Speech Selection And Connectivity Check

- Server-only change: manual ASR/TTS model IDs, grouped Sambert voice selector, independent cost-confirmed connectivity buttons and authenticated latest-only in-memory WAV audition. No device build, flash or firmware code change.
- Test requests do not save configuration, route through production fallback, invoke a chat model, or replace the latest diagnostic device recording. Refreshing the GET result does not repeat the cloud call. Model/protocol mismatch is explicit rather than reported as an invalid Key.
- Local suite: 120 tests passed with one platform skip. Linux staging: all 120 passed. Browser fixture verifies voice selection, blank Key after test, PCM/WAV audio metadata and desktop/mobile screenshots with no horizontal overflow; no real Key or quota used by fixture.
- Active healthy release: `/opt/esp32-voice/releases/dialogue-billing-20261001-133111`; rollback backup: `/var/lib/esp32-voice/release-backups/dialogue-billing-20261001-133111`. Encrypted config still belongs to voice-gateway:voice-gateway, mode 600.
- One authorized real Sambert zhiyue test returned 42400 bytes / 1.325 s of 16k PCM in 0.82 s. The encrypted config hash was unchanged. Current user-selected ASR/TTS routes were both cloud-first and were preserved; this supersedes the earlier local-first observation. This is connectivity evidence, not microphone recognition acceptance. Public health remains ready with local SenseVoice / Sherpa VITS available.
