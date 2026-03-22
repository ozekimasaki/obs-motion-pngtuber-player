from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any


def _normalize_token(value: str | None) -> str:
    if value is None:
        return ""
    return " ".join(str(value).strip().lower().split())


@dataclass(frozen=True, slots=True)
class DeviceIdentity:
    index: int | None
    name: str
    hostapi: str | None = None
    max_input_channels: int = 0
    default_samplerate: int = 0

    @property
    def display_name(self) -> str:
        pieces = []
        if self.index is not None:
            pieces.append(f"{self.index}")
        pieces.append(self.name)
        if self.hostapi:
            pieces.append(f"[{self.hostapi}]")
        if self.max_input_channels > 0:
            pieces.append(f"ch={self.max_input_channels}")
        if self.default_samplerate > 0:
            pieces.append(f"sr={self.default_samplerate}")
        return " ".join(pieces)

    def matches(self, other: "DeviceIdentity") -> bool:
        self_name = _normalize_token(self.name)
        other_name = _normalize_token(other.name)
        self_hostapi = _normalize_token(self.hostapi)
        other_hostapi = _normalize_token(other.hostapi)
        if self_name and self_name == other_name and self_hostapi and self_hostapi == other_hostapi:
            return True
        if self_name and self_name == other_name and self.max_input_channels == other.max_input_channels:
            return True
        return bool(self_name and self_name == other_name)

    def to_dict(self) -> dict[str, Any]:
        return {
            "index": self.index,
            "name": self.name,
            "hostapi": self.hostapi,
            "max_input_channels": self.max_input_channels,
            "default_samplerate": self.default_samplerate,
        }

    @classmethod
    def from_dict(cls, raw: Mapping[str, object] | None) -> "DeviceIdentity | None":
        if raw is None:
            return None
        name = str(raw.get("name", "") or "").strip()
        if not name:
            return None
        index_raw = raw.get("index")
        index = int(index_raw) if isinstance(index_raw, int) else None
        max_input_channels = int(raw.get("max_input_channels", 0) or 0)
        default_samplerate = int(float(raw.get("default_samplerate", 0) or 0))
        hostapi_raw = raw.get("hostapi")
        hostapi = str(hostapi_raw).strip() if hostapi_raw not in (None, "") else None
        return cls(
            index=index,
            name=name,
            hostapi=hostapi,
            max_input_channels=max_input_channels,
            default_samplerate=default_samplerate,
        )

    @classmethod
    def from_sounddevice(
        cls,
        index: int,
        device_info: Mapping[str, object],
        hostapi_name: str | None,
    ) -> "DeviceIdentity":
        return cls(
            index=int(index),
            name=str(device_info.get("name", "") or "").strip(),
            hostapi=str(hostapi_name).strip() if hostapi_name else None,
            max_input_channels=int(device_info.get("max_input_channels", 0) or 0),
            default_samplerate=int(float(device_info.get("default_samplerate", 0) or 0)),
        )
