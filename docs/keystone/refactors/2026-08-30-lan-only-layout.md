# LAN-only project layout refactor

## Goal

Turn the current mixed Arduino experiment workspace into one self-contained LAN voice-dialogue project that can be cloned, configured, tested, and extended with a future cloud-server implementation.

## Behavior invariants

- The ESP32-S3 firmware still records 16 kHz mono PCM and sends it to the PC over the LAN.
- The PC server still performs Zipformer ASR, calls the configured DeepSeek endpoint, uses Windows offline TTS, and streams PCM back to the device.
- BLE provisioning, UDP server discovery, the phone web UI, conversation history, settings, and device-token behavior remain unchanged.
- Local secrets, runtime jobs, downloaded models, virtual environments, and build outputs remain local and are never tracked by Git.

## Pressures addressed

- The LAN server currently reaches into two unrelated experiment/root directories for `audio_frontend.py`, the ASR model, and `windows_speech.ps1`.
- The workspace mixes legacy serial firmware, multiple experiments, generated binaries, recordings, browser artifacts, and the LAN implementation.
- Fresh setup is split across two setup scripts and the LAN README does not describe the external model dependency.

## Affected areas

- Move the LAN firmware and PC server to top-level project directories.
- Move the shared audio frontend and Windows TTS helper into `pc_server`.
- Consolidate Python environment installation and Zipformer model download into `pc_server/setup_server.ps1`.
- Update configuration defaults and documentation for the new paths.
- Remove legacy serial code, unrelated experiments, unused models, caches, runtime output, and build artifacts.
- Preserve the cloud-deployment research as a roadmap under `docs/`.

## Slices

1. Create a recoverable source backup outside the repository.
2. Move the retained LAN files without editing behavior.
3. Update path ownership and setup scripts.
4. Remove exact obsolete paths and generated content.
5. Validate ignore rules and scan the staged set for secrets.
6. Run PC unit tests, audio-frontend tests, a real local model load, and an ESP32-S3 compile.

## Proof

- `python -m unittest -v pc_server/test_server_components.py`
- `python -m unittest -v pc_server/test_audio_frontend.py`
- Load `ServerConfig` and construct `ZipformerRecognizer` with the retained local model.
- `arduino-cli compile` for the documented ESP32-S3 FQBN, with build output outside the repository.
- Inspect `git status`, `git check-ignore`, staged file sizes, and a redacted secret scan before commit.

## Rollback

The pre-refactor source snapshot is stored at `E:\moran_project\arduino_cleanup_backup_20260830_093500`. Downloaded dependencies can be recreated with the retained setup script. No GitHub state is changed until review and proof pass.

## Review focus

- Fresh-clone path correctness and model download behavior.
- No local API key, Wi-Fi credential, runtime audio, virtual environment, or model binary enters Git.
- Existing LAN API routes and firmware protocol strings remain compatible.
- Documentation describes only the retained LAN project and clearly marks future cloud work as unimplemented.
