#!/usr/bin/env bash

zig c++ main.cpp base.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti -O2 -target aarch64-linux-gnu -o backuper_linux
clang++ main.cpp base.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti -O2 -flto -o backuper
