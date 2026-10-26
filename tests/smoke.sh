#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ramif-smoke.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
export XDG_DATA_HOME="$TMP/data"
"$ROOT/recode_all_music_in_folder" --help >/dev/null
"$ROOT/recode_all_music_in_folder" --flush >/dev/null
DB="$XDG_DATA_HOME/recode_all_music_in_folder/ramif_database.db"
test -f "$DB"
python3 - "$DB" <<'PY'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
assert conn.execute("select count(*) from runs").fetchone()[0] == 0
assert conn.execute("select count(*) from files").fetchone()[0] == 0
print("RAMIF smoke test passed.")
PY
