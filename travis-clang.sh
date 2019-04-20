#!/usr/bin/env bash

set +e

sh ./compile_clang_debug.sh
./build/main_debug 100 200 -1.398995 0.001901 0.000035 400000 4 0
sh ./compile_clang.sh
./build/main 100 200 -1.398995 0.001901 0.000035 400000 4 0
