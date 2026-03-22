#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path


LINUX_PLUGIN_BIN_PATTERNS = (
    "/usr/lib/obs-plugins",
    "/usr/lib64/obs-plugins",
    "/usr/lib/*/obs-plugins",
    "/usr/local/lib/obs-plugins",
    "/usr/local/lib64/obs-plugins",
    "/usr/local/lib/*/obs-plugins",
)

LINUX_PLUGIN_DATA_PATTERNS = (
    "/usr/share/obs/obs-plugins",
    "/usr/share/*/obs-plugins",
    "/usr/local/share/obs/obs-plugins",
    "/usr/local/share/*/obs-plugins",
)

PLUGIN_BIN_MARKERS = ("obs-ffmpeg.so", "linux-capture.so", "text-freetype2.so")
PLUGIN_DATA_MARKERS = ("obs-ffmpeg", "linux-capture", "text-freetype2")


def first_matching_directory(patterns: tuple[str, ...], markers: tuple[str, ...]) -> Path:
    candidates: list[Path] = []
    for pattern in patterns:
        candidates.extend(sorted(Path("/").glob(pattern.lstrip("/"))))

    for candidate in candidates:
        if not candidate.is_dir():
            continue
        for marker in markers:
            if (candidate / marker).exists():
                return candidate

    marker_text = ", ".join(markers)
    raise FileNotFoundError(f"Could not find an OBS plugin directory containing any of: {marker_text}")


def detect_linux_layout() -> dict[str, str]:
    obs_executable = shutil.which("obs")
    if not obs_executable:
        raise FileNotFoundError("Could not find the OBS executable in PATH.")

    plugin_bin_dir = first_matching_directory(LINUX_PLUGIN_BIN_PATTERNS, PLUGIN_BIN_MARKERS)
    plugin_data_dir = first_matching_directory(LINUX_PLUGIN_DATA_PATTERNS, PLUGIN_DATA_MARKERS)

    return {
        "obs_executable": obs_executable,
        "plugin_bin_dir": str(plugin_bin_dir),
        "plugin_data_dir": str(plugin_data_dir),
    }


def detect_macos_layout(obs_app_root: Path) -> dict[str, str]:
    obs_executable = obs_app_root / "Contents" / "MacOS" / "OBS"
    if not obs_executable.is_file():
        raise FileNotFoundError(f"Could not find the OBS executable at {obs_executable}")

    plugin_bin_dir = obs_app_root / "Contents" / "PlugIns"
    if not plugin_bin_dir.is_dir():
        raise FileNotFoundError(f"Could not find the OBS plugin directory at {plugin_bin_dir}")

    plugin_data_dir = obs_app_root / "Contents" / "Resources" / "data" / "obs-plugins"
    if not plugin_data_dir.is_dir():
        raise FileNotFoundError(f"Could not find the OBS plugin data directory at {plugin_data_dir}")

    marker_found = any((plugin_bin_dir / marker).exists() for marker in PLUGIN_BIN_MARKERS)
    if not marker_found and not any(plugin_bin_dir.rglob("obs-ffmpeg*")):
        raise FileNotFoundError(f"Could not find an existing OBS plugin marker inside {plugin_bin_dir}")

    if not any((plugin_data_dir / marker).exists() for marker in PLUGIN_DATA_MARKERS):
        raise FileNotFoundError(f"Could not find an existing OBS plugin data marker inside {plugin_data_dir}")

    return {
        "obs_executable": str(obs_executable),
        "plugin_bin_dir": str(plugin_bin_dir),
        "plugin_data_dir": str(plugin_data_dir),
    }


def write_github_output(path: Path, values: dict[str, str]) -> None:
    with path.open("a", encoding="utf-8", newline="\n") as handle:
        for key, value in values.items():
            handle.write(f"{key}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Locate OBS executable and plugin directories for smoke tests.")
    parser.add_argument("--platform", choices=("linux", "macos"), required=True)
    parser.add_argument(
        "--obs-app-root",
        default="/Applications/OBS.app",
        help="OBS.app root for macOS detection.",
    )
    parser.add_argument(
        "--github-output",
        help="Optional path to append GitHub Actions output lines to.",
    )
    args = parser.parse_args()

    if args.platform == "linux":
        layout = detect_linux_layout()
    else:
        layout = detect_macos_layout(Path(args.obs_app_root))

    if args.github_output:
        write_github_output(Path(args.github_output), layout)

    json.dump(layout, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
