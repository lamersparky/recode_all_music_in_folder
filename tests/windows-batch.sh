#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ramif-windows-batch.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
export XDG_DATA_HOME="$TMP/data"
mkdir -p "$TMP/music" "$TMP/out"
# Extension-only discovery is intentional: this test does not execute FFmpeg.
printf 'synthetic audio placeholder' > "$TMP/music/Song & One.mp3"
printf '1\n00:00:01,000 --> 00:00:03,000\nLyrics for Windows\n' > "$TMP/lyrics.srt"
# Minimal valid 1x1 PNG; enough to test asset bundling without external test fixtures.
python3 - "$TMP/cover.png" <<'PY'
import base64, pathlib, sys
png = ('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=')
pathlib.Path(sys.argv[1]).write_bytes(base64.b64decode(png))
PY

printf '%s\n%s\n%s\n%s\n%s\n' 'A & B "quoted"' "O'Reilly" 'The Test Album' "$TMP/lyrics.srt" "$TMP/cover.png" \
  | "$ROOT/recode_all_music_in_folder" \
      --target "$TMP/music" --output "$TMP/out" --windows-batch \
      --title --artist --album --lyrics --image --no-clipboard >"$TMP/run.log" 2>&1

scripts="$XDG_DATA_HOME/recode_all_music_in_folder/scripts"
bat=
for candidate in "$scripts"/recode-*.bat; do
    [ -f "$candidate" ] || continue
    bat=$candidate
    break
done
test -n "$bat"
ps1=${bat%.bat}.ps1
assets=${bat%.bat}-assets
[ -f "$ps1" ]
[ -f "$bat.asc" ]
[ -f "$ps1.asc" ]
[ -f "$assets/lyrics/1.txt" ]
[ -f "$assets/images/1.png" ]
grep -F 'powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass' "$bat" >/dev/null
grep -F '$ffmpegCommand' "$ps1" >/dev/null
grep -F 'Get-FileHash' "$ps1" >/dev/null
grep -F 'LyricsHash' "$ps1" >/dev/null
grep -F 'ImageHash' "$ps1" >/dev/null
if grep -E 'A & B|O.Reilly|Song & One' "$ps1" >/dev/null; then
    echo 'Metadata should be Base64-encoded in the generated PowerShell worker.' >&2
    exit 1
fi
grep -F 'WINDOWS BATCH PACKAGE' "$TMP/run.log" >/dev/null
grep -F -- '--no-clipboard selected' "$TMP/run.log" >/dev/null

# Both generated payloads should verify against the local key that signed them.
"$ROOT/recode_all_music_in_folder" --verify-script "$bat" >"$TMP/verify-bat.log" 2>&1
grep -F 'Signature valid.' "$TMP/verify-bat.log" >/dev/null
"$ROOT/recode_all_music_in_folder" --verify-script "$ps1" >"$TMP/verify-ps1.log" 2>&1
grep -F 'Signature valid.' "$TMP/verify-ps1.log" >/dev/null

python3 - "$XDG_DATA_HOME/recode_all_music_in_folder/ramif_database.db" "$ps1" <<'PY'
import base64, re, sqlite3, sys
c = sqlite3.connect(sys.argv[1])
assert c.execute("select count(*) from scripts where path like '%.bat' or path like '%.ps1'").fetchone()[0] == 2
assert c.execute("select status, windows_batch from runs").fetchone() == ('PLANNED', 1)
assert c.execute('select title,artist,album,relpath from files').fetchone() == ('A & B "quoted"', "O'Reilly", 'The Test Album', 'Song & One.mp3')
worker = open(sys.argv[2], encoding='utf-8').read()
values = {base64.b64decode(v).decode('utf-8') for v in re.findall(r"FromBase64String\('([^']*)'\)", worker)}
assert 'A & B "quoted"' in values
assert "O'Reilly" in values
assert 'Song & One.mp3' in values
assert 'The Test Album' in values
PY
echo 'RAMIF Windows batch-package generation/signature test passed.'
