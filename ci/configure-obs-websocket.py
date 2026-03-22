#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path


DEFAULT_CONFIG_RELATIVE_PATH = Path("plugin_config") / "obs-websocket" / "config.json"
CONFIG_SEARCH_PATTERNS = (
    "**/plugin_config/obs-websocket/config.json",
    "**/obs-websocket/config.json",
)


def find_config_path(config_root: Path) -> Path:
    direct_path = config_root / DEFAULT_CONFIG_RELATIVE_PATH
    if direct_path.is_file():
        return direct_path

    for pattern in CONFIG_SEARCH_PATTERNS:
        for candidate in sorted(config_root.glob(pattern)):
            if candidate.is_file():
                return candidate

    return direct_path


def load_existing_config(config_path: Path) -> dict[str, object]:
    if not config_path.exists():
        return {}

    with config_path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)

    if not isinstance(data, dict):
        raise TypeError(f"Expected a JSON object in {config_path}, got {type(data).__name__}")

    return data


def main() -> int:
    parser = argparse.ArgumentParser(description="Configure the OBS websocket server for smoke tests.")
    parser.add_argument("--config-root", required=True, help="OBS config root (for example obs-studio).")
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--password", default="unused")
    args = parser.parse_args()

    config_root = Path(args.config_root)
    config_path = find_config_path(config_root)
    config = load_existing_config(config_path)
    config.update(
        {
            "alerts_enabled": False,
            "auth_required": False,
            "first_load": False,
            "server_enabled": True,
            "server_password": args.password,
            "server_port": args.port,
        }
    )

    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"config_path": str(config_path), "server_port": args.port}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
