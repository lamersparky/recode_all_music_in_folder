# Changelog

All notable project changes are recorded here. This project is in early testing; version numbers should be treated as pre-release until broader real-audio testing has been completed.

## 0.2.0 — macOS support and cross-platform source consistency

- Added native macOS CLI build support while keeping Linux and macOS on the same C source file.
- Added native `pbcopy` clipboard integration and the default `~/Library/Application Support/RAMIF/` state directory; honors `XDG_DATA_HOME` when set and attempts to migrate an existing Linux-style state directory on first macOS launch.
- Guarded glibc-only memory trimming, changed the Makefile install rules to portable `install -d` / `install -m`, and added macOS CI for Apple Silicon and Intel.
- Synchronized the C source with the already documented/tested `--windows-batch` feature and made its package-generation regression test portable to BSD/macOS `find` behavior.
- Added a macOS state-path/clipboard integration test. Native macOS compilation is validated by the macOS CI workflow, not by a Linux build.

## 0.1.1 — Windows package generation and release polish

- Added documented `--windows-batch` / `--batch-script` Windows package workflow: `.bat` launcher, PowerShell conversion worker, bundled assets, detached signatures, and asset SHA-256 checks.
- Added automated regression coverage for Windows package generation, lyric/image bundling, SQLite records, and detached signature verification.
- Added Windows options to Bash and Zsh completion definitions.
- Clarified that RAMIF currently generates Windows packages from the supported Linux scanner build; Windows users can run the resulting package without a Linux VM.

## 0.1.0 — Initial public-testing package

- Native C command-line program with SQLite-backed run state and incremental audio inventory.
- Streaming SHA-256 duplicate detection with a basename-only default and full-content mode via `--no-duplicates`.
- FFmpeg one-liner generation and standalone POSIX shell-script generation for larger plans.
- Optional title, artist, album, lyrics, and MP3 cover-art prompts.
- Per-track optional lyrics behavior for TXT/LRC/SRT inputs, with SRT timing stripped before embedding as ordinary lyrics text.
- Local OpenPGP signing and SHA-256 records for generated scripts.
- First-run INI configuration, resume support, and `--flush`.
- Built-in Tab path completion and Bash/Zsh option-completion files.
- Escape-key navigation to revisit previous metadata entries, including entries from the preceding track.
- Regression tests for CLI/database setup, metadata persistence and migration, resumability, and Escape navigation.
