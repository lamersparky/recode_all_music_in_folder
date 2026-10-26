#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ramif-metadata.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

# Fresh database: title defaults to the input filename and an empty lyric choice is saved as a skip.
export XDG_DATA_HOME="$TMP/fresh-data"
mkdir -p "$XDG_DATA_HOME" "$TMP/music" "$TMP/out"
: > "$TMP/music/A Song.mp3"
printf '\nnone\n' | "$ROOT/recode_all_music_in_folder" --target "$TMP/music" --output "$TMP/out" --title --lyrics --no-clipboard >"$TMP/fresh.log" 2>&1
grep -F -- "-metadata 'title=A Song'" "$TMP/fresh.log" >/dev/null
python3 - "$XDG_DATA_HOME/recode_all_music_in_folder/ramif_database.db" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
assert c.execute('select ask_title, ask_lyrics from runs').fetchone() == (1, 1)
assert c.execute('select title, lyrics_source, lyrics_path, lyrics_prompted from files').fetchone() == ('A Song', None, None, 1)
PY

# A resume must remember that this track intentionally had no lyrics and not prompt again.
"$ROOT/recode_all_music_in_folder" --resume --no-clipboard </dev/null >"$TMP/resume.log" 2>&1
if grep -E 'Song title for|Lyrics for' "$TMP/resume.log" >/dev/null; then
    echo "RAMIF unexpectedly re-prompted for saved title/lyrics skip" >&2
    exit 1
fi

# Compatibility check: migrate the database schema from the release before --title/lyrics_prompted.
export XDG_DATA_HOME="$TMP/legacy-data"
mkdir -p "$XDG_DATA_HOME/recode_all_music_in_folder" "$TMP/legacy-music" "$TMP/legacy-out"
: > "$TMP/legacy-music/legacy.wav"
python3 - "$XDG_DATA_HOME/recode_all_music_in_folder/ramif_database.db" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
c.executescript('''
CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);
CREATE TABLE runs(run_uid TEXT PRIMARY KEY,created_at TEXT NOT NULL,target TEXT NOT NULL,output TEXT NOT NULL,
 recursive INTEGER NOT NULL,dedupe_all INTEGER NOT NULL,ask_artist INTEGER NOT NULL,ask_album INTEGER NOT NULL,
 ask_lyrics INTEGER NOT NULL,ask_image INTEGER NOT NULL,format TEXT NOT NULL,status TEXT NOT NULL,
 command_text TEXT,generated_script TEXT,script_signature TEXT,script_sha256 TEXT,default_artist TEXT,default_album TEXT,last_error TEXT);
CREATE TABLE files(file_id INTEGER PRIMARY KEY AUTOINCREMENT,run_uid TEXT NOT NULL,path TEXT NOT NULL,relpath TEXT NOT NULL,basename TEXT NOT NULL,
 size INTEGER NOT NULL,mtime INTEGER NOT NULL,sha256 TEXT,hash_status INTEGER NOT NULL DEFAULT 0,duplicate_of INTEGER,
 status TEXT NOT NULL DEFAULT 'DISCOVERED',artist TEXT,album TEXT,lyrics_source TEXT,lyrics_path TEXT,image_path TEXT,output_path TEXT,error TEXT,
 UNIQUE(run_uid,path),FOREIGN KEY(run_uid) REFERENCES runs(run_uid));
CREATE INDEX files_run_status ON files(run_uid,status);
CREATE INDEX files_run_name ON files(run_uid,basename);
CREATE INDEX files_run_hash ON files(run_uid,sha256);
CREATE TABLE scripts(script_id INTEGER PRIMARY KEY AUTOINCREMENT,run_uid TEXT NOT NULL,path TEXT NOT NULL,
 signature_path TEXT NOT NULL,sha256 TEXT NOT NULL,fingerprint TEXT NOT NULL,created_at TEXT NOT NULL,
 FOREIGN KEY(run_uid) REFERENCES runs(run_uid));
''')
c.close()
PY
"$ROOT/recode_all_music_in_folder" --target "$TMP/legacy-music" --output "$TMP/legacy-out" --no-clipboard >"$TMP/legacy.log" 2>&1
python3 - "$XDG_DATA_HOME/recode_all_music_in_folder/ramif_database.db" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
runs = {r[1] for r in c.execute('pragma table_info(runs)')}
files = {r[1] for r in c.execute('pragma table_info(files)')}
assert 'ask_title' in runs
assert 'title' in files and 'lyrics_prompted' in files
assert c.execute('select count(*) from files').fetchone()[0] == 1
PY

echo "RAMIF metadata/resume/migration tests passed."
