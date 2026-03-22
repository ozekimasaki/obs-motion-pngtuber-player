from __future__ import annotations

from collections.abc import Mapping

try:
    import sounddevice as sd
except Exception:  # pragma: no cover - environment dependent
    sd = None  # type: ignore[assignment]

from .device_identity import DeviceIdentity


class AudioInputService:
    def _require_sounddevice(self) -> None:
        if sd is None:  # pragma: no cover - environment dependent
            raise RuntimeError("sounddevice is not available")

    def _hostapi_names(self) -> dict[int, str]:
        self._require_sounddevice()
        names: dict[int, str] = {}
        try:
            hostapis = sd.query_hostapis()
        except Exception:
            return names
        if isinstance(hostapis, Mapping):
            hostapis = [hostapis]
        for idx, raw in enumerate(hostapis):
            if isinstance(raw, Mapping):
                names[idx] = str(raw.get("name", "") or "").strip()
        return names

    def list_input_devices(self) -> list[DeviceIdentity]:
        self._require_sounddevice()
        hostapi_names = self._hostapi_names()
        devices = sd.query_devices()
        if isinstance(devices, Mapping):
            devices = [devices]

        items: list[DeviceIdentity] = []
        for idx, raw in enumerate(devices):
            if not isinstance(raw, Mapping):
                continue
            if int(raw.get("max_input_channels", 0) or 0) <= 0:
                continue
            hostapi_index = raw.get("hostapi")
            hostapi_name = None
            if isinstance(hostapi_index, int):
                hostapi_name = hostapi_names.get(hostapi_index)
            items.append(DeviceIdentity.from_sounddevice(idx, raw, hostapi_name))
        return items

    def resolve_device(
        self,
        preferred_identity: DeviceIdentity | None = None,
        preferred_index: int | None = None,
    ) -> DeviceIdentity | None:
        devices = self.list_input_devices()
        if not devices:
            return None

        if preferred_identity is not None:
            for device in devices:
                if preferred_identity.matches(device):
                    return device

        if preferred_index is not None:
            for device in devices:
                if device.index == preferred_index:
                    return device

        return devices[0]

    @staticmethod
    def samplerate_for(device: DeviceIdentity | None) -> int:
        if device is None:
            return 48000
        samplerate = int(device.default_samplerate or 0)
        return samplerate if samplerate > 0 else 48000
