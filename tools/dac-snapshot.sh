#!/bin/bash
# Capture the DAC's USB + Core Audio state so a hung state can be diffed
# against a working one.
#   ./dac-snapshot.sh broken
#   ./dac-snapshot.sh working
# then:  diff ~/Desktop/dac-broken.txt ~/Desktop/dac-working.txt

LABEL="${1:-snapshot}"
OUT="$HOME/Desktop/dac-$LABEL.txt"

{
  echo "===== $LABEL @ $(date) ====="
  echo
  echo "----- USB tree -----"
  system_profiler SPUSBDataType 2>/dev/null
  echo
  echo "----- USB audio interfaces (ioreg) -----"
  ioreg -w0 -l -r -c IOUSBHostInterface 2>/dev/null \
    | grep -E '"(USB Product Name|bInterfaceClass|bInterfaceSubClass|bAlternateSetting|IOClass|idVendor|idProduct)"'
  echo
  echo "----- Core Audio devices -----"
  system_profiler SPAudioDataType 2>/dev/null
  echo
  echo "----- coreaudiod -----"
  ps -Ao pid,etime,comm | grep -i coreaudio | grep -v grep
} > "$OUT" 2>&1

echo "wrote $OUT"
