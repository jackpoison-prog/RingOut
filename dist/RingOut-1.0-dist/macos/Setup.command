#!/bin/bash
# Double-click setup for macOS: asks for the disc image, then runs the same
# setup.sh the Linux package uses (it has a macOS branch). Opens in Terminal.
cd "$(dirname "$0")" || exit 1
if [ $# -eq 0 ]; then
    disc="$(osascript -e 'POSIX path of (choose file with prompt "Choose your SoulCalibur II (USA) disc image")' 2>/dev/null)" || {
        echo "No disc image chosen -- nothing was changed."; exit 1; }
    set -- "$disc"
fi
/bin/bash ./setup.sh "$@"
status=$?
echo
if [ $status -eq 0 ]; then
    echo "Done. Start the game with RingOut.app in this folder."
fi
read -r -n 1 -s -p "Press any key to close this window."
echo
exit $status
