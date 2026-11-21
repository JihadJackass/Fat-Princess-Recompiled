#!/bin/sh
# ===========================================================================
#  Fat Princess Recomp - start the game on Linux / Steam Deck. Log: run.log
#  Settings live in fatprincess.ini (made with the defaults on the first run;
#  the one from a Windows install works here unchanged).
#  Steam Deck: add this script to Steam as a non-Steam game.
# ===========================================================================
cd "$(dirname "$(readlink -f "$0")")" || exit 1

fail() {
  echo "$1"
  command -v zenity >/dev/null 2>&1 && zenity --error --width=420 --text="$1" 2>/dev/null
  exit 1
}

[ -f vfs/dev_hdd0/game/NPUA80139/USRDIR/GAME.elf ] ||
  fail "Game files missing: copy the vfs folder from your Fat Princess Recomp install next to fatprincess.sh."
[ -f vfs/dev_hdd0/game/NPUA80139/USRDIR/siege.qrk ] ||
  fail "siege.qrk missing in vfs/dev_hdd0/game/NPUA80139/USRDIR/."
[ -x ./FatPrincessRecomp ] || fail "FatPrincessRecomp missing: build with ./build.sh, or unpack the Linux package here."

echo "Starting Fat Princess... (log: run.log)"
exec ./FatPrincessRecomp > run.log 2>&1
