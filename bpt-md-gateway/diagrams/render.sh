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

render_one() {
    local stem="$1"
    d2 --layout=elk "${stem}.d2" "${stem}.svg"
    # D2 v0.7.x paints a #FFFFFF page-background rect with class fill-N7.
    # Setting fill="none" doesn't win — the CSS rule .fill-N7{fill:#FFFFFF}
    # in the embedded <style> overrides the presentation attribute (CSS
    # beats attributes in SVG). Just delete the rect entirely. Nothing
    # else uses the class.
    sed -i 's|<rect[^>]* class=" fill-N7"[^>]*/>||g' "${stem}.svg"
    echo "rendered ${stem}.svg (background stripped)"
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
