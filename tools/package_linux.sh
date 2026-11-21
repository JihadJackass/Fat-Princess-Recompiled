#!/bin/sh
# ===========================================================================
#  Fat Princess Recomp - make a Linux / Steam Deck package from a build:
#  dist/FatPrincess-Linux/ and dist/FatPrincess-Linux.tar.gz, holding the
#  game binary, fatprincess.sh, the icon font and a short README. The game
#  files (vfs/) are not included: copy your own next to fatprincess.sh.
# ===========================================================================
set -e
cd "$(dirname "$0")/.."
[ -x FatPrincessRecomp ] || { echo "Build first: ./build.sh"; exit 1; }
OUT=dist/FatPrincess-Linux
rm -rf "$OUT" && mkdir -p "$OUT/fonts"
cp FatPrincessRecomp "$OUT/"
strip "$OUT/FatPrincessRecomp" 2>/dev/null || true
cp fatprincess.sh "$OUT/" && chmod +x "$OUT/fatprincess.sh" "$OUT/FatPrincessRecomp"
cp fonts/DejaVuSans-Bold.ttf fonts/LICENSE-DejaVu.txt "$OUT/fonts/"
cp docs/LINUX.md "$OUT/README.md"
need=$(objdump -T "$OUT/FatPrincessRecomp" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
echo "Needs $need or newer (SteamOS 3.5+ has it)."
tar -C dist -czf dist/FatPrincess-Linux.tar.gz FatPrincess-Linux
ls -la dist/FatPrincess-Linux.tar.gz
