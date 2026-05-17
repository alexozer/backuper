#!/usr/bin/env bash

clang++ main.cpp base.cpp -std=c++20 -nostdinc++ -fno-exceptions -fno-rtti -O2 -flto -o backuper
