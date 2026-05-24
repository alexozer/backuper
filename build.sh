#!/usr/bin/env bash

zig c++ main.cpp base.cpp platform_posix.cpp platform_macos.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti -O2 -s -target aarch64-macos-none -o backuper_macos
zig c++ main.cpp base.cpp platform_posix.cpp platform_linux.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti -O2 -s -target x86_64-linux-gnu -o backuper_linux
zig c++ main.cpp base.cpp platform_windows.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti -O2 -s -target x86_64-windows -o backuper_windows.exe
