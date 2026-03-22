#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

import websocket
from PIL import Image, ImageChops


SOURCE_KIND = "motionpngtuber_player"
PROP_AUDIO_DEVICE_IDENTITY = "audio_device_identity"


class ObsRequestError(RuntimeError):
    def __init__(self, request_type: str, code: int | None, comment: str | None):
        message = f"{request_type} failed"
        if code is not None:
            message += f" (code={code})"
        if comment:
            message += f": {comment}"
        super().__init__(message)
        self.request_type = request_type
        self.code = code
        self.comment = comment or ""


@dataclass
class CropSettings:
    relative: bool = True
    left: int = 48
    right: int = 24
    top: int = 24
    bottom: int = 12

    def to_request(self) -> dict[str, object]:
        return {
            "relative": self.relative,
            "left": self.left,
            "right": self.right,
            "top": self.top,
            "bottom": self.bottom,
        }


@dataclass(frozen=True)
class ContentExtent:
    width: int
    height: int


class ObsWebSocketClient:
    def __init__(self, url: str, password: str | None = None):
        self.url = url
        self.password = password
        self.ws: websocket.WebSocket | None = None

    def connect(self, timeout_seconds: float = 60.0) -> None:
        deadline = time.time() + timeout_seconds
        last_error: Exception | None = None

        while time.time() < deadline:
            try:
                ws = websocket.create_connection(self.url, timeout=30)
                hello = json.loads(ws.recv())
                if hello.get("op") != 0:
                    raise RuntimeError(f"Unexpected OBS hello opcode: {hello!r}")

                identify_data: dict[str, object] = {
                    "rpcVersion": hello.get("d", {}).get("rpcVersion", 1),
                    "eventSubscriptions": 0,
                }
                auth = hello.get("d", {}).get("authentication")
                if auth:
                    if not self.password:
                        raise RuntimeError("OBS websocket requested authentication but no password was supplied.")
                    identify_data["authentication"] = self._build_auth_token(auth["challenge"], auth["salt"])

                ws.send(json.dumps({"op": 1, "d": identify_data}))
                identified = json.loads(ws.recv())
                if identified.get("op") != 2:
                    raise RuntimeError(f"Unexpected OBS identify response: {identified!r}")

                ws.settimeout(30)
                self.ws = ws
                return
            except Exception as exc:  # connection retries are intentional here
                last_error = exc
                time.sleep(1.0)

        raise RuntimeError(f"Could not connect to OBS websocket at {self.url}: {last_error}")

    def close(self) -> None:
        if self.ws is not None:
            try:
                self.ws.close()
            finally:
                self.ws = None

    def request(self, request_type: str, request_data: dict[str, object] | None = None) -> dict[str, object]:
        if self.ws is None:
            raise RuntimeError("OBS websocket is not connected.")

        request_id = str(uuid.uuid4())
        payload: dict[str, object] = {
            "op": 6,
            "d": {
                "requestType": request_type,
                "requestId": request_id,
            },
        }
        if request_data:
            payload["d"]["requestData"] = request_data

        self.ws.send(json.dumps(payload))
        while True:
            response = json.loads(self.ws.recv())
            op = response.get("op")
            if op == 5:
                continue
            if op != 7:
                continue

            data = response.get("d", {})
            if data.get("requestId") != request_id:
                continue

            status = data.get("requestStatus", {})
            if not status.get("result", False):
                raise ObsRequestError(request_type, status.get("code"), status.get("comment"))
            return data.get("responseData", {})

    def _build_auth_token(self, challenge: str, salt: str) -> str:
        secret = base64.b64encode(hashlib.sha256((self.password + salt).encode("utf-8")).digest()).decode("utf-8")
        return base64.b64encode(hashlib.sha256((secret + challenge).encode("utf-8")).digest()).decode("utf-8")


def normalize_path(value: str) -> str:
    return os.path.normcase(os.path.normpath(value))


def wait_for_ready(client: ObsWebSocketClient, timeout_seconds: float = 60.0) -> None:
    deadline = time.time() + timeout_seconds
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            client.request("GetVersion")
            return
        except Exception as exc:  # OBS can still be starting
            last_error = exc
            time.sleep(1.0)
    raise RuntimeError(f"OBS did not become ready in time: {last_error}")


def ensure_source_kind_available(client: ObsWebSocketClient) -> None:
    response = client.request("GetInputKindList", {"unversioned": True})
    kinds = response.get("inputKinds", [])
    if SOURCE_KIND not in kinds:
        raise RuntimeError(f"{SOURCE_KIND} was not registered in OBS input kinds: {kinds}")


def is_already_exists_error(error: ObsRequestError) -> bool:
    return error.code == 601 or "already exists" in error.comment.lower()


def ensure_scene(client: ObsWebSocketClient, scene_name: str) -> None:
    try:
        client.request("CreateScene", {"sceneName": scene_name})
    except ObsRequestError as exc:
        if not is_already_exists_error(exc):
            raise
    client.request("SetCurrentProgramScene", {"sceneName": scene_name})


def get_input_settings(client: ObsWebSocketClient, source_name: str) -> dict[str, object]:
    response = client.request("GetInputSettings", {"inputName": source_name})
    settings = response.get("inputSettings")
    if not isinstance(settings, dict):
        raise RuntimeError(f"OBS did not return input settings for {source_name}: {response}")
    return settings


def get_filter_settings(client: ObsWebSocketClient, source_name: str, filter_name: str) -> dict[str, object]:
    response = client.request("GetSourceFilter", {"sourceName": source_name, "filterName": filter_name})
    settings = response.get("filterSettings")
    if not isinstance(settings, dict):
        raise RuntimeError(f"OBS did not return filter settings for {filter_name}: {response}")
    return settings


def decode_image_data(image_data: str) -> Image.Image:
    if "," in image_data:
        _, encoded = image_data.split(",", 1)
    else:
        encoded = image_data
    raw = base64.b64decode(encoded)
    image = Image.open(io.BytesIO(raw))
    image.load()
    return image


def get_nonblank_bbox(image: Image.Image) -> tuple[int, int, int, int] | None:
    rgb_image = image.convert("RGB")
    background = Image.new("RGB", rgb_image.size, rgb_image.getpixel((0, 0)))
    return ImageChops.difference(rgb_image, background).getbbox()


def get_nonblank_extent(image: Image.Image) -> ContentExtent | None:
    bbox = get_nonblank_bbox(image)
    if bbox is None:
        return None

    left, top, right, bottom = bbox
    return ContentExtent(width=right - left, height=bottom - top)


def is_nonblank(image: Image.Image) -> bool:
    return get_nonblank_bbox(image) is not None


def get_image_difference_bbox(before: Image.Image, after: Image.Image) -> tuple[int, int, int, int] | None:
    if before.size != after.size:
        return (0, 0, max(before.width, after.width), max(before.height, after.height))

    difference = ImageChops.difference(before.convert("RGBA"), after.convert("RGBA"))
    return difference.getbbox()


def capture_screenshot(
    client: ObsWebSocketClient,
    source_name: str,
    output_path: Path,
    timeout_seconds: float = 30.0,
) -> Image.Image:
    deadline = time.time() + timeout_seconds
    last_image: Image.Image | None = None

    while time.time() < deadline:
        response = client.request(
            "GetSourceScreenshot",
            {
                "sourceName": source_name,
                "imageFormat": "png",
            },
        )
        image_data = response.get("imageData")
        if not isinstance(image_data, str) or not image_data:
            time.sleep(1.0)
            continue

        image = decode_image_data(image_data)
        if image.width > 0 and image.height > 0 and is_nonblank(image):
            output_path.parent.mkdir(parents=True, exist_ok=True)
            image.save(output_path)
            return image

        last_image = image
        time.sleep(1.0)

    if last_image is not None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        last_image.save(output_path)
    raise RuntimeError(f"Did not receive a nonblank screenshot for {source_name}")


def capture_changed_screenshot(
    client: ObsWebSocketClient,
    source_name: str,
    baseline_image: Image.Image,
    output_path: Path,
    timeout_seconds: float = 30.0,
) -> Image.Image:
    deadline = time.time() + timeout_seconds
    last_image: Image.Image | None = None

    while time.time() < deadline:
        remaining_timeout = max(0.1, deadline - time.time())
        current_image = capture_screenshot(
            client,
            source_name,
            output_path,
            timeout_seconds=min(5.0, remaining_timeout),
        )
        if get_image_difference_bbox(baseline_image, current_image) is not None:
            return current_image

        last_image = current_image
        time.sleep(1.0)

    if last_image is not None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        last_image.save(output_path)
    raise AssertionError("crop filter should visibly change the rendered scene output")


def assert_equal(actual: object, expected: object, description: str) -> None:
    if actual != expected:
        raise AssertionError(f"{description}: expected {expected!r}, got {actual!r}")


def assert_true(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def run_create_phase(args: argparse.Namespace) -> dict[str, object]:
    asset_root = Path(args.asset_dir)
    artifacts_dir = Path(args.artifacts_dir)
    loop_video = asset_root / "loop.mp4"
    mouth_dir = asset_root / "mouth"
    track_file = asset_root / "mouth_track.json"
    crop = CropSettings()

    client = ObsWebSocketClient(args.url, args.password)
    client.connect()
    try:
        wait_for_ready(client)
        ensure_source_kind_available(client)
        ensure_scene(client, args.scene_name)

        client.request(
            "CreateInput",
            {
                "sceneName": args.scene_name,
                "inputName": args.source_name,
                "inputKind": SOURCE_KIND,
                "inputSettings": {
                    "loop_video": str(loop_video),
                    "render_fps": 18,
                },
                "sceneItemEnabled": True,
            },
        )
        client.request("SetCurrentProgramScene", {"sceneName": args.scene_name})

        time.sleep(2.0)
        input_settings = get_input_settings(client, args.source_name)
        assert_equal(normalize_path(str(loop_video)), normalize_path(str(input_settings.get("loop_video", ""))), "loop_video")
        assert_equal(normalize_path(str(mouth_dir)), normalize_path(str(input_settings.get("mouth_dir", ""))), "mouth_dir auto-fill")
        assert_equal(normalize_path(str(track_file)), normalize_path(str(input_settings.get("track_file", ""))), "track_file auto-fill")
        assert_equal(int(input_settings.get("render_fps", 0)), 18, "initial render_fps")

        property_items = client.request(
            "GetInputPropertiesListPropertyItems",
            {
                "inputName": args.source_name,
                "propertyName": PROP_AUDIO_DEVICE_IDENTITY,
            },
        ).get("propertyItems", [])
        assert_true(bool(property_items), "audio device list should contain at least one entry")

        before_filter = capture_screenshot(client, args.scene_name, artifacts_dir / "before-filter.png")
        before_filter_extent = get_nonblank_extent(before_filter)
        assert_true(before_filter_extent is not None, "before-filter screenshot should contain visible content")
        client.request(
            "CreateSourceFilter",
            {
                "sourceName": args.source_name,
                "filterName": args.filter_name,
                "filterKind": "crop_filter",
                "filterSettings": crop.to_request(),
            },
        )

        time.sleep(1.0)
        filter_settings = get_filter_settings(client, args.source_name, args.filter_name)
        assert_equal(int(filter_settings.get("left", 0)), crop.left, "crop_filter left")
        assert_equal(int(filter_settings.get("right", 0)), crop.right, "crop_filter right")
        assert_equal(int(filter_settings.get("top", 0)), crop.top, "crop_filter top")
        assert_equal(int(filter_settings.get("bottom", 0)), crop.bottom, "crop_filter bottom")

        # The smoke video is static, so the rendered scene should change as soon as the crop filter takes effect.
        after_filter = capture_changed_screenshot(
            client,
            args.scene_name,
            before_filter,
            artifacts_dir / "after-filter.png",
        )
        after_filter_extent = get_nonblank_extent(after_filter)
        assert_true(after_filter_extent is not None, "after-filter screenshot should contain visible content")
        difference_bbox = get_image_difference_bbox(before_filter, after_filter)
        assert_true(difference_bbox is not None, "crop filter should visibly change the rendered scene output")

        client.request(
            "SetInputSettings",
            {
                "inputName": args.source_name,
                "inputSettings": {
                    "render_fps": 24,
                    "valid_policy": "strict",
                },
                "overlay": True,
            },
        )
        time.sleep(1.0)

        updated_settings = get_input_settings(client, args.source_name)
        assert_equal(int(updated_settings.get("render_fps", 0)), 24, "updated render_fps")
        assert_equal(str(updated_settings.get("valid_policy", "")), "strict", "updated valid_policy")

        summary = {
            "before_filter_size": list(before_filter.size),
            "after_filter_size": list(after_filter.size),
            "before_filter_content_size": [before_filter_extent.width, before_filter_extent.height],
            "after_filter_content_size": [after_filter_extent.width, after_filter_extent.height],
            "filter_difference_bbox": list(difference_bbox) if difference_bbox is not None else None,
            "audio_device_items": len(property_items),
        }
        (artifacts_dir / "summary-create.json").write_text(json.dumps(summary, indent=2), encoding="utf-8", newline="\n")
        time.sleep(3.0)
        return summary
    finally:
        client.close()


def run_reopen_phase(args: argparse.Namespace) -> dict[str, object]:
    asset_root = Path(args.asset_dir)
    artifacts_dir = Path(args.artifacts_dir)
    loop_video = asset_root / "loop.mp4"
    mouth_dir = asset_root / "mouth"
    track_file = asset_root / "mouth_track.json"
    crop = CropSettings()

    client = ObsWebSocketClient(args.url, args.password)
    client.connect()
    try:
        wait_for_ready(client)
        ensure_source_kind_available(client)
        ensure_scene(client, args.scene_name)

        inputs = client.request("GetInputList", {"inputKind": SOURCE_KIND}).get("inputs", [])
        source_names = [entry.get("inputName") for entry in inputs if isinstance(entry, dict)]
        assert_true(args.source_name in source_names, f"{args.source_name} should still exist after OBS restart")

        settings = get_input_settings(client, args.source_name)
        assert_equal(normalize_path(str(loop_video)), normalize_path(str(settings.get("loop_video", ""))), "persisted loop_video")
        assert_equal(normalize_path(str(mouth_dir)), normalize_path(str(settings.get("mouth_dir", ""))), "persisted mouth_dir")
        assert_equal(normalize_path(str(track_file)), normalize_path(str(settings.get("track_file", ""))), "persisted track_file")
        assert_equal(int(settings.get("render_fps", 0)), 24, "persisted render_fps")
        assert_equal(str(settings.get("valid_policy", "")), "strict", "persisted valid_policy")

        filters = client.request("GetSourceFilterList", {"sourceName": args.source_name}).get("filters", [])
        filter_names = [entry.get("filterName") for entry in filters if isinstance(entry, dict)]
        assert_true(args.filter_name in filter_names, f"{args.filter_name} should still exist after OBS restart")

        filter_settings = get_filter_settings(client, args.source_name, args.filter_name)
        assert_equal(int(filter_settings.get("left", 0)), crop.left, "persisted crop_filter left")
        assert_equal(int(filter_settings.get("right", 0)), crop.right, "persisted crop_filter right")
        assert_equal(int(filter_settings.get("top", 0)), crop.top, "persisted crop_filter top")
        assert_equal(int(filter_settings.get("bottom", 0)), crop.bottom, "persisted crop_filter bottom")

        restarted_image = capture_screenshot(client, args.source_name, artifacts_dir / "after-restart.png")
        assert_true(restarted_image.width > 0 and restarted_image.height > 0, "restart screenshot must be non-empty")

        summary = {
            "after_restart_size": list(restarted_image.size),
        }
        (artifacts_dir / "summary-reopen.json").write_text(json.dumps(summary, indent=2), encoding="utf-8", newline="\n")
        return summary
    finally:
        client.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run MotionPngTuberPlayer OBS websocket smoke tests.")
    parser.add_argument("--phase", choices=("create", "reopen"), required=True)
    parser.add_argument("--asset-dir", required=True)
    parser.add_argument("--artifacts-dir", required=True)
    parser.add_argument("--url", default="ws://127.0.0.1:4455")
    parser.add_argument("--password")
    parser.add_argument("--scene-collection-name", default="MotionPngTuberPlayer Smoke")
    parser.add_argument("--scene-name", default="MotionPngTuberPlayer Scene")
    parser.add_argument("--source-name", default="MotionPngTuberPlayer Smoke Source")
    parser.add_argument("--filter-name", default="MotionPngTuberPlayer Crop Filter")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    Path(args.artifacts_dir).mkdir(parents=True, exist_ok=True)

    if args.phase == "create":
        summary = run_create_phase(args)
    else:
        summary = run_reopen_phase(args)

    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
