#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw


DEFAULT_WIDTH = 320
DEFAULT_HEIGHT = 240
DEFAULT_FPS = 24
DEFAULT_DURATION_SECONDS = 2


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


def build_npy_payload(descr: str, shape: tuple[int, ...], payload: bytes) -> bytes:
    if len(descr) < 3:
        raise ValueError(f"Invalid dtype descriptor: {descr!r}")

    if len(shape) == 0:
        shape_text = "()"
    elif len(shape) == 1:
        shape_text = f"({shape[0]},)"
    else:
        shape_text = "(" + ", ".join(str(dim) for dim in shape) + ")"

    header = f"{{'descr': '{descr}', 'fortran_order': False, 'shape': {shape_text}, }}".encode("ascii")
    preamble_len = 10
    padding_len = (-((preamble_len + len(header) + 1) % 64)) % 64
    header_bytes = header + (b" " * padding_len) + b"\n"
    return b"\x93NUMPY\x01\x00" + struct.pack("<H", len(header_bytes)) + header_bytes + payload


def create_track_npz(output_path: Path, width: int, height: int, fps: int) -> None:
    mouth_left = width // 2 - 52
    mouth_top = height // 2 - 8
    mouth_right = mouth_left + 104
    mouth_bottom = mouth_top + 68
    frame_count = max(1, fps)

    quad = (
        float(mouth_left),
        float(mouth_top),
        float(mouth_right),
        float(mouth_top + 2),
        float(mouth_right - 2),
        float(mouth_bottom),
        float(mouth_left + 1),
        float(mouth_bottom - 1),
    )
    quad_payload = struct.pack("<" + "f" * (len(quad) * frame_count), *(quad * frame_count))
    valid_payload = bytes([1] * frame_count)
    width_payload = struct.pack("<i", width)
    height_payload = struct.pack("<i", height)

    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.writestr("quad.npy", build_npy_payload("<f4", (frame_count, 4, 2), quad_payload))
        archive.writestr("valid.npy", build_npy_payload("|u1", (frame_count,), valid_payload))
        archive.writestr("w.npy", build_npy_payload("<i4", (), width_payload))
        archive.writestr("h.npy", build_npy_payload("<i4", (), height_payload))


def write_manifest(output_path: Path, asset_root: Path) -> None:
    manifest = {
        "loop_video": str(asset_root / "loop.mp4"),
        "mouth_dir": str(asset_root / "mouth"),
        "track_file": str(asset_root / "mouth_track.npz"),
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
    create_track_npz(output_dir / "mouth_track.npz", args.width, args.height, args.fps)
    write_manifest(output_dir / "smoke-assets.json", output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
