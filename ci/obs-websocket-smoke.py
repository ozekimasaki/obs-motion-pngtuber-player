#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
import platform
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

import websocket
from PIL import Image, ImageChops


SOURCE_KIND = "motionpngtuber_player"
PROP_AUDIO_DEVICE_IDENTITY = "audio_device_identity"
SMOKE_CAPTURE_WIDTH = 320
SMOKE_CAPTURE_HEIGHT = 240
SCENE_COLLECTION_SAVE_SUFFIX = " Save Checkpoint"
CONTROL_SCENE_SUFFIX = " Control Scene"
CONTROL_IMAGE_SUFFIX = " Control Image"


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


@dataclass(frozen=True)
class SourceActivityState:
    active: bool
    showing: bool


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


def get_scene_names(client: ObsWebSocketClient) -> list[str]:
    response = client.request("GetSceneList")
    scenes = response.get("scenes", [])
    names: list[str] = []
    if isinstance(scenes, list):
        for entry in scenes:
            if not isinstance(entry, dict):
                continue
            scene_name = entry.get("sceneName")
            if isinstance(scene_name, str):
                names.append(scene_name)
    return names


def select_existing_scene(client: ObsWebSocketClient, scene_name: str) -> None:
    scene_names = get_scene_names(client)
    assert_true(scene_name in scene_names, f"{scene_name} should still exist after OBS restart")
    client.request("SetCurrentProgramScene", {"sceneName": scene_name})


def get_scene_collection_state(client: ObsWebSocketClient) -> tuple[str | None, list[str]]:
    response = client.request("GetSceneCollectionList")
    current = response.get("currentSceneCollectionName")
    current_name = current if isinstance(current, str) else None
    raw_collections = response.get("sceneCollections", [])
    collection_names: list[str] = []

    if isinstance(raw_collections, list):
        for entry in raw_collections:
            if isinstance(entry, str):
                collection_names.append(entry)
                continue
            if not isinstance(entry, dict):
                continue
            for key in ("sceneCollectionName", "sceneCollection", "name"):
                value = entry.get(key)
                if isinstance(value, str):
                    collection_names.append(value)
                    break

    if current_name and current_name not in collection_names:
        collection_names.append(current_name)
    return current_name, collection_names


def ensure_scene_collection(
    client: ObsWebSocketClient,
    scene_collection_name: str,
    *,
    create_if_missing: bool,
) -> None:
    current_name, collection_names = get_scene_collection_state(client)
    if current_name == scene_collection_name:
        return

    if scene_collection_name not in collection_names:
        if not create_if_missing:
            raise AssertionError(f"{scene_collection_name} should still exist after OBS restart")
        try:
            client.request("CreateSceneCollection", {"sceneCollectionName": scene_collection_name})
        except ObsRequestError as exc:
            if not is_already_exists_error(exc):
                raise
        wait_for_ready(client)

    current_name, _ = get_scene_collection_state(client)
    if current_name != scene_collection_name:
        client.request("SetCurrentSceneCollection", {"sceneCollectionName": scene_collection_name})
        wait_for_ready(client)


def force_save_scene_collection(client: ObsWebSocketClient, scene_collection_name: str) -> None:
    ensure_scene_collection(client, scene_collection_name, create_if_missing=False)
    _, collection_names = get_scene_collection_state(client)
    checkpoint_name = f"{scene_collection_name}{SCENE_COLLECTION_SAVE_SUFFIX}"

    if checkpoint_name not in collection_names:
        try:
            client.request("CreateSceneCollection", {"sceneCollectionName": checkpoint_name})
        except ObsRequestError as exc:
            if not is_already_exists_error(exc):
                raise
        wait_for_ready(client)

    current_name, _ = get_scene_collection_state(client)
    if current_name != checkpoint_name:
        client.request("SetCurrentSceneCollection", {"sceneCollectionName": checkpoint_name})
        wait_for_ready(client)

    client.request("SetCurrentSceneCollection", {"sceneCollectionName": scene_collection_name})
    wait_for_ready(client)


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


def ensure_input(
    client: ObsWebSocketClient,
    scene_name: str,
    input_name: str,
    input_kind: str,
    input_settings: dict[str, object],
) -> None:
    try:
        client.request(
            "CreateInput",
            {
                "sceneName": scene_name,
                "inputName": input_name,
                "inputKind": input_kind,
                "inputSettings": input_settings,
                "sceneItemEnabled": True,
            },
        )
    except ObsRequestError as exc:
        if not is_already_exists_error(exc):
            raise


def get_source_activity_state(client: ObsWebSocketClient, source_name: str) -> SourceActivityState:
    response = client.request("GetSourceActive", {"sourceName": source_name})

    active = response.get("videoActive")
    if not isinstance(active, bool):
        active = response.get("active")
    showing = response.get("videoShowing")
    if not isinstance(showing, bool):
        showing = response.get("show")

    if not isinstance(active, bool) or not isinstance(showing, bool):
        raise RuntimeError(f"OBS did not return source activity state for {source_name}: {response}")
    return SourceActivityState(active=active, showing=showing)


def probe_scene_screenshot_support(
    client: ObsWebSocketClient,
    base_name: str,
    image_path: Path,
    artifacts_dir: Path,
) -> tuple[bool, str | None]:
    if platform.system() != "Darwin":
        return True, None

    control_scene_name = f"{base_name}{CONTROL_SCENE_SUFFIX}"
    control_image_name = f"{base_name}{CONTROL_IMAGE_SUFFIX}"
    ensure_scene(client, control_scene_name)
    ensure_input(
        client,
        control_scene_name,
        control_image_name,
        "image_source",
        {"file": str(image_path)},
    )
    client.request("SetCurrentProgramScene", {"sceneName": control_scene_name})
    time.sleep(1.0)

    try:
        capture_screenshot(
            client,
            [control_scene_name],
            artifacts_dir / "control-scene.png",
            timeout_seconds=10.0,
        )
        return True, None
    except RuntimeError as exc:
        return False, str(exc)


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


def capture_screenshot_exact(
    client: ObsWebSocketClient,
    source_name: str,
    output_path: Path,
    timeout_seconds: float = 30.0,
) -> Image.Image:
    deadline = time.time() + timeout_seconds
    last_image: Image.Image | None = None
    last_error: Exception | None = None

    while time.time() < deadline:
        try:
            response = client.request(
                "GetSourceScreenshot",
                {
                    "sourceName": source_name,
                    "imageFormat": "png",
                    "imageWidth": SMOKE_CAPTURE_WIDTH,
                    "imageHeight": SMOKE_CAPTURE_HEIGHT,
                },
            )
        except ObsRequestError as exc:
            if exc.request_type == "GetSourceScreenshot" and exc.code == 702:
                last_error = exc
                time.sleep(1.0)
                continue
            raise
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
    if last_error is not None:
        raise RuntimeError(f"Did not receive a renderable screenshot for {source_name}: {last_error}")
    raise RuntimeError(f"Did not receive a nonblank screenshot for {source_name}")


def capture_screenshot(
    client: ObsWebSocketClient,
    source_names: list[str],
    output_path: Path,
    timeout_seconds: float = 30.0,
) -> tuple[Image.Image, str]:
    errors: list[str] = []
    for source_name in source_names:
        try:
            return capture_screenshot_exact(client, source_name, output_path, timeout_seconds), source_name
        except RuntimeError as exc:
            errors.append(f"{source_name}: {exc}")
    raise RuntimeError("; ".join(errors))


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
        current_image = capture_screenshot_exact(
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


def should_skip_visual_verification(
    visual_verification_supported: bool,
) -> bool:
    return platform.system() == "Darwin" and not visual_verification_supported


def run_create_phase(args: argparse.Namespace) -> dict[str, object]:
    asset_root = Path(args.asset_dir)
    artifacts_dir = Path(args.artifacts_dir)
    loop_video = asset_root / "loop.mp4"
    mouth_dir = asset_root / "mouth"
    control_image = mouth_dir / "open.png"
    auto_filled_track_file = asset_root / "mouth_track.json"
    track_file = asset_root / "mouth_track.npz"
    crop = CropSettings()

    client = ObsWebSocketClient(args.url, args.password)
    client.connect()
    try:
        wait_for_ready(client)
        ensure_scene_collection(client, args.scene_collection_name, create_if_missing=True)
        ensure_source_kind_available(client)
        visual_verification_supported, visual_skip_reason = probe_scene_screenshot_support(
            client,
            args.scene_name,
            control_image,
            artifacts_dir,
        )
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
        assert_equal(
            normalize_path(str(auto_filled_track_file)),
            normalize_path(str(input_settings.get("track_file", ""))),
            "track_file auto-fill",
        )
        assert_equal(int(input_settings.get("render_fps", 0)), 18, "initial render_fps")
        source_activity = get_source_activity_state(client, args.source_name)
        scene_activity = get_source_activity_state(client, args.scene_name)
        skip_visual_verification = should_skip_visual_verification(visual_verification_supported)

        client.request(
            "SetInputSettings",
            {
                "inputName": args.source_name,
                "inputSettings": {
                    "track_file": str(track_file),
                },
                "overlay": True,
            },
        )
        time.sleep(1.0)
        input_settings = get_input_settings(client, args.source_name)
        assert_equal(normalize_path(str(track_file)), normalize_path(str(input_settings.get("track_file", ""))), "track_file npz")

        property_items = client.request(
            "GetInputPropertiesListPropertyItems",
            {
                "inputName": args.source_name,
                "propertyName": PROP_AUDIO_DEVICE_IDENTITY,
            },
        ).get("propertyItems", [])
        assert_true(bool(property_items), "audio device list should contain at least one entry")

        before_filter: Image.Image | None = None
        after_filter: Image.Image | None = None
        before_filter_extent: ContentExtent | None = None
        after_filter_extent: ContentExtent | None = None
        difference_bbox: tuple[int, int, int, int] | None = None
        capture_target: str | None = None

        if not skip_visual_verification:
            # Crop validation must use the rendered scene output, not a raw source capture.
            before_filter, capture_target = capture_screenshot(
                client,
                [args.scene_name],
                artifacts_dir / "before-filter.png",
            )
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

        if not skip_visual_verification:
            # The smoke video is static, so the rendered scene should change as soon as the crop filter takes effect.
            assert_true(before_filter is not None, "before-filter screenshot should be captured before filter comparison")
            assert_true(capture_target is not None, "capture target should be known before filter comparison")
            after_filter = capture_changed_screenshot(
                client,
                capture_target,
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

        force_save_scene_collection(client, args.scene_collection_name)
        reloaded_settings = get_input_settings(client, args.source_name)
        assert_equal(int(reloaded_settings.get("render_fps", 0)), 24, "reloaded render_fps")
        assert_equal(str(reloaded_settings.get("valid_policy", "")), "strict", "reloaded valid_policy")
        reloaded_filter_settings = get_filter_settings(client, args.source_name, args.filter_name)
        assert_equal(int(reloaded_filter_settings.get("left", 0)), crop.left, "reloaded crop_filter left")
        assert_equal(int(reloaded_filter_settings.get("right", 0)), crop.right, "reloaded crop_filter right")
        assert_equal(int(reloaded_filter_settings.get("top", 0)), crop.top, "reloaded crop_filter top")
        assert_equal(int(reloaded_filter_settings.get("bottom", 0)), crop.bottom, "reloaded crop_filter bottom")

        summary = {
            "before_filter_size": list(before_filter.size) if before_filter is not None else None,
            "after_filter_size": list(after_filter.size) if after_filter is not None else None,
            "before_filter_content_size": [before_filter_extent.width, before_filter_extent.height]
            if before_filter_extent is not None
            else None,
            "after_filter_content_size": [after_filter_extent.width, after_filter_extent.height]
            if after_filter_extent is not None
            else None,
            "filter_difference_bbox": list(difference_bbox) if difference_bbox is not None else None,
            "audio_device_items": len(property_items),
            "capture_target": capture_target,
            "source_active": source_activity.active,
            "source_showing": source_activity.showing,
            "scene_active": scene_activity.active,
            "scene_showing": scene_activity.showing,
            "visual_verification_supported": visual_verification_supported,
            "visual_verification_skipped": skip_visual_verification,
            "visual_skip_reason": visual_skip_reason,
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
    control_image = mouth_dir / "open.png"
    track_file = asset_root / "mouth_track.npz"
    crop = CropSettings()

    client = ObsWebSocketClient(args.url, args.password)
    client.connect()
    try:
        wait_for_ready(client)
        ensure_scene_collection(client, args.scene_collection_name, create_if_missing=False)
        ensure_source_kind_available(client)
        visual_verification_supported, visual_skip_reason = probe_scene_screenshot_support(
            client,
            args.scene_name,
            control_image,
            artifacts_dir,
        )
        select_existing_scene(client, args.scene_name)

        inputs = client.request("GetInputList", {"inputKind": SOURCE_KIND}).get("inputs", [])
        source_names = [entry.get("inputName") for entry in inputs if isinstance(entry, dict)]
        assert_true(args.source_name in source_names, f"{args.source_name} should still exist after OBS restart")
        source_activity = get_source_activity_state(client, args.source_name)
        scene_activity = get_source_activity_state(client, args.scene_name)
        skip_visual_verification = should_skip_visual_verification(visual_verification_supported)

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

        restarted_image: Image.Image | None = None
        restart_capture_target: str | None = None
        if not skip_visual_verification:
            restarted_image, restart_capture_target = capture_screenshot(
                client,
                [args.scene_name],
                artifacts_dir / "after-restart.png",
            )
            assert_true(restarted_image.width > 0 and restarted_image.height > 0, "restart screenshot must be non-empty")

        summary = {
            "after_restart_size": list(restarted_image.size) if restarted_image is not None else None,
            "capture_target": restart_capture_target,
            "source_active": source_activity.active,
            "source_showing": source_activity.showing,
            "scene_active": scene_activity.active,
            "scene_showing": scene_activity.showing,
            "visual_verification_supported": visual_verification_supported,
            "visual_verification_skipped": skip_visual_verification,
            "visual_skip_reason": visual_skip_reason,
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
