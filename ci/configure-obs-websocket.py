#!/usr/bin/env python3
from __future__ import annotations

import argparse
import configparser
import json
from pathlib import Path


DEFAULT_CONFIG_RELATIVE_PATH = Path("plugin_config") / "obs-websocket" / "config.json"
CONFIG_SEARCH_PATTERNS = (
    "**/plugin_config/obs-websocket/config.json",
    "**/obs-websocket/config.json",
)
LEGACY_CONFIG_FILE_NAME = "global.ini"
LEGACY_CONFIG_SECTION = "OBSWebSocket"


def create_ini_parser() -> configparser.RawConfigParser:
    parser = configparser.RawConfigParser(strict=False)
    parser.optionxform = str
    return parser


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

    with config_path.open("r", encoding="utf-8-sig") as handle:
        data = json.load(handle)

    if not isinstance(data, dict):
        raise TypeError(f"Expected a JSON object in {config_path}, got {type(data).__name__}")

    return data


def update_legacy_global_config(
    config_root: Path,
    port: int,
    password: str,
    *,
    mark_macos_permissions_dialog_shown: bool = False,
) -> Path:
    global_config_path = config_root / LEGACY_CONFIG_FILE_NAME
    parser = create_ini_parser()

    if global_config_path.exists():
        parser.read(global_config_path, encoding="utf-8-sig")

    if not parser.has_section(LEGACY_CONFIG_SECTION):
        parser.add_section(LEGACY_CONFIG_SECTION)

    parser.set(LEGACY_CONFIG_SECTION, "FirstLoad", "false")
    parser.set(LEGACY_CONFIG_SECTION, "ServerEnabled", "true")
    parser.set(LEGACY_CONFIG_SECTION, "ServerPort", str(port))
    parser.set(LEGACY_CONFIG_SECTION, "AlertsEnabled", "false")
    parser.set(LEGACY_CONFIG_SECTION, "AuthRequired", "false")
    parser.set(LEGACY_CONFIG_SECTION, "ServerPassword", password)

    if mark_macos_permissions_dialog_shown:
        if not parser.has_section("General"):
            parser.add_section("General")
        parser.set("General", "MacOSPermissionsDialogLastShown", "1")

    global_config_path.parent.mkdir(parents=True, exist_ok=True)
    with global_config_path.open("w", encoding="utf-8", newline="\n") as handle:
        parser.write(handle, space_around_delimiters=False)

    return global_config_path


def update_user_config(config_root: Path, *, mark_first_run_complete: bool = False) -> Path | None:
    user_config_path = config_root / "user.ini"
    if not mark_first_run_complete:
        return user_config_path if user_config_path.exists() else None

    parser = create_ini_parser()

    if user_config_path.exists():
        parser.read(user_config_path, encoding="utf-8-sig")

    if not parser.has_section("General"):
        parser.add_section("General")
    parser.set("General", "FirstRun", "true")

    user_config_path.parent.mkdir(parents=True, exist_ok=True)
    with user_config_path.open("w", encoding="utf-8", newline="\n") as handle:
        parser.write(handle, space_around_delimiters=False)

    return user_config_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Configure the OBS websocket server for smoke tests.")
    parser.add_argument("--config-root", required=True, help="OBS config root (for example obs-studio).")
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--password", default="unused")
    parser.add_argument("--set-macos-permissions-dialog-shown", action="store_true")
    parser.add_argument("--mark-first-run-complete", action="store_true")
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
    global_config_path = update_legacy_global_config(
        config_root,
        args.port,
        args.password,
        mark_macos_permissions_dialog_shown=args.set_macos_permissions_dialog_shown,
    )
    user_config_path = update_user_config(
        config_root,
        mark_first_run_complete=args.mark_first_run_complete,
    )
    print(
        json.dumps(
            {
                "config_path": str(config_path),
                "global_config_path": str(global_config_path),
                "user_config_path": str(user_config_path) if user_config_path else None,
                "server_port": args.port,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
