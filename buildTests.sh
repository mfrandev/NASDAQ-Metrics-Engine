#!/bin/bash
# Builds the metric tests into ./build (configures the directory if needed; never wipes it).
set -e
cd "$(dirname "$0")"

git submodule update --init --recursive

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target metrics_tests --parallel
