#!/bin/bash
#
# Removes everything install.sh put on the system.
#   ./uninstall.sh            # leaves /var/log/dac-reinit.log in place
#   ./uninstall.sh --purge    # removes the log too
#
set -euo pipefail

SBIN=/usr/local/sbin
DAEMON_PLIST=/Library/LaunchDaemons/local.dacreinit.plist
LABEL=local.dacreinit
PURGE=0
[ "${1:-}" = "--purge" ] && PURGE=1

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }

say "Stopping the daemon"
sudo launchctl bootout "system/$LABEL" 2>/dev/null || true

say "Removing files"
sudo rm -f "$DAEMON_PLIST" \
           "$SBIN/dac-reinitd" \
           "$SBIN/usb-reenumerate" \
           "$SBIN/audio-bounce" \
           "$SBIN/dac-reinit-hook.sh" \
           "$SBIN/dac-reinit.sh"

if [ "$PURGE" -eq 1 ]; then
  say "Removing the log"
  sudo rm -f /var/log/dac-reinit.log
else
  echo "    (left /var/log/dac-reinit.log -- pass --purge to remove it)"
fi

say "Uninstalled. Your audio setup is untouched."
