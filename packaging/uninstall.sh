#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_PATH="${HOME}/.local/bin/omarchy-desktop-recorder"
DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/omarchy-desktop-recorder"
MANIFEST="${DATA_DIR}/install.sha256"
CONFIG="${HOME}/.config/hypr/bindings.lua"
BEGIN='-- BEGIN omarchy-desktop-audio-recorder (managed)'
END='-- END omarchy-desktop-audio-recorder (managed)'

die() { printf 'Uninstall error: %s\n' "$*" >&2; exit 1; }
strip_block() {
  awk -v begin="$BEGIN" -v end="$END" '
    $0 == begin { skip=1; next }
    $0 == end { skip=0; next }
    !skip { print }
  ' "$1" > "$2"
}

[[ ${EUID} -ne 0 ]] || die 'Run as the desktop user, not root.'
if [[ -x "$BIN_PATH" ]]; then
  state="$("$BIN_PATH" status 2>/dev/null || true)"
  [[ "$state" == idle ]] || die "Recorder state is '$state'; stop it before uninstalling."
fi
if [[ -e "$CONFIG" ]] && grep -Fq -- "$BEGIN" "$CONFIG"; then
  [[ "$(grep -Fc -- "$BEGIN" "$CONFIG")" == 1 && "$(grep -Fc -- "$END" "$CONFIG")" == 1 ]] || die 'Managed binding markers are inconsistent; no files were changed.'
  command -v hyprctl >/dev/null || die 'hyprctl is required to safely remove the binding.'
  baseline_errors="$(hyprctl configerrors)" || die 'Could not read Hyprland config errors.'
  [[ -z "${baseline_errors//[[:space:]]/}" ]] || die 'Hyprland has config errors; resolve them before uninstalling.'
  backup="${CONFIG}.bak.$(date +%s)"
  cp -p -- "$CONFIG" "$backup"
  temp="$(mktemp "${CONFIG}.tmp.XXXXXX")"
  trap 'rm -f -- "$temp"' EXIT
  strip_block "$CONFIG" "$temp"
  chmod --reference="$CONFIG" "$temp"
  mv -f -- "$temp" "$CONFIG"
  if ! hyprctl reload >/dev/null || ! errors="$(hyprctl configerrors)" || [[ -n "${errors//[[:space:]]/}" ]]; then
    temp="$(mktemp "${CONFIG}.rollback.XXXXXX")"
    cat "$CONFIG" > "$temp"
    printf '\n%s\n' "$BEGIN" >> "$temp"
    cat "$ROOT/integration/hypr/bindings.lua" >> "$temp"
    printf '%s\n' "$END" >> "$temp"
    chmod --reference="$CONFIG" "$temp"
    mv -f -- "$temp" "$CONFIG"
    hyprctl reload >/dev/null 2>&1 || true
    die 'Hyprland validation failed; the owned binding was restored.'
  fi
  printf 'Removed the owned bindings. Config backup: %s\n' "$backup"
fi

if [[ -e "$BIN_PATH" ]]; then
  [[ -f "$MANIFEST" ]] || die "No ownership manifest for $BIN_PATH; leaving it in place."
  installed_hash="$(sha256sum "$BIN_PATH" | cut -d' ' -f1)"
  [[ "$installed_hash" == "$(cat "$MANIFEST")" ]] || die 'Executable was modified; leaving it in place.'
  rm -- "$BIN_PATH"
  rm -- "$MANIFEST"
fi
printf 'Recordings, recovery data, and user configuration were preserved.\n'
