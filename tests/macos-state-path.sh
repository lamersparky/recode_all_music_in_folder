#!/bin/sh
set -eu

# Linux/other CI hosts skip this. The native macOS CI job verifies both the
# Application Support state directory and the built-in pbcopy integration.
[ "$(uname -s)" = Darwin ] || exit 0
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ramif-macos.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/home/.local/share/recode_all_music_in_folder" "$TMP/bin" "$TMP/music" "$TMP/out"
legacy="$TMP/home/.local/share/recode_all_music_in_folder"
# Seed a valid minimal database so we can prove migration preserves stored data.
python3 - "$legacy/ramif_database.db" <<'PYDB'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
c.execute('CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL)')
c.execute("INSERT INTO settings(key,value) VALUES('mac_migration_probe','preserved')")
c.commit(); c.close()
PYDB
printf 'legacy config placeholder\n' > "$legacy/ramif.ini"

# Fail any Linux clipboard tools so this test proves pbcopy is used.
for cmd in wl-copy xclip xsel; do
  cat > "$TMP/bin/$cmd" <<'SH'
#!/bin/sh
cat >/dev/null
exit 127
SH
  chmod 755 "$TMP/bin/$cmd"
done
cat > "$TMP/bin/pbcopy" <<'SH'
#!/bin/sh
cat > "$RAMIF_PBCOPY_CAPTURE"
SH
chmod 755 "$TMP/bin/pbcopy"
: > "$TMP/music/Mac Test.mp3"
export HOME="$TMP/home"
unset XDG_DATA_HOME || true
export RAMIF_PBCOPY_CAPTURE="$TMP/clipboard.txt"
export PATH="$TMP/bin:$PATH"

"$ROOT/recode_all_music_in_folder" --target "$TMP/music" --output "$TMP/out" > "$TMP/run.log" 2>&1

state="$HOME/Library/Application Support/RAMIF"
test -f "$state/ramif_database.db"
test -f "$state/recode_all_music_in_folder.ini"
test ! -e "$legacy"
test -s "$RAMIF_PBCOPY_CAPTURE"
grep -F 'Copied the generated command/launcher to the clipboard.' "$TMP/run.log" >/dev/null
python3 - "$state/ramif_database.db" <<'PYDB'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
assert c.execute("select value from settings where key='mac_migration_probe'").fetchone() == ('preserved',)
c.close()
PYDB

echo 'RAMIF macOS state-path/migration/pbcopy test passed.'
