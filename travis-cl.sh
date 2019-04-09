#!/usr/bin/env bash

set +e

./compile_cl_debug.bat
./build/main_debug.exe
./compile_cl.bat
./build/main.exe
