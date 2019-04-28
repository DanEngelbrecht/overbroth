#!/usr/bin/env bash

set +e

sh ./compile_clang_debug.sh
./build/main_debug 400 300 -1.398995 0.001901 0.000035 200000 4 0
sh ./compile_clang.sh
./build/main 400 300 -1.398995 0.001901 0.000035 200000 4 0
