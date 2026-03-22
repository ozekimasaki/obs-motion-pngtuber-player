from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, replace
import os
from typing import Any
from typing import Literal

from .device_identity import DeviceIdentity

ValidPolicy = Literal["hold", "strict"]
EmotionPreset = Literal["stable", "standard", "snappy"]


def _optional_int(value: object) -> int | None:
    if value in (None, ""):
        return None
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, (int, float)):
        return int(value)
    text = str(value).strip()
    if not text:
        return None
    return int(float(text))


@dataclass(slots=True)
class RuntimeConfig:
    loop_video: str
    mouth_dir: str = ""
    track: str = ""
    track_calibrated: str = ""
    prefer_calibrated: bool = True
    full_w: int = 0
    full_h: int = 0
    render_fps: int = 30
    audio_hz: int = 100
    cutoff_hz: float = 8.0
    device_index: int | None = None
    device_identity: DeviceIdentity | None = None
    mouth_fixed_x: int | None = None
    mouth_fixed_y: int | None = None
    valid_policy: ValidPolicy = "hold"
    draw_quad: bool = False
    min_vowel_interval: float = 0.12
    peak_margin: float = 0.02
    silence_gate: float = 0.002
    hist_sec: int = 10
    emotion: str = ""
    emotion_auto: bool = True
    emotion_preset: EmotionPreset = "standard"
    emotion_hud: bool = False
    emotion_silence_db: float = -65.0
    emotion_min_conf: float = 0.45

    def normalized(self) -> "RuntimeConfig":
        preset = self.emotion_preset if self.emotion_preset in {"stable", "standard", "snappy"} else "standard"
        policy = self.valid_policy if self.valid_policy in {"hold", "strict"} else "hold"
        return replace(
            self,
            loop_video=os.path.abspath(self.loop_video) if self.loop_video else "",
            mouth_dir=os.path.abspath(self.mouth_dir) if self.mouth_dir else "",
            track=os.path.abspath(self.track) if self.track else "",
            track_calibrated=os.path.abspath(self.track_calibrated) if self.track_calibrated else "",
            full_w=max(0, int(self.full_w or 0)),
            full_h=max(0, int(self.full_h or 0)),
            render_fps=max(1, int(self.render_fps or 30)),
            audio_hz=max(10, int(self.audio_hz or 100)),
            cutoff_hz=max(0.1, float(self.cutoff_hz or 8.0)),
            valid_policy=policy,
            min_vowel_interval=max(0.01, float(self.min_vowel_interval or 0.12)),
            peak_margin=max(0.0, float(self.peak_margin or 0.02)),
            silence_gate=max(0.0, float(self.silence_gate or 0.002)),
            hist_sec=max(1, int(self.hist_sec or 10)),
            emotion=str(self.emotion or "").strip(),
            emotion_preset=preset,
            emotion_silence_db=float(self.emotion_silence_db),
            emotion_min_conf=max(0.0, float(self.emotion_min_conf or 0.45)),
        )

    def resolve_track_path(self) -> str:
        if self.prefer_calibrated and self.track_calibrated and os.path.isfile(self.track_calibrated):
            return self.track_calibrated
        return self.track

    def to_dict(self) -> dict[str, Any]:
        return {
            "loop_video": self.loop_video,
            "mouth_dir": self.mouth_dir,
            "track": self.track,
            "track_calibrated": self.track_calibrated,
            "prefer_calibrated": self.prefer_calibrated,
            "full_w": self.full_w,
            "full_h": self.full_h,
            "render_fps": self.render_fps,
            "audio_hz": self.audio_hz,
            "cutoff_hz": self.cutoff_hz,
            "device_index": self.device_index,
            "device_identity": self.device_identity.to_dict() if self.device_identity is not None else None,
            "mouth_fixed_x": self.mouth_fixed_x,
            "mouth_fixed_y": self.mouth_fixed_y,
            "valid_policy": self.valid_policy,
            "draw_quad": self.draw_quad,
            "min_vowel_interval": self.min_vowel_interval,
            "peak_margin": self.peak_margin,
            "silence_gate": self.silence_gate,
            "hist_sec": self.hist_sec,
            "emotion": self.emotion,
            "emotion_auto": self.emotion_auto,
            "emotion_preset": self.emotion_preset,
            "emotion_hud": self.emotion_hud,
            "emotion_silence_db": self.emotion_silence_db,
            "emotion_min_conf": self.emotion_min_conf,
        }

    @classmethod
    def from_dict(cls, raw: Mapping[str, object]) -> "RuntimeConfig":
        device_identity_raw = raw.get("device_identity")
        device_identity = DeviceIdentity.from_dict(device_identity_raw) if isinstance(device_identity_raw, Mapping) else None
        device_index = _optional_int(raw.get("device_index"))
        return cls(
            loop_video=str(raw.get("loop_video", "") or ""),
            mouth_dir=str(raw.get("mouth_dir", "") or ""),
            track=str(raw.get("track", "") or ""),
            track_calibrated=str(raw.get("track_calibrated", "") or ""),
            prefer_calibrated=bool(raw.get("prefer_calibrated", True)),
            full_w=int(raw.get("full_w", 0) or 0),
            full_h=int(raw.get("full_h", 0) or 0),
            render_fps=int(raw.get("render_fps", 30) or 30),
            audio_hz=int(raw.get("audio_hz", 100) or 100),
            cutoff_hz=float(raw.get("cutoff_hz", 8.0) or 8.0),
            device_index=device_index,
            device_identity=device_identity,
            mouth_fixed_x=int(raw.get("mouth_fixed_x", 0) or 0) or None,
            mouth_fixed_y=int(raw.get("mouth_fixed_y", 0) or 0) or None,
            valid_policy=str(raw.get("valid_policy", "hold") or "hold"),
            draw_quad=bool(raw.get("draw_quad", False)),
            min_vowel_interval=float(raw.get("min_vowel_interval", 0.12) or 0.12),
            peak_margin=float(raw.get("peak_margin", 0.02) or 0.02),
            silence_gate=float(raw.get("silence_gate", 0.002) or 0.002),
            hist_sec=int(raw.get("hist_sec", 10) or 10),
            emotion=str(raw.get("emotion", "") or ""),
            emotion_auto=bool(raw.get("emotion_auto", True)),
            emotion_preset=str(raw.get("emotion_preset", "standard") or "standard"),
            emotion_hud=bool(raw.get("emotion_hud", False)),
            emotion_silence_db=float(raw.get("emotion_silence_db", -65.0) or -65.0),
            emotion_min_conf=float(raw.get("emotion_min_conf", 0.45) or 0.45),
        )
