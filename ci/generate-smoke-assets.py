#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import json
import math
import shutil
import struct
import subprocess
import wave
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw


DEFAULT_WIDTH = 320
DEFAULT_HEIGHT = 240
DEFAULT_FPS = 24
DEFAULT_DURATION_SECONDS = 2
DEFAULT_AUDIO_SAMPLE_RATE = 48000


def create_video(ffmpeg: str, output_path: Path, width: int, height: int, fps: int, duration_seconds: int) -> None:
    draw_grid = f"drawgrid=w={max(16, width // 8)}:h={max(16, height // 8)}:t=2:c=0x4a5c72"
    draw_face = f"drawbox=x={width // 6}:y={height // 5}:w={width * 2 // 3}:h={height // 2}:color=0x7fd36b@0.92:t=fill"
    draw_mouth = (
        f"drawbox=x={width // 3}:y={height // 2}:w={width // 3}:h={max(12, height // 14)}:color=0xf8ede5@0.85:t=fill"
    )
    filter_graph = ",".join((draw_grid, draw_face, draw_mouth))

    command = [
        ffmpeg,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"color=c=0x203044:s={width}x{height}:d={duration_seconds}:r={fps}",
        "-vf",
        filter_graph,
        "-pix_fmt",
        "yuv420p",
        str(output_path),
    ]
    subprocess.run(command, check=True)


def create_open_mouth_sprite(output_path: Path) -> None:
    image = Image.new("RGBA", (112, 72), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((6, 10, 106, 66), radius=24, fill=(222, 76, 100, 224), outline=(255, 233, 230, 255), width=4)
    draw.rounded_rectangle((24, 28, 88, 46), radius=8, fill=(255, 230, 226, 220))
    image.save(output_path)


def create_track_file(output_path: Path, width: int, height: int, fps: int) -> None:
    mouth_left = width // 2 - 52
    mouth_top = height // 2 - 8
    mouth_right = mouth_left + 104
    mouth_bottom = mouth_top + 68

    frame = {
        "valid": True,
        "x0": mouth_left,
        "y0": mouth_top,
        "x1": mouth_right,
        "y1": mouth_top + 2,
        "x2": mouth_right - 2,
        "y2": mouth_bottom,
        "x3": mouth_left + 1,
        "y3": mouth_bottom - 1,
    }

    payload = {
        "width": width,
        "height": height,
        "frames": [frame for _ in range(max(1, fps))],
    }

    output_path.write_text(json.dumps(payload, indent=2), encoding="utf-8", newline="\n")


def create_lip_sync_audio(output_path: Path, sample_rate: int = DEFAULT_AUDIO_SAMPLE_RATE, duration_seconds: int = 6) -> None:
    segment_duration_seconds = duration_seconds / 2.0
    amplitude = 0.85
    frequency_hz = 440.0
    frame_count = max(1, int(sample_rate * duration_seconds))

    samples = bytearray()
    for frame_index in range(frame_count):
        time_seconds = frame_index / sample_rate
        segment_index = int(time_seconds / segment_duration_seconds)
        value = 0.0
        if segment_index % 2 == 1:
            value = amplitude * math.sin(2.0 * math.pi * frequency_hz * time_seconds)
        sample = max(-32767, min(32767, int(round(value * 32767.0))))
        samples.extend(struct.pack("<h", sample))

    with wave.open(str(output_path), "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(bytes(samples))


def build_npy_payload(descr: str, shape: tuple[int, ...], payload: bytes, *, fortran_order: bool = False) -> bytes:
    if len(descr) < 3:
        raise ValueError(f"Invalid dtype descriptor: {descr!r}")

    if len(shape) == 0:
        shape_text = "()"
    elif len(shape) == 1:
        shape_text = f"({shape[0]},)"
    else:
        shape_text = "(" + ", ".join(str(dim) for dim in shape) + ")"

    header = f"{{'descr': '{descr}', 'fortran_order': {str(fortran_order)}, 'shape': {shape_text}, }}".encode("ascii")
    preamble_len = 10
    padding_len = (-((preamble_len + len(header) + 1) % 64)) % 64
    header_bytes = header + (b" " * padding_len) + b"\n"
    return b"\x93NUMPY\x01\x00" + struct.pack("<H", len(header_bytes)) + header_bytes + payload


class UnseekableBytesIO(io.BytesIO):
    def seekable(self) -> bool:
        return False

    def seek(self, offset: int, whence: int = 0) -> int:
        raise io.UnsupportedOperation("seek")

    def tell(self) -> int:
        raise io.UnsupportedOperation("tell")


def build_quad_payload(
    frame_count: int,
    mouth_left: int,
    mouth_top: int,
    mouth_right: int,
    mouth_bottom: int,
    *,
    byte_order: str,
    fortran_order: bool,
) -> bytes:
    quad = (
        (
            float(mouth_left),
            float(mouth_top),
        ),
        (
            float(mouth_right),
            float(mouth_top + 2),
        ),
        (
            float(mouth_right - 2),
            float(mouth_bottom),
        ),
        (
            float(mouth_left + 1),
            float(mouth_bottom - 1),
        ),
    )

    values: list[float] = []
    if fortran_order:
        for axis2 in range(2):
            for axis1 in range(4):
                for _ in range(frame_count):
                    values.append(quad[axis1][axis2])
    else:
        for _ in range(frame_count):
            for axis1 in range(4):
                for axis2 in range(2):
                    values.append(quad[axis1][axis2])

    return struct.pack(f"{byte_order}{len(values)}f", *values)


def create_track_npz(output_path: Path, width: int, height: int, fps: int, *, compression: int, streamed: bool = False) -> None:
    mouth_left = width // 2 - 52
    mouth_top = height // 2 - 8
    mouth_right = mouth_left + 104
    mouth_bottom = mouth_top + 68
    frame_count = max(1, fps)

    quad_payload = build_quad_payload(
        frame_count,
        mouth_left,
        mouth_top,
        mouth_right,
        mouth_bottom,
        byte_order=">",
        fortran_order=True,
    )
    valid_payload = bytes([1] * frame_count)
    width_payload = struct.pack(">i", width)
    height_payload = struct.pack(">i", height)

    members = {
        "quad.npy": build_npy_payload(">f4", (frame_count, 4, 2), quad_payload, fortran_order=True),
        "valid.npy": build_npy_payload("|u1", (frame_count,), valid_payload),
        "w.npy": build_npy_payload(">i4", (), width_payload),
        "h.npy": build_npy_payload(">i4", (), height_payload),
    }

    if streamed:
        buffer = UnseekableBytesIO()
        with zipfile.ZipFile(buffer, "w", compression=compression) as archive:
            for name, payload in members.items():
                with archive.open(name, "w") as member:
                    member.write(payload)
        output_path.write_bytes(buffer.getvalue())
        return

    with zipfile.ZipFile(output_path, "w", compression=compression) as archive:
        for name, payload in members.items():
            archive.writestr(name, payload)


def write_manifest(output_path: Path, asset_root: Path) -> None:
    manifest = {
        "loop_video": str(asset_root / "loop.mp4"),
        "mouth_dir": str(asset_root / "mouth"),
        "track_file": str(asset_root / "mouth_track_nyapan.npz"),
        "lip_sync_audio": str(asset_root / "lip_sync_tone.wav"),
    }
    output_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate deterministic MotionPngTuberPlayer smoke-test assets.")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    parser.add_argument("--duration-seconds", type=int, default=DEFAULT_DURATION_SECONDS)
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    if output_dir.exists():
        shutil.rmtree(output_dir)
    (output_dir / "mouth").mkdir(parents=True, exist_ok=True)

    create_video(args.ffmpeg, output_dir / "loop.mp4", args.width, args.height, args.fps, args.duration_seconds)
    create_open_mouth_sprite(output_dir / "mouth" / "open.png")
    create_track_file(output_dir / "mouth_track.json", args.width, args.height, args.fps)
    create_lip_sync_audio(output_dir / "lip_sync_tone.wav")
    create_track_npz(output_dir / "mouth_track.npz", args.width, args.height, args.fps, compression=zipfile.ZIP_STORED)
    create_track_npz(
        output_dir / "mouth_track_nyapan.npz",
        args.width,
        args.height,
        args.fps,
        compression=zipfile.ZIP_DEFLATED,
        streamed=True,
    )
    write_manifest(output_dir / "smoke-assets.json", output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
