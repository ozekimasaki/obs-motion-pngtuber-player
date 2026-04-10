#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate Linux validation assets for MotionPngTuberPlayer tests.")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    return parser.parse_args()


def run_ffmpeg(ffmpeg: str, *args: str) -> None:
    subprocess.run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", *args], check=True)


def make_npy(payload: bytes, descr: str, shape: tuple[int, ...], fortran_order: bool = False) -> bytes:
    prefix = b"\x93NUMPY\x01\x00"
    header = (
        "{'descr': '%s', 'fortran_order': %s, 'shape': %s, }"
        % (descr, "True" if fortran_order else "False", repr(shape))
    ).encode("latin1")
    padding = (16 - ((len(prefix) + 2 + len(header) + 1) % 16)) % 16
    header += b" " * padding + b"\n"
    return prefix + struct.pack("<H", len(header)) + header + payload


def write_track_assets(output_dir: Path) -> None:
    frames = [
        (18.0, 28.0, 28.0, 8.0),
        (18.0, 26.0, 28.0, 12.0),
        (16.0, 24.0, 32.0, 16.0),
        (18.0, 26.0, 28.0, 12.0),
    ]
    valid = [1, 1, 1, 1]
    width = 64.0
    height = 64.0

    json_frames = []
    for x, y, w, h in frames:
        json_frames.append(
            {
                "valid": True,
                "x0": x,
                "y0": y,
                "x1": x + w,
                "y1": y,
                "x2": x + w,
                "y2": y + h,
                "x3": x,
                "y3": y + h,
            }
        )

    (output_dir / "mouth_track.json").write_text(
        json.dumps({"width": width, "height": height, "frames": json_frames}, separators=(",", ":")),
        encoding="utf-8",
    )

    bbox_payload = struct.pack("<%sd" % (len(frames) * 4), *(value for frame in frames for value in frame))
    valid_payload = struct.pack("<%dB" % len(valid), *valid)
    scalar_width_payload = struct.pack("<d", width)
    scalar_height_payload = struct.pack("<d", height)

    with zipfile.ZipFile(output_dir / "mouth_track.npz", "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("bbox.npy", make_npy(bbox_payload, "<f8", (len(frames), 4)))
        archive.writestr("valid.npy", make_npy(valid_payload, "|b1", (len(valid),)))
        archive.writestr("w.npy", make_npy(scalar_width_payload, "<f8", (1,)))
        archive.writestr("h.npy", make_npy(scalar_height_payload, "<f8", (1,)))


def write_sprite(ffmpeg: str, path: Path, mouth_height: int, color: str) -> None:
    filter_graph = (
        f"color=c=black@0.0:s=64x64:d=1,"
        f"drawbox=x=16:y={32 - mouth_height // 2}:w=32:h={mouth_height}:color={color}:t=fill,"
        "format=rgba"
    )
    run_ffmpeg(ffmpeg, "-f", "lavfi", "-i", filter_graph, "-frames:v", "1", path.as_posix())


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir).resolve()
    if output_dir.exists():
        shutil.rmtree(output_dir)
    (output_dir / "mouth").mkdir(parents=True, exist_ok=True)

    run_ffmpeg(
        args.ffmpeg,
        "-f",
        "lavfi",
        "-i",
        "testsrc2=size=64x64:rate=24:duration=2",
        "-pix_fmt",
        "yuv420p",
        (output_dir / "loop_motion.mp4").as_posix(),
    )

    write_sprite(args.ffmpeg, output_dir / "mouth" / "open.png", 18, "0xFF4040@1.0")
    write_sprite(args.ffmpeg, output_dir / "mouth" / "half.png", 12, "0xFF8040@1.0")
    write_sprite(args.ffmpeg, output_dir / "mouth" / "closed.png", 6, "0xC02020@1.0")
    write_sprite(args.ffmpeg, output_dir / "mouth" / "u.png", 16, "0x8040FF@1.0")
    write_sprite(args.ffmpeg, output_dir / "mouth" / "e.png", 14, "0x40C0FF@1.0")

    write_track_assets(output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
