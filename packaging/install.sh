#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${HOME}/.local/bin"
DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/omarchy-desktop-recorder"
BIN_PATH="${BIN_DIR}/omarchy-desktop-recorder"
MANIFEST="${DATA_DIR}/install.sha256"
CONFIG="${HOME}/.config/hypr/bindings.lua"
BEGIN='-- BEGIN omarchy-desktop-audio-recorder (managed)'
END='-- END omarchy-desktop-audio-recorder (managed)'

die() { printf 'Install error: %s\n' "$*" >&2; exit 1; }
rollback_block() {
  local source="$1" target="$2"
  awk -v begin="$BEGIN" -v end="$END" '
    $0 == begin { skip=1; next }
    $0 == end { skip=0; next }
    !skip { print }
  ' "$source" > "$target"
}

[[ ${EUID} -ne 0 ]] || die 'Run as the desktop user, not root.'
command -v make >/dev/null || die 'make is required to build the application.'
command -v hyprctl >/dev/null || die 'hyprctl is required to validate the F10 integration.'
command -v jq >/dev/null || die 'jq is required to inspect active keybindings.'
command -v ffmpeg >/dev/null || die 'ffmpeg is required at runtime.'
command -v flac >/dev/null || die 'flac is required for final integrity checks.'
command -v omarchy >/dev/null || die 'the Omarchy notification helper is required.'
pkg-config --exists gtk4 libpulse libpulse-mainloop-glib || die 'GTK4 and libpulse development files are required to build.'
[[ -f /usr/include/gtk4-layer-shell/gtk4-layer-shell.h ]] || die 'gtk4-layer-shell headers are required to build.'
[[ -f "$CONFIG" && ! -L "$CONFIG" ]] || die "Expected a regular Hyprland bindings file at $CONFIG."
[[ -f "$ROOT/omarchy-desktop-recorder" ]] || make -C "$ROOT"

mkdir -p "$BIN_DIR" "$DATA_DIR"
if [[ -e "$BIN_PATH" ]]; then
  [[ -f "$MANIFEST" ]] || die "Refusing to replace an unmanaged executable at $BIN_PATH."
  current_hash="$(sha256sum "$BIN_PATH" | cut -d' ' -f1)"
  [[ "$current_hash" == "$(cat "$MANIFEST")" ]] || die "Installed executable was modified; refusing to overwrite it."
fi

if grep -Fq -- "$BEGIN" "$CONFIG"; then
  [[ "$(grep -Fc -- "$BEGIN" "$CONFIG")" == 1 && "$(grep -Fc -- "$END" "$CONFIG")" == 1 ]] || die 'Managed binding markers are inconsistent.'
  already_installed=1
else
  already_installed=0
  jq -e '[.[] | select((.key == "F10") or (.keycode == 68))] | length == 0' \
    < <(hyprctl -j binds) >/dev/null || die 'F10 or keycode 68 is already bound in Hyprland. No binding was changed.'
  if grep -Eq 'o\.bind\([^\n]*F10|hl\.bind\([^\n]*F10|keycode[^\n]*68' "$CONFIG"; then
    die 'The user bindings file contains an F10/keycode 68 binding; review it before installation.'
  fi
fi

baseline_errors="$(hyprctl configerrors)" || die 'Could not read Hyprland config errors.'
[[ -z "${baseline_errors//[[:space:]]/}" ]] || die 'Hyprland already reports config errors; resolve them before installing.'

stage_bin="$(mktemp "${BIN_DIR}/.omarchy-desktop-recorder.XXXXXX")"
old_bin=""
old_manifest=""
had_old=0
if [[ -e "$BIN_PATH" ]]; then
  old_bin="$(mktemp "${BIN_DIR}/.omarchy-desktop-recorder-old.XXXXXX")"
  cp -p -- "$BIN_PATH" "$old_bin"
  old_manifest="$(cat "$MANIFEST")"
  had_old=1
fi
tmp_config=""
backup=""
changed_config=0
finish() {
  rm -f -- "$stage_bin"
  [[ -z "$old_bin" ]] || rm -f -- "$old_bin"
  [[ -z "$tmp_config" ]] || rm -f -- "$tmp_config"
}
trap finish EXIT
install -m 0755 "$ROOT/omarchy-desktop-recorder" "$stage_bin"
mv -f -- "$stage_bin" "$BIN_PATH"
new_hash="$(sha256sum "$BIN_PATH" | cut -d' ' -f1)"
printf '%s\n' "$new_hash" > "${MANIFEST}.tmp"
chmod 0600 "${MANIFEST}.tmp"
mv -f -- "${MANIFEST}.tmp" "$MANIFEST"

if [[ "$already_installed" == 0 ]]; then
  backup="${CONFIG}.bak.$(date +%s)"
  cp -p -- "$CONFIG" "$backup"
  tmp_config="$(mktemp "${CONFIG}.tmp.XXXXXX")"
  cp --preserve=mode,ownership -- "$CONFIG" "$tmp_config"
  printf '\n%s\n' "$BEGIN" >> "$tmp_config"
  cat "$ROOT/integration/hypr/bindings.lua" >> "$tmp_config"
  printf '%s\n' "$END" >> "$tmp_config"
  mv -f -- "$tmp_config" "$CONFIG"
  tmp_config=""
  changed_config=1
fi

rollback() {
  set +e
  if [[ "$changed_config" == 1 ]]; then
    tmp_config="$(mktemp "${CONFIG}.rollback.XXXXXX")"
    rollback_block "$CONFIG" "$tmp_config"
    chmod --reference="$CONFIG" "$tmp_config"
    mv -f -- "$tmp_config" "$CONFIG"
    tmp_config=""
    hyprctl reload >/dev/null 2>&1
  fi
  if [[ "$had_old" == 1 ]]; then
    cp -p -- "$old_bin" "$BIN_PATH"
    printf '%s\n' "$old_manifest" > "$MANIFEST"
  else
    rm -f -- "$BIN_PATH" "$MANIFEST"
  fi
  set -e
}

if [[ "$changed_config" == 1 ]] && ! hyprctl reload >/dev/null; then
  rollback
  die 'Hyprland reload failed; the owned binding was rolled back.'
fi
if ! after_errors="$(hyprctl configerrors)" || [[ -n "${after_errors//[[:space:]]/}" ]]; then
  rollback
  die 'Hyprland config validation failed; the owned binding was rolled back.'
fi
owned_count="$(hyprctl -j binds | jq '[.[] | select(.key == "F10" and .keycode == 0 and .modmask == 0 and .submap == "" and (.description == "Start desktop audio recording" or .description == "Stop desktop audio recording"))] | length')"
[[ "$owned_count" == 2 ]] || {
  rollback
  die 'Expected exactly two unmodified F10 press/release bindings; installation was rolled back.'
}

printf 'Installed %s\n' "$BIN_PATH"
if [[ "$already_installed" == 1 ]]; then
  printf 'Verified the owned F10 press/release bindings in %s\n' "$CONFIG"
else
  printf 'Added the owned F10 press/release bindings to %s\n' "$CONFIG"
fi
[[ -z "$backup" ]] || printf 'Config backup: %s\n' "$backup"
