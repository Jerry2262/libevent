#!/bin/sh
set -e

rm -rf build
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DEVENT__DISABLE_OPENSSL=ON \
    -DEVENT__DISABLE_BENCHMARK=OFF
cmake --build build
