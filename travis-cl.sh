#!/usr/bin/env bash

set +e

./compile_cl_debug.bat
./build/main_debug.exe 400 300 -1.398995 0.001901 0.000035 200000 4 0
./compile_cl.bat
./build/main.exe 400 300 -1.398995 0.001901 0.000035 200000 4 0
