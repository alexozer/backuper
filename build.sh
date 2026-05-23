#!/usr/bin/env bash

zig c++ main.cpp base.cpp platform_posix.cpp platform_linux.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti -O2 -target aarch64-linux-gnu -o backuper_linux
clang++ main.cpp base.cpp platform_posix.cpp platform_macos.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti -O2 -flto -o backuper_macos
