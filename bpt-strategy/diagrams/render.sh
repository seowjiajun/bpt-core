#!/usr/bin/env bash
# Render every .d2 in this directory to a sibling .svg, then strip
# D2's default white page-background rect so the SVG inherits the
# parent page's background (matters on GitHub light/dark mode).
#
# Run from the diagrams/ directory:
#   ./render.sh           # render everything
#   ./render.sh foo       # render only foo.d2 → foo.svg

set -euo pipefail
cd "$(dirname "$0")"

# IBM Plex Sans (apt: fonts-ibm-plex) — D2 embeds these into each SVG so
# every viewer renders the same font regardless of whether they have it
# installed locally. Falls back to D2's default Source Sans Pro if Plex
# isn't installed.
PLEX_DIR=/usr/share/fonts/truetype/ibm-plex
FONT_REGULAR="${PLEX_DIR}/IBMPlexSans-Regular.ttf"
FONT_BOLD="${PLEX_DIR}/IBMPlexSans-Bold.ttf"
FONT_ITALIC="${PLEX_DIR}/IBMPlexSans-Italic.ttf"
FONT_SEMIBOLD="${PLEX_DIR}/IBMPlexSans-SemiBold.ttf"

FONT_ARGS=()
if [[ -f "$FONT_REGULAR" ]]; then FONT_ARGS+=(--font-regular "$FONT_REGULAR"); fi
if [[ -f "$FONT_BOLD" ]];    then FONT_ARGS+=(--font-bold    "$FONT_BOLD");    fi
if [[ -f "$FONT_ITALIC" ]];  then FONT_ARGS+=(--font-italic  "$FONT_ITALIC");  fi
if [[ -f "$FONT_SEMIBOLD" ]];then FONT_ARGS+=(--font-semibold "$FONT_SEMIBOLD"); fi

render_one() {
    local stem="$1"
    d2 --layout=elk "${FONT_ARGS[@]}" "${stem}.d2" "${stem}.svg"
    # D2 v0.7.x paints a #FFFFFF page-background rect with class fill-N7.
    # Setting fill="none" doesn't win — the CSS rule .fill-N7{fill:#FFFFFF}
    # in the embedded <style> overrides the presentation attribute (CSS
    # beats attributes in SVG). Just delete the rect entirely. Nothing
    # else uses the class.
    sed -i 's|<rect[^>]* class=" fill-N7"[^>]*/>||g' "${stem}.svg"
    echo "rendered ${stem}.svg (background stripped, IBM Plex embedded if installed)"
}

if [[ $# -gt 0 ]]; then
    for arg in "$@"; do
        render_one "${arg%.d2}"
    done
else
    for src in *.d2; do
        render_one "${src%.d2}"
    done
fi
