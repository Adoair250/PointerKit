#!/bin/sh
# Linux / WSL の mingw-w64 でビルドする場合
set -e
OUT=${1:-build}
mkdir -p "$OUT"
x86_64-w64-mingw32-windres app.rc -O coff -o "$OUT/app.res"
x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static -Wall \
  main.cpp config.cpp hook.cpp gui.cpp touchpad.cpp keyboard.cpp "$OUT/app.res" -o "$OUT/PointerKit.exe" \
  -luser32 -lshell32 -ladvapi32 -lgdi32 -lcomctl32 -lole32 -loleaut32 -luuid -lhid -lwbemuuid -lcomdlg32
x86_64-w64-mingw32-strip "$OUT/PointerKit.exe"
