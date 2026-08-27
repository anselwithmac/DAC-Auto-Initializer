#!/bin/bash
#
# dac-reinit installer.
#
#   ./install.sh                          # list USB audio devices and pick one
#   ./install.sh --match "FiiO K5 Pro" -y # non-interactive
#   ./install.sh --match "..." --settle-ms 400
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SBIN=/usr/local/sbin
DAEMON_PLIST=/Library/LaunchDaemons/local.dacreinit.plist
LABEL=local.dacreinit

MATCH=""; SETTLE=400; COOLDOWN=3000; ASSUME_YES=0

while [ $# -gt 0 ]; do
  case "$1" in
    --match)       MATCH="$2"; shift 2 ;;
    --settle-ms)   SETTLE="$2"; shift 2 ;;
    --cooldown-ms) COOLDOWN="$2"; shift 2 ;;
    -y|--yes)      ASSUME_YES=1; shift ;;
    -h|--help)     sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m warning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31m error:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- toolchain --

command -v clang >/dev/null 2>&1 \
  || die "clang not found. Install the Xcode command line tools:  xcode-select --install"

# ------------------------------------------------------------- find the DAC --

# USB devices that expose an audio-class interface (bInterfaceClass == 1).
list_usb_audio() {
  ioreg -w0 -l -r -c IOUSBHostDevice 2>/dev/null \
    | awk '/"USB Product Name"/{n=$0} /"bInterfaceClass" = 1$/{if(n!=""){print n; n=""}}' \
    | sed -E 's/.*"USB Product Name" = "(.*)".*/\1/' \
    | sort -u
}

if [ -z "$MATCH" ]; then
  say "Looking for USB audio devices..."
  # stock macOS ships bash 3.2, which has no mapfile
  FOUND=(); while IFS= read -r l; do [ -n "$l" ] && FOUND+=("$l"); done < <(list_usb_audio)
  N="${#FOUND[@]}"

  if [ "$N" -eq 0 ]; then
    die "No USB audio devices found. Connect the DAC and re-run, or pass --match \"Name\"."
  fi

  # -y must never block waiting for input, so it can only auto-select when
  # there is nothing to choose between.
  if [ "$ASSUME_YES" -eq 1 ]; then
    [ "$N" -eq 1 ] || die "$N USB audio devices found. With -y you must pass --match \"Name\"."
    MATCH="${FOUND[0]}"
    say "Found: $MATCH"
  else
    echo
    echo "USB audio devices connected:"
    i=1; for f in "${FOUND[@]}"; do printf '  %d) %s\n' "$i" "$f"; i=$((i+1)); done
    echo

    # Re-prompt rather than abort: a typo here otherwise means running the
    # whole script again.
    MATCH=""
    for _ in 1 2 3; do
      printf 'Which device should the daemon watch? [1-%d] ' "$N"
      read -r choice || die "no input"
      if [[ "$choice" =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le "$N" ]; then
        MATCH="${FOUND[$((choice-1))]}"
        break
      fi
      warn "Enter a number between 1 and $N."
    done
    [ -n "$MATCH" ] || die "no valid selection"
    say "Selected: $MATCH"
  fi
fi

# Trailing spaces are common in USB descriptors and would break exact matching
# later; the tools all substring-match, so trim for tidiness.
MATCH="$(printf '%s' "$MATCH" | sed -E 's/[[:space:]]+$//')"

# ------------------------------------------------------------------- build ---

say "Building..."
BUILD="$HERE/build"; mkdir -p "$BUILD"
clang -O2 -Wall -o "$BUILD/dac-reinitd"     "$HERE/src/dac-reinitd.c"     -framework CoreFoundation -framework IOKit
clang -O2 -Wall -o "$BUILD/usb-reenumerate" "$HERE/src/usb-reenumerate.c" -framework CoreFoundation -framework IOKit
say "Built 2 binaries into build/"

# --------------------------------------------------------------- generate ----

xml_escape() { printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }

sed -e "s|@MATCH@|$(xml_escape "$MATCH")|g" \
    -e "s|@SETTLE@|$SETTLE|g" \
    -e "s|@COOLDOWN@|$COOLDOWN|g" \
    "$HERE/templates/local.dacreinit.plist.in" > "$BUILD/local.dacreinit.plist"

plutil -lint "$BUILD/local.dacreinit.plist" >/dev/null || die "generated plist is invalid"

echo
say "About to install:"
echo "    device      : $MATCH"
echo "    settle      : ${SETTLE}ms"
echo "    cooldown    : ${COOLDOWN}ms"
echo "    binaries    -> $SBIN/{dac-reinitd,usb-reenumerate}"
echo "    daemon      -> $DAEMON_PLIST  (runs as root, starts at boot)"
echo
if [ "$ASSUME_YES" -ne 1 ]; then
  printf 'Proceed? [y/N] '; read -r ok
  case "$ok" in y|Y|yes|YES) ;; *) echo "aborted"; exit 1 ;; esac
fi

# ---------------------------------------------------------------- install ----

say "Installing (sudo required: the daemon seizes a USB device, which needs root)"
sudo mkdir -p "$SBIN"
sudo launchctl bootout "system/$LABEL" 2>/dev/null || true

sudo install -o root -g wheel -m 755 "$BUILD/dac-reinitd"       "$SBIN/dac-reinitd"
sudo install -o root -g wheel -m 755 "$BUILD/usb-reenumerate"   "$SBIN/usb-reenumerate"
sudo install -o root -g wheel -m 644 "$BUILD/local.dacreinit.plist" "$DAEMON_PLIST"

sudo launchctl bootstrap system "$DAEMON_PLIST"
sleep 1

if sudo launchctl print "system/$LABEL" >/dev/null 2>&1; then
  say "Installed and running. It will start automatically on every boot."
else
  die "daemon failed to start; check /var/log/dac-reinit.log"
fi

echo
echo "Watch it work as you flip the switch:"
echo "    sudo tail -f /var/log/dac-reinit.log"
echo
echo "Uninstall:"
echo "    ./uninstall.sh"
