#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-}"
OUTPUT_DIR="${2:-$ROOT/dist}"

if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  printf 'Usage: %s VERSION [OUTPUT_DIR]\n' "$0" >&2
  exit 2
fi

command -v make >/dev/null || { printf 'make is required.\n' >&2; exit 1; }
command -v tar >/dev/null || { printf 'tar is required.\n' >&2; exit 1; }
command -v sha256sum >/dev/null || { printf 'sha256sum is required.\n' >&2; exit 1; }

make -C "$ROOT"
mkdir -p -- "$OUTPUT_DIR"

ARCH="$(uname -m)"
NAME="omarchy-desktop-recorder-${VERSION}-linux-${ARCH}"
ARCHIVE="$OUTPUT_DIR/$NAME.tar.gz"
CHECKSUM="$ARCHIVE.sha256"
[[ ! -e "$ARCHIVE" && ! -e "$CHECKSUM" ]] || {
  printf 'Refusing to overwrite existing release files for %s.\n' "$VERSION" >&2
  exit 1
}

STAGE="$(mktemp -d)"
trap 'rm -rf -- "$STAGE"' EXIT
PACKAGE="$STAGE/$NAME"
mkdir -p "$PACKAGE/packaging" "$PACKAGE/integration/hypr"
install -m 0755 "$ROOT/omarchy-desktop-recorder" "$PACKAGE/omarchy-desktop-recorder"
install -m 0755 "$ROOT/packaging/install.sh" "$PACKAGE/packaging/install.sh"
install -m 0755 "$ROOT/packaging/uninstall.sh" "$PACKAGE/packaging/uninstall.sh"
install -m 0644 "$ROOT/integration/hypr/bindings.lua" "$PACKAGE/integration/hypr/bindings.lua"
install -m 0644 "$ROOT/README.md" "$PACKAGE/README.md"

tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
  -czf "$ARCHIVE" -C "$STAGE" "$NAME"
(cd -- "$OUTPUT_DIR" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
tar -tzf "$ARCHIVE" | grep -Fx "$NAME/packaging/install.sh" >/dev/null
tar -tzf "$ARCHIVE" | grep -Fx "$NAME/omarchy-desktop-recorder" >/dev/null
(cd -- "$OUTPUT_DIR" && sha256sum --check "$NAME.tar.gz.sha256")

printf 'Created %s\nCreated %s\n' "$ARCHIVE" "$CHECKSUM"
