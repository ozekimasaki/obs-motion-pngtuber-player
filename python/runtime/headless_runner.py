from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import os
import queue
import time

import cv2
import numpy as np

try:
    import sounddevice as sd
except Exception:  # pragma: no cover - environment dependent
    sd = None  # type: ignore[assignment]

try:
    from realtime_emotion_audio import RealtimeEmotionAnalyzer  # type: ignore
    HAS_EMOTION_AUDIO = True
except Exception:  # pragma: no cover - environment dependent
    RealtimeEmotionAnalyzer = None  # type: ignore[assignment]
    HAS_EMOTION_AUDIO = False

from lipsync_core import (
    BgVideo,
    MouthTrack,
    alpha_blit_rgb_safe,
    discover_mouth_sets,
    format_emotion_hud_text,
    infer_label_from_set_name,
    load_mouth_sprites,
    one_pole_beta,
    pick_mouth_set_for_label,
    probe_video_size,
    warp_rgba_to_quad,
)

from .audio_input_service import AudioInputService
from .device_identity import DeviceIdentity
from .runtime_config import RuntimeConfig

EMOTION_PRESET_PARAMS = {
    "stable": dict(smooth_alpha=0.18, min_hold_sec=0.75, cand_stable_sec=0.30, switch_margin=0.14),
    "standard": dict(smooth_alpha=0.25, min_hold_sec=0.45, cand_stable_sec=0.22, switch_margin=0.10),
    "snappy": dict(smooth_alpha=0.35, min_hold_sec=0.25, cand_stable_sec=0.12, switch_margin=0.06),
}


@dataclass(slots=True)
class HeadlessStatus:
    device_index: int | None
    device_name: str | None
    samplerate: int
    width: int
    height: int
    current_emotion: str
    current_label: str
    current_mouth_shape: str
    last_hud_text: str
    resolved_track_path: str
    last_error: str | None = None


class HeadlessRunner:
    def __init__(
        self,
        config: RuntimeConfig,
        *,
        audio_service: AudioInputService | None = None,
    ) -> None:
        self.config = config.normalized()
        self.audio_service = audio_service or AudioInputService()
        self.last_error: str | None = None

        self.full_w, self.full_h = self._resolve_canvas_size(self.config)
        self.mouth_fixed_x = self.config.mouth_fixed_x if self.config.mouth_fixed_x is not None else int(self.full_w * 0.50)
        self.mouth_fixed_y = self.config.mouth_fixed_y if self.config.mouth_fixed_y is not None else int(self.full_h * 0.58)

        self.loop_video = self.config.loop_video
        if not os.path.isfile(self.loop_video):
            raise FileNotFoundError(f"Loop video not found: {self.loop_video}")

        self.mouth_dir = self._resolve_mouth_dir(self.config.mouth_dir)
        self.mouth_sets = self._load_mouth_sets(self.mouth_dir)
        self.emotions = sorted(self.mouth_sets.keys())
        self.neutral_set = self._resolve_neutral_set(self.emotions)

        desired = (self.config.emotion or "").strip()
        if desired and desired in self.mouth_sets:
            self.current_emotion = desired
        else:
            self.current_emotion = self.neutral_set
        self._mouth = self.mouth_sets[self.current_emotion]

        self.resolved_track_path = self.config.resolve_track_path()
        self.track = MouthTrack.load(self.resolved_track_path, self.full_w, self.full_h, policy=self.config.valid_policy)

        self.bg_video = BgVideo(self.loop_video, self.full_w, self.full_h)
        self.device = self.audio_service.resolve_device(self.config.device_identity, self.config.device_index)
        self.samplerate = self.audio_service.samplerate_for(self.device)

        self._feature_queue: queue.Queue[tuple[float, float]] = queue.Queue(maxsize=self.config.audio_hz * 2)
        self._emotion_audio_q: queue.Queue[np.ndarray] | None = None
        self._emotion_analyzer = None
        self._emotion_buffer = np.zeros((0,), dtype=np.float32)
        self._emotion_window_sec = 0.25
        self._emotion_eval_interval = 0.10
        self._emotion_window_len = 0
        self._last_emotion_eval = 0.0

        self._audio_stream = None
        self._started = False
        self._t0 = time.perf_counter()

        self._beta = one_pole_beta(self.config.cutoff_hz, self.config.audio_hz)
        self._noise = 1e-4
        self._peak = 1e-3
        self._peak_decay = 0.995
        self._rms_smooth_q: deque[float] = deque(maxlen=3)
        self._env_lp = 0.0
        self._env_hist: deque[float] = deque(maxlen=self.config.audio_hz * self.config.hist_sec)
        self._cent_hist: deque[float] = deque(maxlen=self.config.audio_hz * self.config.hist_sec)
        self._talk_th = 0.06
        self._half_th = 0.30
        self._open_th = 0.52
        self._u_th = 0.16
        self._e_th = 0.20
        self._current_open_shape = "open"
        self._last_vowel_change_t = -999.0
        self._e_prev2 = 0.0
        self._e_prev1 = 0.0
        self._mouth_shape_now = "closed"

        self._emotion_auto_enabled = bool(self.config.emotion_auto) and (len(self.mouth_sets) > 1) and HAS_EMOTION_AUDIO and (RealtimeEmotionAnalyzer is not None)
        if self._emotion_auto_enabled:
            self._emotion_audio_q = queue.Queue(maxsize=max(8, self.config.audio_hz * 2))
            self._emotion_window_len = int(self.samplerate * self._emotion_window_sec)
            params = EMOTION_PRESET_PARAMS[self.config.emotion_preset]
            self._emotion_analyzer = RealtimeEmotionAnalyzer(sr=int(self.samplerate), **params)  # type: ignore[misc]
            self.current_emotion = self.neutral_set
            self._mouth = self.mouth_sets[self.current_emotion]

        self._last_hud_text = format_emotion_hud_text(infer_label_from_set_name(self.current_emotion))

    @staticmethod
    def _resolve_canvas_size(config: RuntimeConfig) -> tuple[int, int]:
        full_w, full_h = config.full_w, config.full_h
        probed = probe_video_size(config.loop_video)
        if probed is not None:
            video_w, video_h = probed
            if full_w <= 0 or full_h <= 0:
                return video_w, video_h
            req_aspect = full_w / max(1, full_h)
            vid_aspect = video_w / max(1, video_h)
            if abs(req_aspect - vid_aspect) > 0.05:
                return video_w, video_h
            return full_w, full_h

        if full_w > 0 and full_h > 0:
            return full_w, full_h
        return 1440, 2560

    def _resolve_mouth_dir(self, configured: str) -> str:
        candidates = []
        if configured:
            candidates.append(configured)
        loop_dir = os.path.dirname(os.path.abspath(self.loop_video))
        candidates.extend(
            [
                os.path.join(loop_dir, "mouth"),
                os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mouth"),
                os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mouth_dir"),
            ]
        )
        for candidate in candidates:
            resolved = os.path.abspath(candidate)
            if os.path.isdir(resolved):
                return resolved
        raise FileNotFoundError(f"mouth dir not found: {configured or '(auto)'}")

    def _load_mouth_sets(self, mouth_dir: str) -> dict[str, dict[str, np.ndarray]]:
        sets_dirs = discover_mouth_sets(mouth_dir)
        if not sets_dirs:
            raise FileNotFoundError(
                f"No mouth sprite sets found under: {mouth_dir} (need open.png or subfolders with open.png)"
            )

        mouth_sets: dict[str, dict[str, np.ndarray]] = {}
        for name, path in sets_dirs.items():
            mouth_sets[name] = load_mouth_sprites(path, self.full_w, self.full_h)
        if not mouth_sets:
            raise RuntimeError(f"All mouth sprite sets failed to load under: {mouth_dir}")
        return mouth_sets

    def _resolve_neutral_set(self, emotions: list[str]) -> str:
        neutral = pick_mouth_set_for_label(emotions, "neutral")
        if neutral is not None:
            return neutral
        if "Default" in self.mouth_sets:
            return "Default"
        return emotions[0]

    def start(self) -> None:
        if self._started:
            return
        self._t0 = time.perf_counter()
        self._open_audio_stream()
        self._started = True

    def _open_audio_stream(self) -> None:
        if sd is None:  # pragma: no cover - environment dependent
            self.last_error = "sounddevice is not available"
            return

        hop = int(self.samplerate / self.config.audio_hz)
        hop = max(hop, 256)
        self._hop = hop
        self._window = np.hanning(hop).astype(np.float32)
        self._freqs = np.fft.rfftfreq(hop, d=1.0 / self.samplerate)

        def audio_cb(indata, frames, time_info, status):
            x = indata.astype(np.float32)
            if x.ndim == 2:
                x = x.mean(axis=1)
            if len(x) < self._hop:
                x = np.pad(x, (0, self._hop - len(x)))
            elif len(x) > self._hop:
                x = x[: self._hop]

            rms_raw = float(np.sqrt(np.mean(x * x) + 1e-12))
            w = x * self._window
            mag = np.abs(np.fft.rfft(w)) + 1e-9
            centroid = float((self._freqs * mag).sum() / mag.sum())
            centroid = float(np.clip(centroid / (self.samplerate * 0.5), 0.0, 1.0))
            try:
                self._feature_queue.put_nowait((rms_raw, centroid))
            except queue.Full:
                pass
            if self._emotion_audio_q is not None:
                try:
                    self._emotion_audio_q.put_nowait(x)
                except queue.Full:
                    pass

        try:
            self._audio_stream = sd.InputStream(
                samplerate=self.samplerate,
                channels=1,
                blocksize=self._hop,
                dtype="float32",
                callback=audio_cb,
                device=self.device.index if self.device is not None else None,
                latency="low",
            )
            self._audio_stream.start()
        except Exception as exc:
            self.last_error = f"failed to open audio device: {exc}"
            fallback = self.audio_service.resolve_device()
            if fallback is None:
                return
            try:
                self.device = fallback
                self.samplerate = self.audio_service.samplerate_for(fallback)
                self._audio_stream = sd.InputStream(
                    samplerate=self.samplerate,
                    channels=1,
                    blocksize=self._hop,
                    dtype="float32",
                    callback=audio_cb,
                    device=fallback.index,
                    latency="low",
                )
                self._audio_stream.start()
                self.last_error = None
            except Exception as fallback_exc:
                self.last_error = f"failed to open fallback audio device: {fallback_exc}"
                self._audio_stream = None

    def render_frame(self, now: float | None = None) -> np.ndarray:
        if not self._started:
            self.start()
        frame_time = time.perf_counter() if now is None else float(now)
        self._update_emotion(frame_time)
        self._update_audio(frame_time)

        frame = self.bg_video.get_frame(frame_time).copy()
        self._draw_one(frame, self.bg_video.frame_idx)
        return frame

    def _update_emotion(self, now: float) -> None:
        if not self._emotion_auto_enabled or self._emotion_audio_q is None or self._emotion_analyzer is None:
            return

        while True:
            try:
                self._emotion_buffer = np.concatenate([self._emotion_buffer, self._emotion_audio_q.get_nowait()])
            except queue.Empty:
                break

        max_len = int(self.samplerate * 1.2)
        if self._emotion_buffer.size > max_len:
            self._emotion_buffer = self._emotion_buffer[-max_len:]

        if (now - self._last_emotion_eval) < self._emotion_eval_interval:
            return
        if self._emotion_buffer.size < self._emotion_window_len:
            return

        self._last_emotion_eval = now
        window = self._emotion_buffer[-self._emotion_window_len :]
        label, info = self._emotion_analyzer.update(window)
        if label is None:
            return

        rms_db = float(info.get("rms_db", -120.0))
        voiced = float(info.get("voiced", 0.0)) >= 0.5
        if rms_db < float(self.config.emotion_silence_db):
            target_label = "neutral"
            target_set = self.neutral_set
        elif not voiced:
            target_label = None
            target_set = None
        else:
            target_label = str(label).lower()
            target_set = pick_mouth_set_for_label(self.emotions, target_label) or self.neutral_set

        if target_set in self.mouth_sets and target_set != self.current_emotion:
            self.current_emotion = target_set
            self._mouth = self.mouth_sets[target_set]
            self._last_hud_text = format_emotion_hud_text(target_label or "neutral")

    def _update_audio(self, now: float) -> None:
        items: list[tuple[float, float]] = []
        while True:
            try:
                items.append(self._feature_queue.get_nowait())
            except queue.Empty:
                break

        t = now - self._t0
        for rms_raw, cent in items:
            if rms_raw < self._noise + 0.0005:
                self._noise = 0.99 * self._noise + 0.01 * rms_raw
            else:
                self._noise = 0.999 * self._noise + 0.001 * rms_raw

            self._peak = max(rms_raw, self._peak * self._peak_decay, self._noise + self.config.silence_gate)
            denom = max(self._peak - self._noise, self.config.silence_gate)
            rms_norm = float(np.clip((rms_raw - self._noise) / denom, 0.0, 1.0) ** 0.5)
            if rms_raw < self._noise + self.config.silence_gate:
                rms_norm = 0.0

            self._rms_smooth_q.append(rms_norm)
            rms_sm = float(np.mean(self._rms_smooth_q))
            self._env_lp = self._env_lp + self._beta * (rms_sm - self._env_lp)
            env = float(np.clip(0.75 * self._env_lp + 0.25 * rms_sm, 0.0, 1.0))

            self._env_hist.append(env)
            self._cent_hist.append(float(cent))

            if len(self._env_hist) > self.config.audio_hz * 3 and (len(self._env_hist) % self.config.audio_hz == 0):
                values = np.array(self._env_hist, dtype=np.float32)
                head_count = max(1, int(0.2 * len(values)))
                noise_floor_env = float(np.median(np.sort(values)[:head_count]))
                self._talk_th = float(np.clip(noise_floor_env + 0.05, 0.03, 0.18))

                talk_values = values[values > self._talk_th]
                if len(talk_values) > 20:
                    self._half_th = float(np.percentile(talk_values, 25))
                    self._open_th = float(np.percentile(talk_values, 58))
                    self._half_th = max(self._half_th, self._talk_th + 0.02)
                    self._open_th = max(self._open_th, self._half_th + 0.05)

                    cents = np.array(self._cent_hist, dtype=np.float32)
                    open_mask = values >= self._open_th
                    cent_open = cents[open_mask] if open_mask.sum() > 20 else cents[values > self._talk_th]
                    if len(cent_open) > 20:
                        self._u_th = float(np.percentile(cent_open, 20))
                        self._e_th = float(np.percentile(cent_open, 80))

            if env < self._half_th:
                mouth_level = "closed"
            elif env < self._open_th:
                mouth_level = "half"
            else:
                mouth_level = "open"

            if mouth_level == "open":
                is_peak = (
                    (self._e_prev2 < self._e_prev1)
                    and (self._e_prev1 >= env)
                    and (self._e_prev1 > self._open_th + self.config.peak_margin)
                )
                if is_peak and (t - self._last_vowel_change_t) >= self.config.min_vowel_interval:
                    if len(self._cent_hist) >= 5:
                        centroid_mean = float(np.mean(list(self._cent_hist)[-5:]))
                    else:
                        centroid_mean = float(cent)

                    if centroid_mean < self._u_th:
                        self._current_open_shape = "u"
                    elif centroid_mean > self._e_th:
                        self._current_open_shape = "e"
                    else:
                        self._current_open_shape = "open"
                    self._last_vowel_change_t = t
                self._mouth_shape_now = self._current_open_shape
            elif mouth_level == "half":
                self._mouth_shape_now = "half"
            else:
                self._mouth_shape_now = "closed"

            self._e_prev2, self._e_prev1 = self._e_prev1, env

    def _draw_one(self, dst_rgb: np.ndarray, frame_idx: int) -> None:
        sprite = self._mouth.get(self._mouth_shape_now, self._mouth["closed"])
        quad = self.track.get_quad(frame_idx) if self.track is not None else None
        if quad is None:
            x = int(self.mouth_fixed_x - sprite.shape[1] // 2)
            y = int(self.mouth_fixed_y - sprite.shape[0] // 2)
            alpha_blit_rgb_safe(dst_rgb, sprite, x, y)
            return

        patch, x0, y0 = warp_rgba_to_quad(sprite, quad)
        alpha_blit_rgb_safe(dst_rgb, patch, x0, y0)
        if self.config.draw_quad:
            quad_int = quad.astype(np.int32).reshape(4, 2)
            cv2.polylines(dst_rgb, [quad_int], isClosed=True, color=(0, 255, 0), thickness=2)

    def status_snapshot(self) -> HeadlessStatus:
        return HeadlessStatus(
            device_index=self.device.index if self.device is not None else None,
            device_name=self.device.name if self.device is not None else None,
            samplerate=self.samplerate,
            width=self.full_w,
            height=self.full_h,
            current_emotion=self.current_emotion,
            current_label=infer_label_from_set_name(self.current_emotion),
            current_mouth_shape=self._mouth_shape_now,
            last_hud_text=self._last_hud_text,
            resolved_track_path=self.resolved_track_path,
            last_error=self.last_error,
        )

    def close(self) -> None:
        if self._audio_stream is not None:
            try:
                self._audio_stream.stop()
            except Exception:
                pass
            try:
                self._audio_stream.close()
            except Exception:
                pass
            self._audio_stream = None
        self.bg_video.close()
        self._started = False

    def __enter__(self) -> "HeadlessRunner":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()
