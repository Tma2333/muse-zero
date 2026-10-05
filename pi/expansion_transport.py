"""ExpansionTransport: run pyflipper's RpcSession over the Flipper Zero
expansion-module protocol (GPIO header UART).

Link layer (official expansion protocol): wake blip -> HEARTBEAT ->
BAUDRATE negotiate to 230400 -> CONTROL Start RPC. Above that, DATA frames
carry the same varint-delimited PB_Main stream the phone app speaks; every
DATA frame is ACKed with a STATUS frame. The firmware drops the session
after ~250 ms of silence, so a background thread sends HEARTBEAT frames
whenever the line would otherwise be idle.

The transport interface matches pyflipper's BleTransport: write(bytes) and
read_message(matcher, timeout, what).
"""

import threading
import time

import serial

from pyflipper.lib import rpc_codec

F_HEARTBEAT = 0x01
F_STATUS = 0x02
F_BAUDRATE = 0x03
F_CONTROL = 0x04
F_DATA = 0x05

CTRL_START_RPC = 0
CTRL_STOP_RPC = 1

STATUS_OK = 0

_CONTENT_LEN = {F_HEARTBEAT: 0, F_STATUS: 1, F_BAUDRATE: 4, F_CONTROL: 1}
_MAX_DATA = 64


class ExpansionError(Exception):
    pass


def _xor(data: bytes) -> int:
    result = 0
    for byte in data:
        result ^= byte
    return result


def _frame(payload: bytes) -> bytes:
    return payload + bytes([_xor(payload)])


class ExpansionTransport:
    def __init__(self, port="/dev/ttyAMA0", baud=230400):
        self._port = port
        self._target_baud = baud
        self._ser = None

        # Reassembled RPC byte stream + waiters (same shape as BleTransport).
        self._buffer = bytearray()
        self._lock = threading.Lock()
        self._data_arrived = threading.Event()

        # Frame layer.
        self._tx_lock = threading.Lock()
        self._ack = threading.Event()
        self._ack_error = None
        self._last_tx = 0.0

        self._stop = threading.Event()
        self._connected = False
        self._reader_thread = None
        self._heartbeat_thread = None

    # ------------------------------------------------------------------
    # connection lifecycle
    # ------------------------------------------------------------------
    def connect(self, attempts=3):
        """Connect with retries: the first baud negotiation after a quiet
        line intermittently goes unanswered; a fresh attempt after a short
        pause has proven reliable, so bake that discipline in here instead
        of making every caller loop."""
        last_error = None
        for attempt in range(attempts):
            try:
                return self._connect_once()
            except Exception as exc:  # noqa: BLE001 - retried below
                last_error = exc
                try:
                    if self._ser is not None:
                        self._ser.close()
                except Exception:
                    pass
                self._ser = None
                self._connected = False
                self._stop.clear()
                if attempt + 1 < attempts:
                    time.sleep(2.0)
        raise ExpansionError(
            f"connect failed after {attempts} attempts: {last_error}")

    def _connect_once(self):
        ser = serial.Serial(self._port, 9600, timeout=0.01)
        self._ser = ser

        def wait_for(pattern: bytes, seconds: float) -> bool:
            buf = b""
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                chunk = ser.read(1)
                if chunk:
                    buf += chunk
                    if pattern in buf:
                        return True
            return False

        heartbeat = _frame(bytes([F_HEARTBEAT]))
        status_ok = _frame(bytes([F_STATUS, STATUS_OK]))

        for _ in range(6):
            ser.reset_input_buffer()
            ser.write(b"\x00")  # ~1 ms of low on the RX pin == the wake edge
            if wait_for(heartbeat, 1.2):
                break
        else:
            raise ExpansionError("no HEARTBEAT from Flipper after wake blips")

        ser.write(_frame(bytes([F_BAUDRATE]) + self._target_baud.to_bytes(4, "little")))
        if not wait_for(status_ok, 0.5):
            raise ExpansionError("BAUDRATE negotiate was not ACKed")

        time.sleep(0.03)  # firmware wants ~25 ms of silence while it re-clocks
        ser.baudrate = self._target_baud

        ser.write(_frame(bytes([F_CONTROL, CTRL_START_RPC])))
        if not wait_for(status_ok, 0.5):
            raise ExpansionError("CONTROL Start RPC was not ACKed")

        self._last_tx = time.monotonic()
        self._connected = True
        self._reader_thread = threading.Thread(
            target=self._read_loop, name="expansion-rx", daemon=True)
        self._reader_thread.start()
        self._heartbeat_thread = threading.Thread(
            target=self._heartbeat_loop, name="expansion-hb", daemon=True)
        self._heartbeat_thread.start()
        return self

    def disconnect(self):
        if self._ser is None:
            return
        try:
            if self._connected:
                self._ack.clear()
                self._send_frame(bytes([F_CONTROL, CTRL_STOP_RPC]))
                self._ack.wait(0.5)
        except Exception:
            pass
        self._connected = False
        self._stop.set()
        try:
            self._ser.close()
        finally:
            self._ser = None

    @property
    def is_connected(self):
        return self._connected

    def __enter__(self):
        if not self._connected:
            self.connect()
        return self

    def __exit__(self, *_exc):
        self.disconnect()
        return False

    # ------------------------------------------------------------------
    # frame layer
    # ------------------------------------------------------------------
    def _send_frame(self, payload: bytes):
        with self._tx_lock:
            self._ser.write(_frame(payload))
            self._last_tx = time.monotonic()

    def _read_loop(self):
        buf = bytearray()
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(64)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while True:
                parsed = self._try_parse(buf)
                if parsed is None:
                    break
                frame_type, content, consumed = parsed
                if consumed:
                    del buf[:consumed]
                if frame_type is not None:
                    self._handle_frame(frame_type, content)

    @staticmethod
    def _try_parse(buf):
        """Parse one frame off the front of ``buf``.

        Returns (type, content, consumed), (None, b"", consumed) when bytes
        were skipped to resync, or None when more bytes are needed.
        """
        if not buf:
            return None
        frame_type = buf[0]
        if frame_type == F_DATA:
            if len(buf) < 2:
                return None
            if buf[1] > _MAX_DATA:
                return (None, b"", 1)
            content_len = 1 + buf[1]
        elif frame_type in _CONTENT_LEN:
            content_len = _CONTENT_LEN[frame_type]
        else:
            return (None, b"", 1)
        total = 1 + content_len + 1
        if len(buf) < total:
            return None
        raw = bytes(buf[:total])
        if _xor(raw[:-1]) != raw[-1]:
            return (None, b"", 1)
        return (frame_type, raw[1:1 + content_len], total)

    def _handle_frame(self, frame_type, content):
        if frame_type == F_DATA:
            size = content[0]
            with self._lock:
                self._buffer += content[1:1 + size]
            self._send_frame(bytes([F_STATUS, STATUS_OK]))
            self._data_arrived.set()
        elif frame_type == F_STATUS:
            self._ack_error = content[0] if content else None
            self._ack.set()
        # HEARTBEAT frames from the Flipper are replies to ours; answering
        # them would ping-pong forever, so they are simply dropped.

    def _heartbeat_loop(self):
        # The firmware tears the session down after ~250 ms without a frame,
        # so keep the line warm while nothing else is being sent.
        while not self._stop.is_set():
            time.sleep(0.05)
            if time.monotonic() - self._last_tx > 0.09:
                try:
                    self._send_frame(bytes([F_HEARTBEAT]))
                except Exception:
                    break

    # ------------------------------------------------------------------
    # RpcSession transport interface
    # ------------------------------------------------------------------
    def write(self, data: bytes):
        """Send an RPC byte stream as <=64-byte DATA frames, ACK by ACK."""
        for offset in range(0, len(data), _MAX_DATA):
            chunk = bytes(data[offset:offset + _MAX_DATA])
            self._ack.clear()
            self._send_frame(bytes([F_DATA, len(chunk)]) + chunk)
            if not self._ack.wait(2.0):
                raise ExpansionError("no STATUS ack for DATA frame")
            if self._ack_error:
                raise ExpansionError(
                    f"Flipper rejected DATA frame (status {self._ack_error})")

    def read_message(self, matcher, timeout=None, what="a reply"):
        """Wait for a parsed PB_Main that ``matcher`` accepts."""
        if timeout is None:
            timeout = 5.0
        deadline = time.monotonic() + timeout
        while True:
            with self._lock:
                while True:
                    parsed = rpc_codec.parse_one(self._buffer)
                    if parsed is None:
                        break
                    message, consumed = parsed
                    del self._buffer[:consumed]
                    if matcher(message):
                        return message
                self._data_arrived.clear()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"timed out waiting for {what}")
            self._data_arrived.wait(remaining)
