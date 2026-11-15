#!/bin/bash
# Checks the launchers' Vulkan layer retry (RingOut#16): a run that fails on the
# swap chain is retried once without Mesa's device-select layer, and nothing
# else is. Both launchers carry the same block; this checks that too.
#
#   bash dist/RingOut-1.0-dist/tests/test_layer_retry.sh
set -u
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
block() { sed -n '/^# >>> layer-retry/,/^# <<< layer-retry/p' "$1"; }
fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: want '$2', got '$3'"; fail=1; fi
}
check "both launchers carry the same block" "" \
    "$(diff <(block "$HERE/RingOut-1.0-dist/RingOut") <(block "$HERE/RingOut-1.0-deck/RingOut"))"
eval "$(block "$HERE/RingOut-1.0-dist/RingOut")"

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
# A stand-in game: fails like the #16 report unless the layer is disabled.
cat > "$T/game" <<'G'
#!/bin/bash
echo "attempt VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE:-}" >> "$RUNS"
case "${MODE}" in
  swapchain) [[ ",${VK_LOADER_LAYERS_DISABLE:-}," == *",VK_LAYER_MESA_device_select,"* ]] && exit 0
             echo "[alert] Warning: Failed to create Vulkan swap chain." >&2; exit 1 ;;
  stubborn)  echo "[alert] Warning: Failed to create Vulkan swap chain." >&2; exit 1 ;;
  other)     echo "some other failure" >&2; exit 1 ;;
  ok)        exit 0 ;;
  hang)      trap 'echo stopped >> "$RUNS"; exit 143' TERM; while :; do sleep 0.1; done ;;
esac
G
chmod +x "$T/game"
run() { # mode [initial VK_LOADER_LAYERS_DISABLE] -> "rc runs"
    : > "$T/runs"
    ( export RUNS="$T/runs" MODE="$1"; unset VK_LOADER_LAYERS_DISABLE
      [ -n "${2:-}" ] && export VK_LOADER_LAYERS_DISABLE="$2"
      run_with_layer_retry "$T/game" >/dev/null 2>&1; echo "$? $(wc -l < "$T/runs")" )
}
check "normal launch: one run, no retry"        "0 1" "$(run ok)"
check "swap-chain failure: retried and works"   "0 2" "$(run swapchain)"
check "retry disables the layer"                "attempt VK_LOADER_LAYERS_DISABLE=VK_LAYER_MESA_device_select" "$(tail -1 "$T/runs")"
check "still failing: one retry, then exit 1"   "1 2" "$(run stubborn)"
check "other failure: no retry"                 "1 1" "$(run other)"
check "layer already off: no retry"             "1 1" "$(run stubborn VK_LAYER_MESA_device_select)"
check "user's own list kept on retry"           "0 2" "$(run swapchain VK_LAYER_foo)"
check "...and extended"                         "attempt VK_LOADER_LAYERS_DISABLE=VK_LAYER_foo,VK_LAYER_MESA_device_select" "$(tail -1 "$T/runs")"
# A stop aimed at the launcher must reach the game, as it did when the launcher
# exec'd it.
: > "$T/runs"
( export RUNS="$T/runs" MODE=hang; run_with_layer_retry "$T/game" >/dev/null 2>&1; echo "rc $?" >> "$T/runs" ) &
sleep 1; kill -TERM $!; sleep 1.5
check "TERM to the launcher stops the game"     "stopped" "$(grep -c stopped "$T/runs" | sed 's/^1$/stopped/')"
check "...and the launcher exits with its code" "rc 143" "$(grep '^rc' "$T/runs")"
pkill -f "$T/game" 2>/dev/null
exit $fail
