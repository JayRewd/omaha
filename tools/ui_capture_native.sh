#!/usr/bin/env bash
# Capture native modern-menu screenshots (one launch per shot).
# OpenMoHAA `wait N` is milliseconds.
# CL_TryStartIntro opens the fakk console when intros are skipped; ui_compare_goto closes it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Override with OPENMOHAA_DEST; never hardcode a personal absolute path.
DEST="${OPENMOHAA_DEST:-$HOME/Games/openmohaa}"
HOME_PATH="${OPENMOHAA_COMPARE_HOME:-/tmp/om-uir-home}"
OUT="$ROOT/artifacts/modern-menu-compare/native"
mkdir -p "$OUT"
cp -f "$ROOT/build-release/Release/openmohaa" "$DEST/openmohaa"
cp -rf "$ROOT/assets/main/ui/modern/." "$DEST/main/ui/modern/"
cp -rf "$ROOT/assets/main/fonts/." "$DEST/main/fonts/"

capture_one() {
  local goto_args="$1"
  local shot="$2"
  cat > "$DEST/main/uir_one.cfg" << EOF
set ui_legacy 0
set ui_skip_eamovie 1
set ui_skip_titlescreen 1
set ui_skip_legalscreen 1
set r_fullscreen 0
set r_mode -1
set r_customwidth 1280
set r_customheight 720
set developer 0
set name Soldier
set dm_playermodel allied_manon
set dm_playergermanmodel german_afrika_officer
ui_getplayermodel
wait 3500
ui_compare_goto ${goto_args}
wait 800
ui_compare_shot compare_${shot}
wait 2000
quit
EOF
  rm -rf "$HOME_PATH"
  mkdir -p "$HOME_PATH"
  DISPLAY="${DISPLAY:-:0}" timeout 120 "$DEST/openmohaa" \
    +set fs_basepath "$DEST" \
    +set fs_homepath "$HOME_PATH" \
    +set ui_legacy 0 \
    +set r_fullscreen 0 \
    +set r_mode -1 \
    +set r_customwidth 1280 \
    +set r_customheight 720 \
    +exec uir_one.cfg >/tmp/om_cap_${shot}.log 2>&1 || true
  local jpg="$HOME_PATH/main/screenshots/compare_${shot}.jpg"
  if [[ -f "$jpg" ]]; then
    python3 -c "from PIL import Image; Image.open('$jpg').save('$OUT/${shot}.png'); print('ok', '$shot', Image.open('$jpg').size)"
  else
    echo "MISSING $jpg"
    rg -n "Wrote screenshot|Unknown|Fatal|ZONE" /tmp/om_cap_${shot}.log | head -20 || true
    tail -15 /tmp/om_cap_${shot}.log
  fi
}

capture_one "play" "01_play_default"
capture_one "play" "02_play_row_selected"
capture_one "settings binds" "03_settings_binds"
capture_one "settings mouse" "04_settings_mouse"
capture_one "settings video" "05_settings_video"
