#!/usr/bin/env python3
"""Muse Bridge wire codec (Pi side) — mirror of transport/bridge_codec.c
and the plan §21 reference core. COBS + CRC-32/ISO-HDLC + 60-byte envelope.

Run directly to print the golden vectors; they must match the C printer
(tests/native/print_vectors.c) byte-for-byte.
"""
import struct
import sys
import zlib

MAGIC = b"\x42\x52"
PROTO_MAJOR, PROTO_MINOR = 1, 0
PAYLOAD_MAX = 384
FRAME_MAX = 512

# message types
T_HELLO, T_HELLO_REPLY, T_CONFIRM, T_SESSION_READY = 1, 2, 3, 4
T_CHALLENGE, T_ECHO, T_QUERY, T_REQUEST, T_RESULT = 5, 6, 7, 8, 9
T_ACCEPTED, T_EVENT, T_COMPLETE = 10, 11, 12

# opcodes (P1-A subset)
OP_PING = 0x0001
OP_GET_INFO = 0x0002
OP_GET_CAPABILITIES = 0x0003
OP_GET_STATUS = 0x0004
OP_GET_RESULT = 0x0005
OP_CANCEL = 0x0006
OP_CLOSE_SESSION = 0x0007
OP_GET_TRACE = 0x0008

STATUS_NAMES = {
    0: "OK", 1: "STOP_REQUESTED", 2: "INVALID_FRAME", 3: "INVALID_ARGUMENT",
    4: "UNSUPPORTED", 5: "VERSION_MISMATCH", 6: "NO_SESSION",
    7: "STALE_CONNECTION", 8: "SEQUENCE_GAP", 9: "REQUEST_ID_REUSED",
    10: "STALE_REQUEST", 11: "BUSY", 12: "RESOURCE_UNAVAILABLE",
    13: "LIMIT_EXCEEDED", 14: "TIMEOUT", 15: "CANCELLED", 16: "LINK_LOST",
    17: "OVERFLOW", 18: "IO_ERROR", 19: "INIT_FAILED", 20: "CLEANUP_FAILED",
    21: "FAULTED", 22: "TRY_LATER_NOT_ADMITTED", 23: "OUTCOME_UNAVAILABLE",
}


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def cobs_encode(data: bytes) -> bytes:
    out = bytearray(b"\x00")
    code_at = 0
    code = 1
    for b in data:
        if b == 0:
            out[code_at] = code
            code_at = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_at] = code
                code_at = len(out)
                out.append(0)
                code = 1
    out[code_at] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    if not data:
        raise ValueError("empty cobs input")
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0:
            raise ValueError("zero code byte")
        count = code - 1
        if count > len(data) - i:
            raise ValueError("cobs overrun")
        chunk = data[i : i + count]
        if 0 in chunk:
            raise ValueError("zero inside cobs block")
        out += chunk
        i += count
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def frame_encode(boot: bytes, session: bytes, generation: int, mtype: int,
                 op: int, status: int, correlation: int, event_seq: int,
                 payload: bytes) -> bytes:
    """Returns the COBS-encoded frame without the 0x00 delimiter."""
    assert len(boot) == 16 and len(session) == 16
    assert len(payload) <= PAYLOAD_MAX
    body = bytearray(60)
    body[0:2] = MAGIC
    body[2] = PROTO_MAJOR
    body[3] = PROTO_MINOR
    body[4] = mtype
    body[5] = 0
    struct.pack_into("<HHH", body, 6, op, status, len(payload))
    struct.pack_into("<I", body, 12, generation)
    struct.pack_into("<Q", body, 16, correlation)
    struct.pack_into("<I", body, 24, event_seq)
    body[28:44] = boot
    body[44:60] = session
    body += payload
    body += struct.pack("<I", crc32(bytes(body)))
    return cobs_encode(bytes(body))


class Frame:
    def __init__(self, raw: bytes):
        if len(raw) < 64:
            raise ValueError("short frame")
        body, check = raw[:-4], struct.unpack("<I", raw[-4:])[0]
        if crc32(body) != check:
            raise ValueError("crc mismatch")
        if raw[0:2] != MAGIC:
            raise ValueError("bad magic")
        if raw[2] != PROTO_MAJOR or raw[3] != PROTO_MINOR:
            raise ValueError("bad version")
        self.type = raw[4]
        self.op, self.status, length = struct.unpack_from("<HHH", raw, 6)
        if length > PAYLOAD_MAX or len(raw) != 60 + length + 4:
            raise ValueError("bad length")
        self.generation = struct.unpack_from("<I", raw, 12)[0]
        self.correlation = struct.unpack_from("<Q", raw, 16)[0]
        self.event_seq = struct.unpack_from("<I", raw, 24)[0]
        self.boot = raw[28:44]
        self.session = raw[44:60]
        self.payload = raw[60 : 60 + length]

    def __repr__(self):
        return (f"Frame(type={self.type} op=0x{self.op:04x} status="
                f"{STATUS_NAMES.get(self.status, self.status)} corr={self.correlation} "
                f"gen={self.generation} payload={self.payload.hex()})")


def frame_decode(wire: bytes) -> Frame:
    """wire = COBS bytes without delimiter."""
    return Frame(cobs_decode(wire))


class RxCollector:
    """Mirror of the C MbRx byte collector (partial timeout in ticks)."""

    def __init__(self, partial_ticks: int = 250):
        self.buf = bytearray()
        self.discard = False
        self.started = 0
        self.partial_ticks = partial_ticks

    def poison(self):
        self.buf.clear()
        self.discard = True

    def expire(self, now: int) -> bool:
        if self.buf and (now - self.started) & 0xFFFFFFFF >= self.partial_ticks:
            self.poison()
            return True
        return False

    def feed(self, byte: int, now: int):
        """Returns (event, frame_bytes) — event in
        wait/frame/discard/oversize/timeout."""
        timed_out = self.expire(now)
        if self.discard:
            if byte == 0:
                self.discard = False
            return ("timeout" if timed_out else "discard"), None
        if byte == 0:
            if not self.buf:
                return "wait", None
            frame = bytes(self.buf)
            self.buf.clear()
            return "frame", frame
        if len(self.buf) >= 515:
            self.poison()
            return "oversize", None
        if not self.buf:
            self.started = now
        self.buf.append(byte)
        return "wait", None


def print_vectors():
    boot = bytes([1]) + bytes(15)
    session = bytes([2]) + bytes(15)
    v1 = frame_encode(boot, session, 1, T_QUERY, OP_PING, 0, 9, 0, bytes([2]) + b"OK")
    print("V1", v1.hex())
    payload = bytes([3]) + b"abc" + struct.pack("<II", 0x01020304, 1000)
    v2 = frame_encode(boot, session, 1, T_RESULT, OP_PING, 0, 9, 0, payload)
    print("V2", v2.hex())
    v3 = frame_encode(boot, session, 1, T_QUERY, OP_GET_INFO, 0, 10, 0, bytes([0]))
    print("V3", v3.hex())


if __name__ == "__main__":
    if "--vectors" in sys.argv:
        print_vectors()
    else:
        # sanity: the golden CRC from the plan's reference test
        assert crc32(b"123456789") == 0xCBF43926
        print("bridge_codec self-check OK (crc32 check value matches)")
