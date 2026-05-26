#!/usr/bin/env bash
set -eou pipefail

cd "$( dirname -- "${BASH_SOURCE[0]}" )"

mkdir -p out
rm -f out/*.pdb

if [[ "${1:-}" == "debug" ]]; then
    PROFILE_ARGS="-O0"
elif [[ "${1:-}" == "release" || "${1:-}" == "" ]]; then
    PROFILE_ARGS="-O2 -s"
else
    echo "Invalid profile: $1"
    exit 1
fi

zig c++ src/main.cpp src/base.cpp src/platform_posix.cpp src/platform_macos.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti $PROFILE_ARGS -target aarch64-macos-none -o out/backuper_macos
zig c++ src/main.cpp src/base.cpp src/platform_posix.cpp src/platform_linux.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti $PROFILE_ARGS -target x86_64-linux-gnu -o out/backuper_linux
zig c++ src/main.cpp src/base.cpp src/platform_windows.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti $PROFILE_ARGS -target x86_64-windows -o out/backuper_windows.exe
