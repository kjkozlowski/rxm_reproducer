#!/bin/bash

git submodule update --init --recursive

num_cpus=$(nproc)

export CC=clang
export CXX=clang++
SANITIZER_PARAM=kcfi

cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ${SANITIZER_PARAM:+-DSANITIZE="$SANITIZER_PARAM"} -S. -B build
cmake  --build build -- -j "$num_cpus"
