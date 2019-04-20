#!/usr/bin/env bash

set +e

./compile_cl_debug.bat
./build/main_debug.exe 100 200 -1.398995 0.001901 0.000035 400000 4 0
./compile_cl.bat
./build/main.exe 100 200 -1.398995 0.001901 0.000035 400000 4 0
