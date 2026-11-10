#!/bin/sh
# FreezeMark build (Linux cross-compile, same toolchain as UseStats)
set -e
cd "$(dirname "$0")"
python3 make_icon.py
x86_64-w64-mingw32-windres resource.rc -O coff -o resource.o
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -static -mwindows -Wall \
  -o FreezeMark.exe main.cpp resource.o \
  -lgdiplus -lgdi32 -luser32 -lshell32 -lkernel32 -lole32 -ladvapi32 -lwinmm
ls -l FreezeMark.exe app.ico
