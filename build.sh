#!/bin/sh
# ===========================================================================
#  Fat Princess Recomp - Linux build (desktop Linux and Steam Deck)
#  Needs: clang, cmake, ninja, and the SDL2 development files
#    Debian/Ubuntu:  sudo apt install clang cmake ninja-build libsdl2-dev
#    Arch/SteamOS:   sudo pacman -S clang cmake ninja sdl2
#    Fedora:         sudo dnf install clang cmake ninja-build SDL2-devel
#  Output: ./FatPrincessRecomp (next to this script)     Log: build-linux.log
#  Optional: JOBS=n ./build.sh to limit parallel compiles
#            (each generated file can take 2-4 GB RAM while compiling).
#  Optional: SDL2_DIR=/path/to/sdl2/cmake for an SDL2 outside the system.
# ===========================================================================
set -e
cd "$(dirname "$0")"
LOG="$PWD/build-linux.log"
echo "[$(date)] build started" > "$LOG"

for t in clang clang++ cmake ninja; do
  command -v "$t" >/dev/null 2>&1 || { echo "ERROR: $t not found - see the top of build.sh"; exit 1; }
done

if [ -z "$JOBS" ]; then
  mem_gb=$(awk '/MemTotal/ {print int($2 / 1048576)}' /proc/meminfo)
  JOBS=$(( mem_gb / 3 )); [ "$JOBS" -lt 1 ] && JOBS=1
  cpus=$(nproc); [ "$JOBS" -gt "$cpus" ] && JOBS=$cpus
fi
echo "Parallel jobs: $JOBS   (override with: JOBS=n ./build.sh)"

echo "Configuring..."
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG" \
  ${SDL2_DIR:+-DSDL2_DIR="$SDL2_DIR"} >> "$LOG" 2>&1 || { echo "ERROR: configure failed - see build-linux.log"; exit 1; }

echo "Compiling about 3 million lines of recompiled code. The first build takes a while."
cmake --build build-linux -j "$JOBS" --target FatPrincessRecomp >> "$LOG" 2>&1 || { echo "ERROR: build failed - see build-linux.log"; exit 1; }
cp build-linux/FatPrincessRecomp ./FatPrincessRecomp
echo "[$(date)] build OK" >> "$LOG"
echo
echo "Build OK. Start the game with ./fatprincess.sh"
