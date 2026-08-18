#!/usr/bin/env sh
set -eu

VERSION="3530400"
YEAR="2026"
ARCHIVE="sqlite-amalgamation-${VERSION}.zip"
URL="https://www.sqlite.org/${YEAR}/${ARCHIVE}"
EXPECTED_SHA3="628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e"
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VENDOR_DIR="$ROOT/vendor/sqlite"
ARCHIVE_PATH="$VENDOR_DIR/$ARCHIVE"
TARGET_DIR="$VENDOR_DIR/sqlite-amalgamation-${VERSION}"

mkdir -p "$VENDOR_DIR"

if [ -f "$TARGET_DIR/sqlite3.c" ] && [ -f "$TARGET_DIR/sqlite3.h" ]; then
  echo "SQLite $VERSION already present at $TARGET_DIR"
  exit 0
fi

printf 'Downloading %s\n' "$URL"
curl --fail --location --proto '=https' --tlsv1.2 "$URL" --output "$ARCHIVE_PATH"

python3 - "$ARCHIVE_PATH" "$EXPECTED_SHA3" <<'PY'
import hashlib
import pathlib
import sys
path = pathlib.Path(sys.argv[1])
expected = sys.argv[2].lower()
h = hashlib.sha3_256()
with path.open('rb') as f:
    for chunk in iter(lambda: f.read(1024 * 1024), b''):
        h.update(chunk)
actual = h.hexdigest()
if actual != expected:
    raise SystemExit(f"SHA3-256 mismatch: expected {expected}, got {actual}")
print(f"Verified SHA3-256: {actual}")
PY

rm -rf "$TARGET_DIR"
unzip -q "$ARCHIVE_PATH" -d "$VENDOR_DIR"
rm -f "$ARCHIVE_PATH"
printf 'SQLite source ready at %s\n' "$TARGET_DIR"
