from __future__ import annotations

import argparse
from dataclasses import asdict
import json
from pathlib import Path
import sys
import threading
import time

import cv2

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from runtime import AudioInputService, ControlServer, HeadlessRunner, RuntimeConfig, SharedFrameBufferWriter, default_frame_buffer_path


def _load_config(path: Path) -> RuntimeConfig:
    with path.open("r", encoding="utf-8") as handle:
        raw = json.load(handle)
    if not isinstance(raw, dict):
        raise ValueError("worker config must be a JSON object")
    return RuntimeConfig.from_dict(raw)


def _write_frame(path: Path, frame) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(path), cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)):
        raise RuntimeError(f"failed to write frame: {path}")


def run_once(config_path: Path, write_frame: Path | None = None) -> None:
    config = _load_config(config_path)
    runner = HeadlessRunner(config)
    try:
        frame = runner.render_frame()
        if write_frame is not None:
            _write_frame(write_frame, frame)
        print(json.dumps({"ok": True, "status": asdict(runner.status_snapshot())}), flush=True)
    finally:
        runner.close()


def list_devices() -> None:
    service = AudioInputService()
    devices = []
    for device in service.list_input_devices():
        identity = device.to_dict()
        item = dict(identity)
        item["display_name"] = device.display_name
        item["identity_json"] = json.dumps(identity, separators=(",", ":"))
        devices.append(item)
    print(json.dumps({"ok": True, "devices": devices}), flush=True)


def serve(config_path: Path, host: str, port: int, *, frame_buffer_path: Path | None = None) -> None:
    config = _load_config(config_path)
    runner = HeadlessRunner(config)
    shutdown_event = threading.Event()
    frame_writer = SharedFrameBufferWriter(
        frame_buffer_path or default_frame_buffer_path("motionpngtuber-player"),
        runner.full_w,
        runner.full_h,
    )
    render_state: dict[str, object] = {
        "metadata": None,
        "last_render_error": None,
    }

    def render_loop() -> None:
        frame_interval = 1.0 / float(max(1, config.render_fps))
        next_frame_t = time.perf_counter()
        while not shutdown_event.is_set():
            now = time.perf_counter()
            frame = runner.render_frame(now)
            timestamp_ns = time.time_ns()
            try:
                metadata = frame_writer.write_rgb_frame(frame, timestamp_ns=timestamp_ns)
                render_state["metadata"] = metadata
                render_state["last_render_error"] = None
            except Exception as exc:
                render_state["last_render_error"] = str(exc)

            next_frame_t += frame_interval
            sleep_s = next_frame_t - time.perf_counter()
            if sleep_s > 0:
                shutdown_event.wait(sleep_s)
            else:
                next_frame_t = time.perf_counter()

    def handler(message: dict[str, object]) -> dict[str, object]:
        kind = str(message.get("type", "") or "").strip().lower()
        if kind == "ping":
            return {"type": "pong"}
        if kind == "get_status":
            metadata = render_state.get("metadata")
            return {
                "type": "status",
                "status": asdict(runner.status_snapshot()),
                "frame_buffer_path": str(frame_writer.path),
                "frame_metadata": asdict(metadata) if metadata is not None else None,
                "render_error": render_state.get("last_render_error"),
            }
        if kind == "render_once":
            frame = runner.render_frame()
            output_path_raw = message.get("output_path")
            if isinstance(output_path_raw, str) and output_path_raw.strip():
                _write_frame(Path(output_path_raw), frame)
            metadata = frame_writer.write_rgb_frame(frame, timestamp_ns=time.time_ns())
            render_state["metadata"] = metadata
            return {
                "type": "rendered",
                "frame": {
                    "width": int(frame.shape[1]),
                    "height": int(frame.shape[0]),
                    "channels": int(frame.shape[2]),
                },
                "frame_buffer_path": str(frame_writer.path),
                "frame_metadata": asdict(metadata),
                "status": asdict(runner.status_snapshot()),
            }
        if kind == "shutdown":
            shutdown_event.set()
            return {"type": "shutdown_ack"}
        return {"ok": False, "error": f"unsupported_message:{kind or 'missing'}"}

    with ControlServer(handler, host=host, port=port) as server:
        runner.start()
        render_thread = threading.Thread(target=render_loop, daemon=True)
        render_thread.start()
        server_host, server_port = server.address
        print(
            json.dumps(
                {
                    "ok": True,
                    "type": "listening",
                    "host": server_host,
                    "port": server_port,
                    "frame_buffer_path": str(frame_writer.path),
                }
            ),
            flush=True,
        )
        try:
            while not shutdown_event.wait(0.2):
                pass
        finally:
            render_thread.join(timeout=1.0)
            runner.close()
            frame_writer.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config-json", type=Path, default=None)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--write-frame", type=Path, default=None)
    parser.add_argument("--frame-buffer", type=Path, default=None)
    args = parser.parse_args()
    if not args.list_devices and args.config_json is None:
        parser.error("--config-json is required unless --list-devices is used")
    return args


def main() -> None:
    args = parse_args()
    if args.list_devices:
        list_devices()
        return
    if args.once:
        run_once(args.config_json, write_frame=args.write_frame)
        return
    serve(args.config_json, args.host, args.port, frame_buffer_path=args.frame_buffer)


if __name__ == "__main__":
    main()
