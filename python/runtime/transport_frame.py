from __future__ import annotations

from dataclasses import dataclass
import mmap
import os
from pathlib import Path
import struct
import tempfile
from typing import BinaryIO

import cv2
import numpy as np


FRAME_MAGIC = b"MPTFRAME"
FRAME_VERSION = 1
PIXEL_FORMAT_BGRA32 = 1
FRAME_STATUS_WRITING = 1
HEADER_STRUCT = struct.Struct("<8sIIIIIQQIQ8x")
HEADER_SIZE = HEADER_STRUCT.size


@dataclass(slots=True, frozen=True)
class FrameMetadata:
    width: int
    height: int
    stride: int
    pixel_format: str
    frame_id: int
    timestamp_ns: int
    status_flags: int = 0

    @property
    def payload_bytes(self) -> int:
        return int(self.stride) * int(self.height)


def _pixel_format_to_code(pixel_format: str) -> int:
    if pixel_format.upper() == "BGRA32":
        return PIXEL_FORMAT_BGRA32
    raise ValueError(f"unsupported pixel format: {pixel_format}")


def _pixel_format_from_code(code: int) -> str:
    if int(code) == PIXEL_FORMAT_BGRA32:
        return "BGRA32"
    raise ValueError(f"unsupported pixel format code: {code}")


def default_frame_buffer_path(name: str) -> Path:
    base = Path(tempfile.gettempdir()) / "motionpngtuber"
    base.mkdir(parents=True, exist_ok=True)
    return base / f"{name}.framebuffer"


class SharedFrameBufferWriter:
    def __init__(self, path: str | os.PathLike[str], width: int, height: int, *, pixel_format: str = "BGRA32") -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.width = int(width)
        self.height = int(height)
        self.pixel_format = pixel_format
        self.stride = self.width * 4
        self.payload_bytes = self.stride * self.height
        self.total_bytes = HEADER_SIZE + self.payload_bytes
        self._file: BinaryIO = open(self.path, "w+b")
        self._file.truncate(self.total_bytes)
        self._mmap = mmap.mmap(self._file.fileno(), self.total_bytes)
        self._frame_id = 0
        self._write_header(timestamp_ns=0, status_flags=0)

    @classmethod
    def create_temp(cls, name: str, width: int, height: int, *, pixel_format: str = "BGRA32") -> "SharedFrameBufferWriter":
        return cls(default_frame_buffer_path(name), width, height, pixel_format=pixel_format)

    def _write_header(self, *, timestamp_ns: int, status_flags: int) -> None:
        payload = HEADER_STRUCT.pack(
            FRAME_MAGIC,
            FRAME_VERSION,
            self.width,
            self.height,
            self.stride,
            _pixel_format_to_code(self.pixel_format),
            self._frame_id,
            int(timestamp_ns),
            int(status_flags),
            self.payload_bytes,
        )
        self._mmap.seek(0)
        self._mmap.write(payload)

    def write_rgb_frame(self, frame_rgb: np.ndarray, *, timestamp_ns: int, status_flags: int = 0) -> FrameMetadata:
        if frame_rgb.ndim != 3 or frame_rgb.shape[2] != 3:
            raise ValueError("frame_rgb must have shape (H, W, 3)")
        if frame_rgb.shape[0] != self.height or frame_rgb.shape[1] != self.width:
            raise ValueError(
                f"frame size mismatch: expected {self.height}x{self.width}, got {frame_rgb.shape[0]}x{frame_rgb.shape[1]}"
        )

        frame_bgra = cv2.cvtColor(frame_rgb, cv2.COLOR_RGB2BGRA)
        self._frame_id += 1
        pending_status_flags = int(status_flags) | FRAME_STATUS_WRITING
        self._write_header(timestamp_ns=timestamp_ns, status_flags=pending_status_flags)
        self._mmap.seek(HEADER_SIZE)
        self._mmap.write(frame_bgra.tobytes(order="C"))
        self._write_header(timestamp_ns=timestamp_ns, status_flags=status_flags)
        self._mmap.flush()
        return FrameMetadata(
            width=self.width,
            height=self.height,
            stride=self.stride,
            pixel_format=self.pixel_format,
            frame_id=self._frame_id,
            timestamp_ns=int(timestamp_ns),
            status_flags=int(status_flags),
        )

    def close(self, *, remove: bool = False) -> None:
        try:
            self._mmap.close()
        finally:
            self._file.close()
        if remove:
            self.path.unlink(missing_ok=True)


class SharedFrameBufferReader:
    def __init__(self, path: str | os.PathLike[str]) -> None:
        self.path = Path(path)
        self._file: BinaryIO = open(self.path, "r+b")
        self._mmap = mmap.mmap(self._file.fileno(), 0)

    def read_metadata(self) -> FrameMetadata:
        self._mmap.seek(0)
        raw = self._mmap.read(HEADER_SIZE)
        magic, version, width, height, stride, pixel_format_code, frame_id, timestamp_ns, status_flags, _payload_bytes = HEADER_STRUCT.unpack(raw)
        if magic != FRAME_MAGIC:
            raise RuntimeError(f"invalid frame magic: {magic!r}")
        if version != FRAME_VERSION:
            raise RuntimeError(f"unsupported frame version: {version}")
        return FrameMetadata(
            width=int(width),
            height=int(height),
            stride=int(stride),
            pixel_format=_pixel_format_from_code(pixel_format_code),
            frame_id=int(frame_id),
            timestamp_ns=int(timestamp_ns),
            status_flags=int(status_flags),
        )

    def read_bgra_frame(self) -> tuple[FrameMetadata, np.ndarray]:
        metadata = self.read_metadata()
        if metadata.status_flags & FRAME_STATUS_WRITING:
            raise RuntimeError("frame buffer is being written")
        self._mmap.seek(HEADER_SIZE)
        payload = self._mmap.read(metadata.payload_bytes)
        metadata_after = self.read_metadata()
        if (
            metadata_after.frame_id != metadata.frame_id
            or metadata_after.timestamp_ns != metadata.timestamp_ns
            or metadata_after.status_flags != metadata.status_flags
        ):
            raise RuntimeError("frame buffer changed during read")
        frame = np.frombuffer(payload, dtype=np.uint8).reshape((metadata.height, metadata.stride // 4, 4)).copy()
        return metadata, frame

    def close(self) -> None:
        try:
            self._mmap.close()
        finally:
            self._file.close()
