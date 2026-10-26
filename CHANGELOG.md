# Changelog

All notable project changes are recorded here. This project is in early testing; version numbers should be treated as pre-release until broader real-audio testing has been completed.

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
