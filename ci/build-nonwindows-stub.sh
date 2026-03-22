#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build-unix-stub}"
generator="${CMAKE_GENERATOR:-Ninja}"

if [[ -n "${OBS_PREFIX:-}" ]]; then
  export CMAKE_PREFIX_PATH="${OBS_PREFIX}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

  for candidate in "${OBS_PREFIX}/lib/pkgconfig" "${OBS_PREFIX}/lib64/pkgconfig" "${OBS_PREFIX}/share/pkgconfig"; do
    if [[ -d "${candidate}" ]]; then
      export PKG_CONFIG_PATH="${candidate}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    fi
  done
fi

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libobs; then
  echo "Using libobs $(pkg-config --modversion libobs)"
else
  echo "libobs was not found via pkg-config." >&2
  echo "Set OBS_PREFIX, PKG_CONFIG_PATH, or libobs_DIR to an OBS/libobs development install before running this stub build." >&2
fi

cmake_args=(-S . -B "${build_dir}")
if [[ -n "${generator}" ]]; then
  cmake_args+=(-G "${generator}")
fi
if [[ -n "${libobs_DIR:-}" ]]; then
  cmake_args+=("-Dlibobs_DIR=${libobs_DIR}")
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}"
