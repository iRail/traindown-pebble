#!/usr/bin/env bash
# Plays the phone for an emulator: feeds the watch the same AppMessages the phone companion sends,
# so screenshots and GIFs can be recorded without a phone (the train data is sample data).
#
#   scripts/emulator-feed.sh <platform> <scenario>
#
# Scenarios: board     two journeys, the first on time
#            delayed   the same board with the first train 4 minutes late
#            canceled  the same board with the first train canceled
#            transfer  one journey with a change at Brussel-Noord, 2 minutes to change
#            details   the trip overview of that journey (send it after pressing Select)
# Environment: TOKEN (board id, must match between board and details), TO (destination, default Brussel-Zuid).
set -euo pipefail
cd "$(dirname "$0")/.."
platform=${1:?platform: flint|emery|chalk}; scenario=${2:?scenario}
declare -A K
for name in Kind Token Count Index From To Departure Arrival Delay Platform Train Transfers Canceled Updated Via Leg ToPlatform Stops; do
  K[$name]=$(python3 -c "import json; print(json.load(open('build/appinfo.json'))['appKeys']['$name'])")
done
send() { # name=value pairs; numbers are sent as ints, the rest as strings
  local ints=() strings=() pair name value
  for pair in "$@"; do
    name=${pair%%=*}; value=${pair#*=}
    if [[ $value =~ ^-?[0-9]+$ && $name != Platform && $name != Train && $name != ToPlatform ]]; then ints+=("${K[$name]}=$value"); else strings+=("${K[$name]}=$value"); fi
  done
  pebble send-app-message --emulator "$platform" ${ints:+--int "${ints[@]}"} ${strings:+--string "${strings[@]}"} >/dev/null
}
now=$(date +%s); token=${TOKEN:-$RANDOM}; to=${TO:-Brussel-Zuid}
delay=0; canceled=0
[[ $scenario == delayed ]] && delay=240
[[ $scenario == canceled ]] && canceled=1
# The journey of the transfer scenario: leave in 12 minutes (plus 2 late), 40 minutes to Brussel-Noord,
# change with 2 minutes to spare, 20 minutes on to the destination.
leave=$((now + 754 + 120)); change=$((leave + 2400)); onward=$((change + 120)); end=$((onward + 1200))

case $scenario in
  board|delayed|canceled)
    send Kind=1 Token=$token Count=2 Updated=$now From=Gent-Sint-Pieters To="$to"
    send Kind=2 Token=$token Index=0 Departure=$((now + 754 + delay)) Arrival=$((now + 2700 + delay)) Delay=$delay Transfers=0 Canceled=$canceled Platform=4 Train="IC 2134" Via=
    send Kind=2 Token=$token Index=1 Departure=$((now + 1954)) Arrival=$((now + 3900)) Delay=0 Transfers=1 Canceled=0 Platform=7 Train="IC 1535" Via=Brussel-Noord
    send Kind=3 Token=$token ;;
  transfer)
    send Kind=1 Token=$token Count=1 Updated=$now From=Gent-Sint-Pieters To="$to"
    send Kind=2 Token=$token Index=0 Departure=$leave Arrival=$end Delay=120 Transfers=1 Canceled=0 Platform=7 Train="IC 1535" Via=Brussel-Noord
    send Kind=3 Token=$token ;;
  details)
    send Kind=4 Token=$token Index=0 Count=2
    send Kind=5 Token=$token Index=0 Leg=0 From=Gent-Sint-Pieters To=Brussel-Noord Train="IC 1535" Platform=7 ToPlatform=3 Departure=$leave Arrival=$change Delay=120
    send Kind=6 Token=$token Index=0 Leg=0 Stops="$((leave + 900)) Lokeren"$'\n'"$((leave + 1500)) Dendermonde"$'\n'"$((leave + 2100)) Mechelen"
    send Kind=5 Token=$token Index=0 Leg=1 From=Brussel-Noord To="$to" Train="IC 2208" Platform=2 ToPlatform=5 Departure=$onward Arrival=$end Delay=0
    send Kind=6 Token=$token Index=0 Leg=1 Stops="$((onward + 420)) Schaarbeek"$'\n'"$((onward + 900)) Brussel-Luxemburg"
    send Kind=7 Token=$token Index=0 ;;
  *) echo "unknown scenario $scenario" >&2; exit 1 ;;
esac
