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
elif [[ -n "${MPT_MACOS_OBS_HEADERS_DIR:-}" ]] && [[ -n "${MPT_MACOS_OBS_LIBRARY:-}" ]]; then
  echo "Using macOS fallback OBS headers at ${MPT_MACOS_OBS_HEADERS_DIR}"
  echo "Using macOS fallback libobs at ${MPT_MACOS_OBS_LIBRARY}"
else
  echo "libobs was not found via pkg-config." >&2
  echo "Set OBS_PREFIX, PKG_CONFIG_PATH, or libobs_DIR to an OBS/libobs development install before running this stub build." >&2
  echo "macOS fallback builds can also set MPT_MACOS_OBS_HEADERS_DIR and MPT_MACOS_OBS_LIBRARY." >&2
fi

cmake_args=(-S . -B "${build_dir}")
if [[ -n "${generator}" ]]; then
  cmake_args+=(-G "${generator}")
fi
if [[ -n "${libobs_DIR:-}" ]]; then
  cmake_args+=("-Dlibobs_DIR=${libobs_DIR}")
fi
if [[ -n "${MPT_MACOS_OBS_HEADERS_DIR:-}" ]]; then
  cmake_args+=("-DMPT_MACOS_OBS_HEADERS_DIR=${MPT_MACOS_OBS_HEADERS_DIR}")
fi
if [[ -n "${MPT_MACOS_OBS_LIBRARY:-}" ]]; then
  cmake_args+=("-DMPT_MACOS_OBS_LIBRARY=${MPT_MACOS_OBS_LIBRARY}")
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}"
