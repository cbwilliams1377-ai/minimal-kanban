#!/usr/bin/env bash
# Cross-compile MinimalKanban for Windows from Linux/macOS (mingw-w64).
# Equivalent of build.bat. Produces MinimalKanban.exe.
#
# Dependencies: ffmpeg, mingw-w64 cross toolchain
#   Debian/Ubuntu:  sudo apt install ffmpeg g++-mingw-w64-x86-64 mingw-w64-tools
#   Arch:           sudo pacman -S ffmpeg mingw-w64-gcc
#   macos (brew):   brew install ffmpeg mingw-w64
set -e

cd "$(dirname "$0")"

CC="${CC:-x86_64-w64-mingw32-g++}"
RC="${RC:-x86_64-w64-mingw32-windres}"

for tool in ffmpeg "$CC" "$RC"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "MISSING: $tool — cannot build. Install ffmpeg + mingw-w64 first." >&2
        exit 1
    fi
done

ffmpeg -y -hide_banner -loglevel error -i check-square.png -vf scale=256:256 check-square.ico
"$RC" minimal_kanban.rc minimal_kanban-res.o
"$CC" minimal_kanban.cpp minimal_kanban-res.o -o MinimalKanban.exe -std=c++17 -O2 -s -mwindows -static -static-libgcc -static-libstdc++ -municode -lurlmon -lole32 -lshell32 -lgdi32 -luser32 -luuid
echo "Built cross-compiled MinimalKanban.exe (Windows binary)."