import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path


class FakeImage:
    def __init__(self, payload: bytes):
        self._payload = payload

    def tobytes(self) -> bytes:
        return self._payload


class DummyClient:
    def request(self, request_type: str, request_data: dict[str, object] | None = None) -> dict[str, object]:
        return {}


def load_obs_smoke_module():
    if "websocket" not in sys.modules:
        websocket_module = types.ModuleType("websocket")
        websocket_module.WebSocket = object
        websocket_module.create_connection = lambda *args, **kwargs: None
        sys.modules["websocket"] = websocket_module

    if "PIL" not in sys.modules:
        pil_module = types.ModuleType("PIL")
        image_module = types.ModuleType("PIL.Image")
        image_module.Image = type("Image", (), {})
        imagechops_module = types.ModuleType("PIL.ImageChops")
        pil_module.Image = image_module
        pil_module.ImageChops = imagechops_module
        sys.modules["PIL"] = pil_module
        sys.modules["PIL.Image"] = image_module
        sys.modules["PIL.ImageChops"] = imagechops_module

    module_path = Path(__file__).resolve().parents[1] / "ci" / "obs-websocket-smoke.py"
    spec = importlib.util.spec_from_file_location("obs_websocket_smoke", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load smoke module from {module_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class VerifyContinuousMotionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.obs_smoke = load_obs_smoke_module()

    def run_sequence(self, payloads: list[bytes]) -> dict[str, object]:
        payload_iter = iter(payloads)
        original_capture = self.obs_smoke.capture_screenshot_exact
        original_sleep = self.obs_smoke.time.sleep

        def fake_capture(client, source_name, output_path, timeout_seconds=10.0):
            return FakeImage(next(payload_iter))

        try:
            self.obs_smoke.capture_screenshot_exact = fake_capture
            self.obs_smoke.time.sleep = lambda *_args, **_kwargs: None
            with tempfile.TemporaryDirectory() as temp_dir:
                return self.obs_smoke.verify_continuous_motion(
                    DummyClient(),
                    "Smoke Source",
                    Path(temp_dir),
                    sample_count=len(payloads),
                    interval_seconds=1.0,
                    max_same_hash_run=4,
                )
        finally:
            self.obs_smoke.capture_screenshot_exact = original_capture
            self.obs_smoke.time.sleep = original_sleep

    def test_two_frame_loop_is_not_treated_as_stall(self) -> None:
        summary = self.run_sequence([b"A", b"B"] * 5)
        self.assertEqual(summary["unique_hashes"], 2)
        self.assertEqual(summary["max_same_hash_run"], 1)

    def test_static_sequence_still_fails(self) -> None:
        with self.assertRaises(AssertionError):
            self.run_sequence([b"A"] * 6)


if __name__ == "__main__":
    unittest.main()
