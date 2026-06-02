#!/bin/bash
# dev_no_sleep.sh — keep a dev laptop / WSL host awake during baselining so it
# doesn't suspend and trip every C++ service's "timeout between service calls"
# (~30s recovery cascade). See site/docs/ops/host-sleep-recovery.md.
#
#   scripts/dev_no_sleep.sh                 # hold a sleep+idle lock until Ctrl-C
#   scripts/dev_no_sleep.sh ./run_stack.sh  # hold only for the wrapped command
set -euo pipefail

why="${BPT_INHIBIT_WHY:-bpt baselining — keep host awake}"

if ! command -v systemd-inhibit >/dev/null; then
    echo "dev_no_sleep.sh: systemd-inhibit not found (non-systemd host?)." >&2
    echo "On WSL2 the suspend is driven by the Windows power plan — inhibit there instead." >&2
    exit 1
fi

if [ "$#" -gt 0 ]; then
    exec systemd-inhibit --what=sleep:idle --who=bpt --why="$why" --mode=block "$@"
fi

echo "Holding sleep/idle inhibitor ($why). Ctrl-C to release."
exec systemd-inhibit --what=sleep:idle --who=bpt --why="$why" --mode=block sleep infinity
