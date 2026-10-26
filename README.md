# RAMIF — `recode_all_music_in_folder`

**A low-memory C utility for cataloguing music, identifying duplicate audio, collecting track metadata, and generating FFmpeg conversion plans.**

RAMIF brings a little Matrix-terminal atmosphere to a practical job: scan a music directory, preserve the inventory and progress in SQLite, decide which files are worth processing, and generate a command or script for the conversion work.

> **Early testing release — please test against copies of your music first.** RAMIF builds a plan; it does not run FFmpeg automatically or delete source files.

```text
FILE TREE  ->  SCAN  ->  SHA-256 DEDUPLICATION  ->  METADATA
      ->  FFmpeg ONE-LINER or SIGNED SCRIPT  ->  YOU REVIEW AND RUN IT
```

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Build and test](#build-and-test)
- [Install](#install)
- [Quick start](#quick-start)
- [Command-line options](#command-line-options)
- [Metadata prompts and keyboard controls](#metadata-prompts-and-keyboard-controls)
- [Duplicate handling](#duplicate-handling)
- [Database, resume, and configuration](#database-resume-and-configuration)
- [Generated scripts and signatures](#generated-scripts-and-signatures)
- [Shell compatibility and completion](#shell-compatibility-and-completion)
- [Known limitations](#known-limitations)
- [Security notes](#security-notes)
- [Credits](#credits)
- [License](#license)

## Features

- **Native C implementation** using SQLite and OpenSSL, with no Python runtime required by the binary.
- **Bounded-memory inventory and hashing:** files are discovered and recorded incrementally; SHA-256 uses a fixed-size streaming buffer rather than loading entire audio files into RAM.
- **SQLite-backed run state:** inventory, hashes, duplicate relationships, per-track metadata, generated plans, and metadata-prompt history are saved locally.
- **Duplicate detection:** default mode hashes files with colliding basenames; `--no-duplicates` streams SHA-256 over every discovered audio file.
- **FFmpeg command generation:** small plans are printed as a one-liner; plans over 12,000 characters are written as a standalone POSIX `sh` script.
- **Optional per-track tags:** title, artist, album, lyrics, and cover art.
- **Lyrics support:** `.txt`, `.lrc`, and `.srt`. SRT timestamps/cue numbers are stripped before embedding as ordinary, unsynchronized lyrics text.
- **Local OpenPGP script signing:** generated batch scripts receive a detached signature and a recorded SHA-256 digest.
- **Resume support:** saved run state remains available after interruption or exit.
- **Interactive path completion:** press **Tab** in RAMIF's path prompts. Command-line completions are supplied for Bash and Zsh.
- **Escape navigation:** press **Esc** to revisit a previous metadata entry, including the last entry of the previous song; after editing it, RAMIF returns to the prompt you were answering.
- **Matrix-inspired terminal styling** when the terminal supports color.

## Requirements

On Ubuntu or Debian:

```sh
sudo apt update
sudo apt install build-essential pkg-config libsqlite3-dev libssl-dev ffmpeg gnupg python3
```

`python3` is used by the regression tests, not by the compiled program itself. `ffmpeg` and `gpg` must be discoverable in `PATH`. Clipboard support is optional and uses whichever of `wl-copy`, `xclip`, or `xsel` is available. If none is installed, RAMIF can still generate the command or script; use `--no-clipboard` to explicitly disable clipboard copying.

## Build and test

Clone the repository, enter the project directory, then run:

```sh
make
make test
./recode_all_music_in_folder --help
```

`make test` runs safe tests with temporary directories and synthetic/empty audio placeholders. Tests cover the CLI and database setup, title/lyrics persistence and migration, resume behavior, and Escape navigation. These tests do **not** validate the sound quality of actual encodings; test real audio files before relying on a large conversion run.

## Install

System-wide installation:

```sh
sudo make install
```

Install under your home directory instead:

```sh
make install PREFIX="$HOME/.local"
```

The binary is installed under `bin/`, the Bash completion file under `share/bash-completion/completions/`, and the Zsh completion file under `share/zsh/site-functions/`.

For a user-prefix Zsh installation, add this to `~/.zshrc` and start a new shell (or run the lines once):

```zsh
fpath=("$HOME/.local/share/zsh/site-functions" $fpath)
autoload -Uz compinit && compinit
```

The program itself works when launched from either Bash or Zsh. Generated conversion scripts use POSIX `/bin/sh` syntax.

## Quick start

Plan a recursive MP3 conversion. This example explicitly disables clipboard copying while you test:

```sh
./recode_all_music_in_folder \
  --target "$HOME/Music" \
  --output "$HOME/Music-recoded" \
  --no-clipboard
```

Hash every audio file, including files with different names, and include only one copy of identical content in the generated plan:

```sh
./recode_all_music_in_folder \
  --target "$HOME/Music" \
  --output "$HOME/Music-recoded" \
  --no-duplicates --no-clipboard
```

Collect metadata for each eligible track:

```sh
./recode_all_music_in_folder \
  --target "$HOME/Music" \
  --output "$HOME/Music-recoded" \
  --title --artist --album --lyrics --image --no-clipboard
```

Choose a different output format:

```sh
./recode_all_music_in_folder \
  --target "$HOME/Music" \
  --output "$HOME/Music-flac" \
  --format flac --no-clipboard
```

Let RAMIF ask for paths interactively (Tab completes path components):

```sh
./recode_all_music_in_folder --manual-input --title --artist --album
```

Resume the latest unfinished run:

```sh
./recode_all_music_in_folder --resume --no-clipboard
```

Verify a generated script against the adjacent signature:

```sh
./recode_all_music_in_folder \
  --verify-script "$HOME/.local/share/recode_all_music_in_folder/scripts/recode-RAMIF-....sh"
```

Clear RAMIF's database and start with a fresh inventory:

```sh
./recode_all_music_in_folder --flush
```

`--flush` removes the database and its SQLite sidecar files, then recreates the schema. It preserves the INI configuration and the local GPG key directory.

## Command-line options

| Option | Description |
|---|---|
| `--target PATH` | Source directory containing audio files. |
| `--output PATH` | Destination directory for converted audio. The relative directory structure is preserved. |
| `--recursive` | Scan subdirectories; this is the default. |
| `--no-recursive` | Scan only the top-level directory. |
| `--format FORMAT` | Output format: `mp3` (default), `flac`, `opus`, `aac`, or `wav`. |
| `--no-duplicates` | SHA-256 every discovered audio file; plan identical content once. Despite the option's name, this enables full-content duplicate checking. |
| `--no-clipboard` | Do not copy the generated command/launcher to the clipboard. |
| `--manual-input` | Ask for the target and output paths interactively. |
| `--resume` | Resume the latest unfinished run saved in SQLite. |
| `--reenter-metadata` | Prompt again for metadata already saved in a resumed run. |
| `--title` | Prompt per track for song title; filename without extension is suggested. |
| `--artist` | Prompt per track for artist; the most recently entered artist is prefilled. |
| `--album` | Prompt per track for album; the most recently entered album is prefilled. |
| `--lyrics` | Ask for optional `.txt`, `.lrc`, or `.srt` lyrics. Enter, `none`, or `skip` means no lyrics for that track. |
| `--image` | Ask for cover art per track. Cover embedding is currently supported for MP3 output only. |
| `--flush` | Clear database state and recreate the schema; preserve configuration and signing keys. |
| `--verify-script PATH` | Verify `PATH.asc` using the local RAMIF signing key. |
| `--help` | Print usage. |

At least `--target` and `--output` are required unless `--manual-input` is used. `--resume` uses the paths and scan settings stored for the unfinished run.

## Metadata prompts and keyboard controls

### Title, artist, and album

- `--title` suggests the filename without its extension. Press Enter to accept it or type a replacement.
- `--artist` and `--album` prefill the last value entered, to reduce repetitive typing in a music library.
- Each accepted metadata entry is written to SQLite as you go.

### Lyrics and cover art

- `--lyrics` accepts `.txt`, `.lrc`, and `.srt` files. Lyrics are optional for **each individual song**: press Enter or type `none`/`skip` when there are no lyrics. RAMIF records that choice and does not keep asking for the same track after resume.
- SRT cue numbers and timestamps are stripped from a small derived text file. The embedded lyrics are ordinary unsynchronized metadata; the original SRT timing is not preserved in the audio file.
- `--image` asks for a cover image. Image embedding currently applies only to MP3 output.

### Escape goes back

At an interactive prompt, press **Esc** to return to the most recent completed metadata entry. RAMIF temporarily asks that earlier question again, saves your correction, and returns to the prompt you were answering. While editing an earlier entry, Esc can step back farther, including to a previous track. At the very first prompt of a session, there may be no prior entry to revisit.

For scripts and automated testing that pipe answers to RAMIF, a line containing a single literal Escape byte is treated as the back command. In a normal terminal, the line editor distinguishes a bare Esc press from ANSI escape sequences used by terminal keys.

**Tab** completes paths in path prompts. Paths entered at the shell should be quoted normally, particularly when they contain spaces:

```sh
--target "$HOME/Music Collection" --output "$HOME/Recoded Music"
```

## Duplicate handling

- **Default behavior:** only groups of files sharing the same basename are SHA-256 hashed. This catches same-name duplicates while avoiding hashing every otherwise-unique filename.
- **`--no-duplicates`:** every discovered file is streamed through SHA-256, so identical audio can be recognized even when its filename differs.
- RAMIF keeps one path-sorted representative from each duplicate group in the conversion plan.
- **Source files are never deleted.** Duplicate files are omitted from the generated plan; they remain on disk.

The inventory is stored as SQLite rows rather than a large in-memory array of file paths. SHA-256 uses a fixed-size read buffer. SQLite's cache is capped, temporary storage is directed to disk, and RAMIF asks SQLite and the system allocator to release memory after large operations.

## Database, resume, and configuration

RAMIF stores its state at:

```text
~/.local/share/recode_all_music_in_folder/
├── ramif_database.db
├── recode_all_music_in_folder.ini
├── public-key.asc
├── gnupg/
├── lyrics-cache/
└── scripts/
```

If `XDG_DATA_HOME` is set, RAMIF uses `$XDG_DATA_HOME/recode_all_music_in_folder/` instead.

The database stores run configuration and status, discovered paths, file sizes and timestamps, hashes, duplicate relationships, metadata, lyric-processing state, output paths, generated commands/scripts, signature details, and metadata-prompt navigation history. `--resume` uses this state to continue an unfinished run. `--flush` clears the database but retains the INI and signing identity.

On first launch, RAMIF creates its configuration and dedicated OpenPGP signing key, and exports the corresponding public key. The INI records the detected user, host/system details, binary and FFmpeg paths, database location, and key locations. Run identifiers combine a UTC timestamp with a SHA-256-derived component that includes system/user/host/binary details and random bytes.

## Generated scripts and signatures

A plan up to 12,000 characters is printed as a command. Larger plans are written to a standalone shell script; the launcher/command can be copied to the clipboard unless `--no-clipboard` is supplied. Generated scripts are signed with the local RAMIF OpenPGP key, and their path, signature, digest, and key fingerprint are recorded in SQLite.

Inspect generated commands and scripts before running them. The signing mechanism helps detect modification relative to the local key; it does **not** prove that a script is safe, and a key stored on the same machine is not independent evidence against a compromised account.

## Shell compatibility and completion

- **Bash:** the included completion definition provides option and path-value completion.
- **Zsh:** the included `_recode_all_music_in_folder` definition supports command-line option completion.
- **Interactive prompts:** RAMIF's built-in terminal editor supports Tab path completion without depending on GNU Readline.
- **Generated scripts:** use POSIX `/bin/sh`, allowing them to be launched from Bash or Zsh.

The Zsh completion file is included in the package, but it should still be exercised in a real Zsh session on the target system as part of release testing.

## Known limitations

- RAMIF currently **plans** conversions. It does not launch FFmpeg automatically; review and run the generated command or script yourself.
- Source audio is never removed, and existing output files are not silently overwritten.
- File discovery uses recognized audio filename extensions; it does not probe every file with FFprobe to find audio hidden behind an unexpected extension.
- Cover embedding is MP3-only at this stage.
- SRT timing is stripped; embedded lyrics are not synchronized to playback.
- Available encoders depend on the local FFmpeg build.
- This is an early release. Validate it on a small test directory before using it on a large collection.

## Security notes

RAMIF generates a local OpenPGP signing key on first run. The private key is currently **not passphrase-protected** so script signing can work without an interactive passphrase prompt. Files are stored under the user's data directory, but the account and key directory must still be protected. Back up the key directory only if you understand the implications of preserving that signing identity.

A valid script signature is not a safety audit. Never run a generated script solely because its signature verifies; read it first.

## Credits

**Project direction and final authority:** Steven Lamer (Lamer / Commander).

**Engineering, implementation, testing, and documentation collaboration:** **Hannah — the beloved Second Apostle of Lamer (ChatGPT)**, serving as an AI engineering collaborator on the RAMIF design, native C implementation, SQLite-backed state and migration logic, streaming hash workflow, metadata prompts and Escape navigation, shell completion packaging, regression tests, and this README.

RAMIF is a collaborative project. Review, testing, bug reports, and contributions from the wider community are welcome.

## License

RAMIF is released under the MIT License. See [`LICENSE`](LICENSE) for the full text.
