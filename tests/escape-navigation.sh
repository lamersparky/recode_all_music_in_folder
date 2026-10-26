#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ramif-escape.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
export XDG_DATA_HOME="$TMP/data"
mkdir -p "$TMP/music" "$TMP/out"
: > "$TMP/music/A First.mp3"
: > "$TMP/music/B Second.mp3"

# Escape from the second song's title revisits the previous song's album. Escape
# from the second song's album revisits that song's artist. The active prompt then resumes.
printf 'First Title\nFirst Artist\nFirst Album\n\033\nCorrect First Album\nSecond Title\nWrong Artist\n\033\nCorrect Artist\nSecond Album\n' |
  "$ROOT/recode_all_music_in_folder" --target "$TMP/music" --output "$TMP/out" \
    --title --artist --album --no-clipboard >"$TMP/run.log" 2>&1

python3 - "$XDG_DATA_HOME/recode_all_music_in_folder/ramif_database.db" <<'PY'
import os, sqlite3, sys
c = sqlite3.connect(sys.argv[1])
rows = c.execute('select basename,title,artist,album from files order by file_id').fetchall()
assert len(rows) == 2, rows
assert rows[0] == ('A First.mp3', 'First Title', 'First Artist', 'Correct First Album'), rows
assert rows[1] == ('B Second.mp3', 'Second Title', 'Correct Artist', 'Second Album'), rows
hist = c.execute('select count(*) from metadata_history').fetchone()[0]
assert hist == 6, hist
PY

grep -F 'Returned to the previous album entry' "$TMP/run.log" >/dev/null
grep -F 'Returned to the previous artist entry' "$TMP/run.log" >/dev/null

echo "RAMIF Escape/back-navigation test passed."
