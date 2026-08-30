#!/usr/bin/env sh
set -eu

: "${VITASDK:?Set VITASDK to your VitaSDK installation directory}"
export PATH="$VITASDK/bin:$PATH"

cmake -S . -B build-vita -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita --parallel

printf '%s\n' "Built build-vita/ps2vita.vpk"

