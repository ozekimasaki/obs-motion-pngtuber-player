from __future__ import annotations

from collections.abc import Callable, Mapping
import json
import socket
import threading
from typing import Any


JsonObject = dict[str, Any]
MessageHandler = Callable[[JsonObject], JsonObject | None]


def _json_dumps(payload: Mapping[str, object]) -> bytes:
    return (json.dumps(dict(payload), ensure_ascii=False) + "\n").encode("utf-8")


class ControlServer:
    def __init__(
        self,
        handler: MessageHandler,
        *,
        host: str = "127.0.0.1",
        port: int = 0,
        backlog: int = 1,
    ) -> None:
        self._handler = handler
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((host, int(port)))
        self._sock.listen(max(1, backlog))
        self._sock.settimeout(0.2)
        self._closed = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def address(self) -> tuple[str, int]:
        host, port = self._sock.getsockname()
        return str(host), int(port)

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._thread = threading.Thread(target=self._serve_forever, daemon=True)
        self._thread.start()

    def _serve_forever(self) -> None:
        while not self._closed.is_set():
            try:
                conn, _addr = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with conn:
                fileobj = conn.makefile("rwb")
                while not self._closed.is_set():
                    raw = fileobj.readline()
                    if not raw:
                        break
                    try:
                        message = json.loads(raw.decode("utf-8"))
                    except json.JSONDecodeError as exc:
                        reply = {"ok": False, "error": f"invalid_json:{exc.msg}"}
                    else:
                        reply = self._dispatch(message)
                    fileobj.write(_json_dumps(reply))
                    fileobj.flush()

    def _dispatch(self, message: object) -> JsonObject:
        if not isinstance(message, dict):
            return {"ok": False, "error": "message_must_be_object"}
        try:
            reply = self._handler(message)
        except Exception as exc:  # pragma: no cover - defensive
            return {"ok": False, "error": str(exc)}
        if reply is None:
            return {"ok": True}
        payload = dict(reply)
        payload.setdefault("ok", True)
        return payload

    def close(self) -> None:
        self._closed.set()
        try:
            self._sock.close()
        except OSError:
            pass
        if self._thread is not None:
            self._thread.join(timeout=1.0)

    def __enter__(self) -> "ControlServer":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


class ControlClient:
    def __init__(self, host: str, port: int, *, timeout: float = 5.0) -> None:
        self._host = host
        self._port = int(port)
        self._timeout = float(timeout)

    def request(self, payload: Mapping[str, object]) -> JsonObject:
        with socket.create_connection((self._host, self._port), timeout=self._timeout) as conn:
            fileobj = conn.makefile("rwb")
            fileobj.write(_json_dumps(payload))
            fileobj.flush()
            raw = fileobj.readline()
        if not raw:
            raise RuntimeError("control connection closed without a reply")
        message = json.loads(raw.decode("utf-8"))
        if not isinstance(message, dict):
            raise RuntimeError("control reply must be a JSON object")
        return message
