#!/usr/bin/env bash
# Records the three demo GIFs of a platform in its emulator, with the phone simulated by
# scripts/emulator-feed.sh. Needs the pebble tool and ImageMagick.
#
#   scripts/record-gifs.sh <flint|emery|chalk>
#
# Writes docs/<platform>-*.gif at 2x for the README and docs/store/<platform>-*.gif at the watch's
# own resolution. On the round Time Round the corners outside the screen are filled dark.
# The emulator is stopped when the script ends.
set -euo pipefail
cd "$(dirname "$0")/.."
P=${1:?platform: flint|emery|chalk}
work=$(mktemp -d)
# Always shut the emulator (qemu and its phone simulator) down again, also after an error or Ctrl-C.
# Note that `pebble kill` stops every running emulator, not only the one used here.
cleanup() { rm -rf "$work"; pebble kill >/dev/null 2>&1 || true; }
trap cleanup EXIT
trap 'exit 130' INT TERM
feed() { scripts/emulator-feed.sh "$P" "$@"; }
press() { pebble emu-button --emulator "$P" "$@" >/dev/null 2>&1; }

frames=()   # "delay file" pairs of the GIF being recorded
reset() { frames=(); }
shot() {    # shot <delay in 1/100 s>: wait for the watch to redraw, then capture
  local file="$work/$(printf %03d ${#frames[@]}).png"
  sleep 0.6; pebble screenshot --emulator "$P" "$file" >/dev/null 2>&1
  frames+=("$1 $file")
}
save() {    # save <name>
  local args=() pair
  for pair in "${frames[@]}"; do args+=(-delay "${pair%% *}" "${pair#* }"); done
  local flat=(-background '#333333' -alpha remove -alpha off)
  convert "${args[@]}" "${flat[@]}" -loop 0 -layers Optimize "docs/store/$P-$1.gif"
  convert "${args[@]}" "${flat[@]}" -filter point -resize 200% -loop 0 -layers Optimize "docs/$P-$1.gif"
  echo "docs/$P-$1.gif ($(identify docs/store/$P-$1.gif | wc -l) frames)"
}

# The watch asks the (simulated, silent) phone for a refresh at every full minute and then shows
# "Updating" until the next board arrives, so start each recording well after the minute tick.
quiet() { local s; while s=$((10#$(date +%S))); (( s < 3 || s > 30 )); do sleep 1; done; }

pebble build >/dev/null
# Start from a clean emulator: it remembers recently chosen stations, which would add rows to the picker.
# (`pebble wipe` clears the stored data of all emulators.)
pebble kill >/dev/null 2>&1 || true; pebble wipe >/dev/null 2>&1 || true
pebble install --emulator "$P" >/dev/null 2>&1; sleep 4

# 1. A train on time, then delayed by 4 minutes, then canceled.
reset; quiet; TOKEN=1 feed board; shot 110; shot 80; shot 80
TOKEN=2 feed delayed; shot 130; shot 80; shot 80
TOKEN=3 feed canceled; shot 80; shot 80; shot 300
save delayed-canceled

# 2. Choosing Brugge as destination.
reset; quiet; TOKEN=4 feed board; shot 130
press --duration 800 click down; shot 80                    # hold Down: the picker for the destination
for i in 1 2 3; do press click down; shot 55; done          # recent -> 's -> A -> B
press click select; shot 55                                 # B...: Ba, Be, Bi, Bl, Bo, Br
for i in 1 2 3 4 5; do press click down; done; shot 55
press click select; shot 55                                 # Br...: Bra, Bre, Bru
for i in 1 2; do press click down; done; shot 55
press click select; shot 55                                 # Bru...: Brugelette, Brugge, ...
press click down; shot 100
press click select; sleep 1                                 # the route is set; the phone answers
TOKEN=5 TO=Brugge feed board; shot 120; shot 300
save choose-station

# 3. A trip with a change.
reset; quiet; TOKEN=6 TO=Leuven feed transfer; shot 140; shot 80
press click select; sleep 0.5
TOKEN=6 TO=Leuven feed details; shot 100
for i in 1 2 3 4 5 6; do press click down; shot $([[ $i == 6 ]] && echo 300 || echo 70); done
save transfer
