#!/usr/bin/env bash
# Linux / HPC build. Requires: CMake >= 3.24, a C++20 compiler, Qt6 (Widgets, Charts), VTK >= 9.2 with GUISupportQt,
# Eigen3, nlohmann_json, doctest; optional SuiteSparse (UMFPACK) and Kokkos (OpenMP/CUDA/HIP).
#   ./scripts/build_linux.sh [build-dir] [-DABSS_BUILD_GUI=OFF ...]
# Example (Ubuntu 24.04): sudo apt install qt6-base-dev libqt6charts6-dev libvtk9-qt-dev libeigen3-dev \
#                          nlohmann-json3-dev doctest-dev libsuitesparse-dev ninja-build
# Kokkos: cmake -S kokkos -B kb -DKokkos_ENABLE_OPENMP=ON -DCMAKE_INSTALL_PREFIX=$HOME/kokkos && cmake --build kb --target install
#         then pass -DCMAKE_PREFIX_PATH=$HOME/kokkos
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-build}"
shift || true
cmake -S "$here" -B "$here/$build" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$here/$build" -j"$(nproc)"
ctest --test-dir "$here/$build" --output-on-failure -LE abaqus
