#!/usr/bin/env python3
"""Muse Bridge hardware-in-the-loop harness (runs on flippi).

Case C04: COBS/CRC codec, bootstrap PING/GET_INFO, resync behavior.
The bridge FAP must already be running (it owns /dev/ttyAMA0).
Prints PASS/FAIL per check and exits nonzero on any failure.
"""
import argparse
import os
import struct
import sys
import threading
import time

import serial

sys.path.insert(0, os.environ.get("FLIPPER_RPC_DIR", os.path.expanduser("~/flipper-rpc")))
import bridge_codec as bc

ZERO16 = bytes(16)


class Link:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.rx = bc.RxCollector(partial_ticks=250)
        self.pending = []  # decoded Frames
        self.now_ms = 0
        self.io_lock = threading.Lock()
        self.threaded = False  # an EchoThread owns pumping when True

    def _pump(self, dur=0.0):
        with self.io_lock:
            self._pump_locked(dur)

    def _pump_locked(self, dur=0.0):
        end = time.monotonic() + dur
        while True:
            data = self.ser.read(256)
            now = int(time.monotonic() * 1000)
            for b in data:
                ev, frame = self.rx.feed(b, now)
                if ev == "frame":
                    try:
                        self.pending.append(bc.frame_decode(frame))
                    except ValueError as e:
                        print(f"  (undecodable frame from FAP: {e})")
            if time.monotonic() >= end:
                break

    def _write_wire(self, wire):
        with self.io_lock:
            self.ser.write(wire + b"\x00")
            self.ser.flush()

    def send(self, mtype, op, correlation, payload, raw=None):
        if raw is not None:
            with self.io_lock:
                self.ser.write(raw)
                self.ser.flush()
        else:
            wire = bc.frame_encode(ZERO16, ZERO16, 0, mtype, op, 0, correlation, 0, payload)
            self._write_wire(wire)

    def send_id(self, boot, session, generation, mtype, op, correlation, payload):
        wire = bc.frame_encode(boot, session, generation, mtype, op, 0, correlation, 0, payload)
        self._write_wire(wire)

    def _pop_match(self, mtype, correlation):
        with self.io_lock:
            for i, f in enumerate(self.pending):
                if (mtype is None or f.type == mtype) and \
                        (correlation is None or f.correlation == correlation):
                    return self.pending.pop(i)
        return None

    def _wait(self, mtype, correlation, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            f = self._pop_match(mtype, correlation)
            if f is not None:
                return f
            if not self.threaded:
                self._pump(0.05)
            else:
                time.sleep(0.01)
        return None

    def wait_frame(self, correlation, timeout=3.0):
        return self._wait(None, correlation, timeout)

    def wait_type(self, mtype, timeout=3.0, correlation=None):
        return self._wait(mtype, correlation, timeout)

    def drain_challenges(self):
        with self.io_lock:
            out = []
            keep = []
            for f in self.pending:
                (out if f.type == bc.T_CHALLENGE else keep).append(f)
            self.pending = keep
            return out

    def pop_all(self, mtype, correlation=None):
        with self.io_lock:
            out = [f for f in self.pending
                   if f.type == mtype and (correlation is None or f.correlation == correlation)]
            self.pending = [f for f in self.pending
                            if not (f.type == mtype and (correlation is None or f.correlation == correlation))]
            return out

    def echo_loop(self, seconds):
        """Answer every HEARTBEAT_CHALLENGE with the exact echo, for a while."""
        echoed = 0
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self._pump(0.05)
            for ch in self.drain_challenges():
                self.send_id(ch.boot, ch.session, ch.generation, bc.T_ECHO, 0, 0, ch.payload)
                echoed += 1
        return echoed

    def close(self):
        self.ser.close()


class EchoThread:
    """Background liveness pump: answers challenges while the main
    thread runs actions. pause() simulates a degrading link (SUSPECT);
    frames keep being pumped, only the answers stop."""

    def __init__(self, link):
        self.link = link
        self.paused = False
        self.held = False  # no pumping at all: both directions broken
        self.running = False
        self.echoed = 0
        self.challenges_seen = 0
        self._thread = None

    def _run(self):
        link = self.link
        while self.running:
            if self.held:
                time.sleep(0.05)
                continue
            link._pump(0.05)
            for ch in link.drain_challenges():
                self.challenges_seen += 1
                if not self.paused:
                    link.send_id(ch.boot, ch.session, ch.generation,
                                 bc.T_ECHO, 0, 0, ch.payload)
                    self.echoed += 1

    def start(self):
        self.link.threaded = True
        self.running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self.running = False
        if self._thread:
            self._thread.join(timeout=2.0)
        self.link.threaded = False


RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok))
    print(f"{'PASS' if ok else 'FAIL'} {name} {detail}")


def ping_payload(data: bytes) -> bytes:
    return bytes([len(data)]) + data


def case_c04(link):
    corr = 100

    # 1. PING echo at several sizes
    for data in (b"", b"hello-flipper!", bytes(range(1, 33))):
        corr += 1
        link.send(bc.T_QUERY, bc.OP_PING, corr, ping_payload(data))
        f = link.wait_frame(corr)
        ok = (
            f is not None
            and f.type == bc.T_RESULT
            and f.op == bc.OP_PING
            and f.status == 0
            and len(f.payload) == 1 + len(data) + 8
            and f.payload[0] == len(data)
            and f.payload[1 : 1 + len(data)] == data
        )
        check(f"ping_echo[{len(data)}B]", ok, repr(f) if not ok else "")

    # 2. GET_INFO page 0
    corr += 1
    link.send(bc.T_QUERY, bc.OP_GET_INFO, corr, bytes([0]))
    f = link.wait_frame(corr)
    ok = f is not None and f.status == 0 and len(f.payload) >= 16 and any(f.boot)
    build = ""
    if f is not None and len(f.payload) >= 17:
        blen = f.payload[16]
        build = f.payload[17 : 17 + blen].decode("ascii", "replace")
    check("get_info", ok, f"build={build} boot={f.boot.hex() if f else '-'}")

    # 3. Garbage, truncated frame, oversize run, then a valid ping
    link.send(None, None, None, None, raw=b"\xff\xfe random junk \x00")
    good = bc.frame_encode(ZERO16, ZERO16, 0, bc.T_QUERY, bc.OP_PING, 0, 999, 0, ping_payload(b"x"))
    link.send(None, None, None, None, raw=good[:10] + b"\x00")  # truncated -> CRC reject
    link.send(None, None, None, None, raw=b"\x01" * 600 + b"\x00")  # oversize
    corr += 1
    link.send(bc.T_QUERY, bc.OP_PING, corr, ping_payload(b"alive"))
    f = link.wait_frame(corr)
    check("resync_after_garbage", f is not None and f.status == 0, repr(f))

    # 4. Two frames concatenated in one write
    corr += 1
    c1, c2 = corr, corr + 1
    w1 = bc.frame_encode(ZERO16, ZERO16, 0, bc.T_QUERY, bc.OP_PING, 0, c1, 0, ping_payload(b"one"))
    w2 = bc.frame_encode(ZERO16, ZERO16, 0, bc.T_QUERY, bc.OP_PING, 0, c2, 0, ping_payload(b"two"))
    link.send(None, None, None, None, raw=w1 + b"\x00" + w2 + b"\x00")
    f1 = link.wait_frame(c1)
    f2 = link.wait_frame(c2)
    check("concat_frames", f1 is not None and f2 is not None)

    # 5. One frame delivered in fragments with real gaps between bursts.
    # (Byte-at-a-time via pyserial+flush costs ~8 ms/byte on this stack,
    # which would exceed the spec'd 250 ms partial-frame window for
    # reasons unrelated to the device; chunk fragments with 30 ms gaps
    # test the same reassembly path realistically.)
    corr = c2 + 1
    w = bc.frame_encode(ZERO16, ZERO16, 0, bc.T_QUERY, bc.OP_PING, 0, corr, 0, ping_payload(b"slow"))
    wire = w + b"\x00"
    chunks = [wire[i : i + 15] for i in range(0, len(wire), 15)]
    for c in chunks:
        link.ser.write(c)
        link.ser.flush()
        time.sleep(0.03)
    f = link.wait_frame(corr)
    check("split_frame", f is not None and f.status == 0, repr(f))

    # 6. Unsupported op still gets a typed refusal (no crash, no silence)
    corr += 1
    link.send(bc.T_QUERY, 0x7F7F, corr, b"")
    f = link.wait_frame(corr)
    check("unsupported_op", f is not None and f.status == 4, repr(f))


CLIENT_A = bytes(range(0xA0, 0xB0))
CLIENT_B = bytes(range(0xB0, 0xC0))
NONCE_1 = bytes(range(0xC0, 0xD0))
NONCE_2 = bytes(range(0xD0, 0xE0))
NONCE_3 = bytes(range(0xE0, 0xF0))


def hello_payload(client, nonce, prior_boot=ZERO16, prior_session=ZERO16):
    return client + nonce + prior_boot + prior_session


def confirm_payload(client, nonce, challenge):
    return client + nonce + struct.pack("<Q", challenge)


def parse_hello_reply(f):
    p = f.payload
    if len(p) != 69:
        return None
    return {
        "client": p[0:16], "nonce": p[16:32],
        "challenge": struct.unpack_from("<Q", p, 32)[0],
        "last_consumed": struct.unpack_from("<Q", p, 40)[0],
        "active_job": struct.unpack_from("<Q", p, 48)[0],
        "interval_ms": struct.unpack_from("<H", p, 56)[0],
        "proof_age_ms": struct.unpack_from("<H", p, 58)[0],
        "suspect_ms": struct.unpack_from("<H", p, 60)[0],
        "lease_ms": struct.unpack_from("<H", p, 62)[0],
        "max_payload": struct.unpack_from("<H", p, 64)[0],
        "ledger": p[66], "link": p[67], "exec": p[68],
        "boot": f.boot, "session": f.session, "gen": f.generation,
    }


def do_hello(link, corr, client, nonce, prior_boot=ZERO16, prior_session=ZERO16):
    link.send_id(ZERO16, ZERO16, 0, bc.T_HELLO, 0, corr,
                 hello_payload(client, nonce, prior_boot, prior_session))
    f = link.wait_type(bc.T_HELLO_REPLY, correlation=corr)
    return f


def do_confirm(link, corr, client, nonce, challenge, boot, session, gen):
    link.send_id(boot, session, gen, bc.T_CONFIRM, 0, corr,
                 confirm_payload(client, nonce, challenge))
    return link


def case_c06(link):
    corr = 200

    # bootstrap still fine on the session build
    corr += 1
    link.send(bc.T_QUERY, bc.OP_GET_INFO, corr, bytes([0]))
    f = link.wait_frame(corr)
    build = ""
    if f is not None and len(f.payload) >= 17:
        blen = f.payload[16]
        build = f.payload[17 : 17 + blen].decode("ascii", "replace")
    check("get_info_session_build", build.startswith("mb-"), build)

    # 1. fresh HELLO -> proposal
    corr += 1
    f = do_hello(link, corr, CLIENT_A, NONCE_1)
    rep = parse_hello_reply(f) if f is not None and f.status == 0 else None
    ok = (
        rep is not None
        and rep["client"] == CLIENT_A and rep["nonce"] == NONCE_1
        and rep["challenge"] != 0
        and rep["gen"] == 1 and any(rep["session"]) and any(rep["boot"])
        and rep["interval_ms"] == 500 and rep["proof_age_ms"] == 1000
        and rep["suspect_ms"] == 1500 and rep["lease_ms"] == 3000
        and rep["max_payload"] == 176 and rep["ledger"] == 16
    )
    check("hello_proposal", ok, repr(f) if not ok else "")

    # 2. CONFIRM with wrong challenge rejected
    corr += 1
    do_confirm(link, corr, CLIENT_A, NONCE_1, rep["challenge"] ^ 1,
               rep["boot"], rep["session"], rep["gen"])
    f = link.wait_frame(corr)
    check("confirm_wrong_challenge",
          f is not None and f.type == bc.T_RESULT and f.status == 3, repr(f))

    # 3. CONFIRM correct -> SESSION_READY
    corr += 1
    do_confirm(link, corr, CLIENT_A, NONCE_1, rep["challenge"],
               rep["boot"], rep["session"], rep["gen"])
    f = link.wait_type(bc.T_SESSION_READY, correlation=corr)
    ok = (
        f is not None and f.payload[0:16] == NONCE_1
        and len(f.payload) == 34 and f.payload[32] == 1  # link LIVE
        and f.generation == 1 and f.session == rep["session"]
    )
    check("confirm_ok_ready", ok, repr(f) if not ok else "")

    # 4. duplicate CONFIRM recovers the same READY (lost-reply case)
    corr += 1
    do_confirm(link, corr, CLIENT_A, NONCE_1, rep["challenge"],
               rep["boot"], rep["session"], rep["gen"])
    f = link.wait_type(bc.T_SESSION_READY, correlation=corr)
    check("confirm_duplicate_ready", f is not None and f.generation == 1, repr(f))

    # 5. competing client gets BUSY while owner is live
    corr += 1
    f = do_hello(link, corr, CLIENT_B, NONCE_2)
    check("competing_hello_busy",
          f is not None and f.status == 11 and len(f.payload) == 0, repr(f))

    # 6. heartbeat: answer challenges ~4 s, owner stays live
    echoed = link.echo_loop(4.0)
    check("heartbeat_echoes", echoed >= 4, f"echoed={echoed}")
    corr += 1
    f = do_hello(link, corr, CLIENT_B, NONCE_2)
    check("still_busy_after_heartbeats",
          f is not None and f.status == 11, repr(f))

    # 7. silence -> lease expiry releases ownership (new owner, new session)
    echoed = link.echo_loop(0.2)  # flush, then go quiet
    quiet_end = time.monotonic() + 4.0
    challenges_seen = 0
    while time.monotonic() < quiet_end:
        link._pump(0.05)
        challenges_seen += len(link.drain_challenges())
    corr += 1
    f = do_hello(link, corr, CLIENT_B, NONCE_2)
    rep_b = parse_hello_reply(f) if f is not None and f.status == 0 else None
    check("expiry_releases_owner",
          rep_b is not None and rep_b["session"] != rep["session"] and rep_b["gen"] == 1,
          repr(f))
    if rep_b is not None:
        corr += 1
        do_confirm(link, corr, CLIENT_B, NONCE_2, rep_b["challenge"],
                   rep_b["boot"], rep_b["session"], rep_b["gen"])
        f = link.wait_type(bc.T_SESSION_READY, correlation=corr)
        check("new_owner_confirms", f is not None, repr(f))
        link.echo_loop(0.5)

    # 8. The replaced session is gone: A's original session cannot be
    #    resurrected. The current owner's (B's) session resumes with
    #    the generation rotated instead.
    quiet_end = time.monotonic() + 3.6
    while time.monotonic() < quiet_end:
        link._pump(0.05)
        link.drain_challenges()
    corr += 1
    f = do_hello(link, corr, CLIENT_A, NONCE_3, rep["boot"], rep["session"])
    rep_a2 = parse_hello_reply(f) if f is not None and f.status == 0 else None
    check("replaced_session_not_resurrectable",
          rep_a2 is not None and rep_a2["session"] != rep["session"] and rep_a2["gen"] == 1,
          repr(f))
    # (A's fresh proposal is left unconfirmed; B's HELLO supersedes it.)
    corr += 1
    f = do_hello(link, corr, CLIENT_B, NONCE_3, rep_b["boot"], rep_b["session"])
    rep_b2 = parse_hello_reply(f) if f is not None and f.status == 0 else None
    check("resume_same_session",
          rep_b2 is not None and rep_b2["session"] == rep_b["session"] and rep_b2["gen"] == 2,
          repr(f))
    if rep_b2 is not None:
        corr += 1
        do_confirm(link, corr, CLIENT_B, NONCE_3, rep_b2["challenge"],
                   rep_b2["boot"], rep_b2["session"], rep_b2["gen"])
        f = link.wait_type(bc.T_SESSION_READY, correlation=corr)
        check("resume_confirms", f is not None and f.generation == 2, repr(f))
        # one fresh challenge arrives; answer it with the OLD generation's
        # header - a stale echo must not renew, so after 3.4 s of silence
        # the lease expires and a different client is no longer BUSY.
        ch = link.wait_type(bc.T_CHALLENGE, timeout=2.0)
        check("challenge_after_resume", ch is not None, "")
        if ch is not None:
            link.send_id(ch.boot, ch.session, 1, bc.T_ECHO, 0, 0, ch.payload)
        quiet_end = time.monotonic() + 3.4
        while time.monotonic() < quiet_end:
            link._pump(0.05)
            link.drain_challenges()
        corr += 1
        f = do_hello(link, corr, CLIENT_A, NONCE_2)
        check("stale_echo_did_not_renew",
              f is not None and f.status == 0, repr(f))

    # 9. bootstrap unaffected at the end
    corr += 1
    link.send(bc.T_QUERY, bc.OP_PING, corr, ping_payload(b"hb"))
    f = link.wait_frame(corr)
    check("ping_after_sessions", f is not None and f.status == 0, repr(f))


OP_FAKE_RUN = 0x7F01
S_OK, S_INVAL, S_GAP, S_REUSED, S_STALE, S_BUSY, S_TRYLATER, S_STALECONN = \
    0, 3, 8, 9, 10, 11, 22, 7


def fake_payload(duration_ms, interval_ms):
    return struct.pack("<IH", duration_ms, interval_ms)


def send_req(link, ident, seq, payload, op=OP_FAKE_RUN):
    link.send_id(ident[0], ident[1], ident[2], bc.T_REQUEST, op, seq, payload)


def get_result(link, corr, ident, seq):
    link.send_id(ident[0], ident[1], ident[2], bc.T_QUERY, bc.OP_GET_RESULT,
                 corr, struct.pack("<Q", seq))
    return link.wait_frame(corr, timeout=4.0)


def handshake_a(link, nonce, client=CLIENT_A):
    corr = 700
    f = do_hello(link, corr, client, nonce)
    rep = parse_hello_reply(f) if f is not None and f.status == 0 else None
    if rep is None:
        return None
    do_confirm(link, corr + 1, client, nonce, rep["challenge"],
               rep["boot"], rep["session"], rep["gen"])
    ready = link.wait_type(bc.T_SESSION_READY, correlation=corr + 1)
    if ready is None:
        return None
    return (rep["boot"], rep["session"], rep["gen"])


def case_c07(link):
    f = None
    corr = 800
    link.send(bc.T_QUERY, bc.OP_GET_INFO, corr, bytes([0]))
    f = link.wait_frame(corr)
    build = ""
    if f is not None and len(f.payload) >= 17:
        blen = f.payload[16]
        build = f.payload[17 : 17 + blen].decode("ascii", "replace")
    check("get_info_c07_build", build.startswith("mb-"), build)

    ident = handshake_a(link, NONCE_1)
    check("session_for_actions", ident is not None, "")
    if ident is None:
        return

    echo = EchoThread(link)
    echo.start()
    try:
        # 1. seq 1: accepted, events, one completion
        send_req(link, ident, 1, fake_payload(1200, 200))
        f = link.wait_type(10, correlation=1, timeout=4.0)  # ACCEPTED
        check("seq1_accepted", f is not None, repr(f))
        events = []
        complete = None
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            got = link.pop_all(11, correlation=1)
            events.extend(got)
            c = link._pop_match(12, 1)
            if c is not None:
                complete = c
                break
            time.sleep(0.02)
        seqs = [e.event_seq for e in events]
        ok = (complete is not None and complete.status == S_OK
              and len(complete.payload) == 10 and len(events) >= 3
              and seqs == sorted(seqs) and seqs[0] == 1)
        check("seq1_events_and_complete", ok,
              f"events={len(events)} complete={repr(complete)}")

        # 2. exact duplicate after completion: COMPLETE replayed verbatim
        send_req(link, ident, 1, fake_payload(1200, 200))
        f = link.wait_type(12, correlation=1, timeout=4.0)
        check("dup_complete_replayed",
              f is not None and complete is not None and f.payload == complete.payload,
              repr(f))

        # 3. same seq, changed payload: REQUEST_ID_REUSED
        send_req(link, ident, 1, fake_payload(900, 200))
        f = link.wait_type(9, correlation=1, timeout=4.0)
        check("changed_payload_reused", f is not None and f.status == S_REUSED, repr(f))

        # 4. seq 3 before 2: SEQUENCE_GAP
        send_req(link, ident, 3, fake_payload(100, 0))
        f = link.wait_type(9, correlation=3, timeout=4.0)
        check("sequence_gap", f is not None and f.status == S_GAP, repr(f))

        # 5. seq 2 lands normally
        send_req(link, ident, 2, fake_payload(200, 0))
        f = link.wait_type(10, correlation=2, timeout=4.0)
        c2 = link.wait_type(12, correlation=2, timeout=4.0)
        check("seq2_runs", f is not None and c2 is not None and c2.status == S_OK,
              f"{repr(f)} {repr(c2)}")

        # 6. BUSY consumes: seq 3 runs long, seq 4 refused twice (2nd is replay)
        send_req(link, ident, 3, fake_payload(2500, 0))
        f = link.wait_type(10, correlation=3, timeout=4.0)
        check("seq3_accepted", f is not None, repr(f))
        send_req(link, ident, 3, fake_payload(2500, 0))
        f = link.wait_type(10, correlation=3, timeout=4.0)
        check("midrun_dup_accepted_replay", f is not None, repr(f))
        send_req(link, ident, 4, fake_payload(100, 0))
        f = link.wait_type(9, correlation=4, timeout=4.0)
        check("busy_consumed", f is not None and f.status == S_BUSY, repr(f))
        send_req(link, ident, 4, fake_payload(100, 0))
        f = link.wait_type(9, correlation=4, timeout=4.0)
        check("busy_replayed_as_result", f is not None and f.status == S_BUSY, repr(f))
        c3 = link.wait_type(12, correlation=3, timeout=6.0)
        check("seq3_completes", c3 is not None and c3.status == S_OK, repr(c3))

        # 7. ledger fill/evict: 16 quick jobs, oldest becomes stale
        fill_ok = True
        for seq in range(5, 21):
            send_req(link, ident, seq, fake_payload(120, 0))
            a = link.wait_type(10, correlation=seq, timeout=4.0)
            c = link.wait_type(12, correlation=seq, timeout=4.0)
            if a is None or c is None or c.status != S_OK:
                fill_ok = False
                break
        check("ledger_fill_16", fill_ok, "")
        send_req(link, ident, 2, fake_payload(200, 0))
        f = link.wait_type(9, correlation=2, timeout=4.0)
        check("evicted_is_stale", f is not None and f.status == S_STALE, repr(f))
        send_req(link, ident, 20, fake_payload(120, 0))
        f = link.wait_type(12, correlation=20, timeout=4.0)
        check("retained_replays_complete", f is not None and f.status == S_OK, repr(f))
        f = get_result(link, 901, ident, 20)
        ok = (f is not None and len(f.payload) >= 6 and f.payload[0] == 1
              and f.payload[1] == 3 and struct.unpack_from("<H", f.payload, 2)[0] == S_OK
              and struct.unpack_from("<H", f.payload, 4)[0] == 10)
        check("get_result_terminal", ok, repr(f))
        f = get_result(link, 902, ident, 999)
        check("get_result_absent", f is not None and f.payload[0] == 0, repr(f))

        # 8. SUSPECT: capacity refusal consumes nothing; retry lands
        echo.paused = True
        time.sleep(1.9)
        send_req(link, ident, 21, fake_payload(150, 0))
        f = link.wait_type(9, correlation=21, timeout=4.0)
        check("suspect_try_later", f is not None and f.status == S_TRYLATER, repr(f))
        echo.paused = False
        time.sleep(0.8)
        send_req(link, ident, 21, fake_payload(150, 0))
        a = link.wait_type(10, correlation=21, timeout=4.0)
        c = link.wait_type(12, correlation=21, timeout=5.0)
        check("retry_after_proof_admitted",
              a is not None and c is not None and c.status == S_OK,
              f"{repr(a)} {repr(c)}")

        # 9. stale connection envelope rejected before hardware
        send_req(link, (ident[0], ident[1], ident[2] + 5), 22, fake_payload(100, 0))
        f = link.wait_type(9, correlation=22, timeout=4.0)
        check("stale_generation_rejected", f is not None and f.status == S_STALECONN, repr(f))
    finally:
        echo.stop()


OP_CANCEL = 0x0006
OP_GET_TRACE = 0x0008
ST_STOP_REQUESTED, ST_TIMEOUT_S, ST_CANCELLED, ST_LINK_LOST = 1, 14, 15, 16
ST_OUTCOME_UNAVAILABLE = 23


def send_cancel(link, ident, corr, job):
    link.send_id(ident[0], ident[1], ident[2], bc.T_QUERY, OP_CANCEL, corr,
                 struct.pack("<Q", job))
    return link.wait_frame(corr, timeout=4.0)


def fetch_trace(link, ident):
    """Page the whole C02 ring out over GET_TRACE (newest 64 records)."""
    recs = []
    after = 0
    for _ in range(20):
        corr = 950 + len(recs)
        link.send_id(ident[0], ident[1], ident[2], bc.T_QUERY, OP_GET_TRACE,
                     corr, struct.pack("<IB", after, 4))
        f = link.wait_frame(corr, timeout=4.0)
        if f is None or len(f.payload) < 15:
            return recs, False
        nxt, more = struct.unpack_from("<IB", f.payload, 0)
        count = f.payload[13]
        off = 15
        for _i in range(count):
            seq, tick, job, gen, a0, a1, code, epoch = struct.unpack_from(
                "<IIQIIIHH", f.payload, off)
            recs.append({"seq": seq, "tick": tick, "job": job, "gen": gen,
                         "arg0": a0, "code": code})
            off += 32
        if not more or nxt == after:
            return recs, True
        after = nxt
    return recs, False


def case_c08(link):
    corr = 840
    link.send(bc.T_QUERY, bc.OP_GET_INFO, corr, bytes([0]))
    f = link.wait_frame(corr)
    build = ""
    if f is not None and len(f.payload) >= 17:
        blen = f.payload[16]
        build = f.payload[17:17 + blen].decode("ascii", "replace")
    check("get_info_c08_build", build == "mb-0.7-c08-1", build)

    # A fresh owner (CLIENT_B) takes over after CLIENT_A's lease from
    # the previous case expired: new owner, fresh ledger, seqs from 1.
    # CLIENT_A may still be inside its 3 s lease when this process
    # starts, so retry the takeover until the expiry lands.
    ident = None
    for _attempt in range(5):
        ident = handshake_a(link, NONCE_2, CLIENT_B)
        if ident is not None:
            break
        time.sleep(1.5)
    check("session_for_cancel", ident is not None, "")
    if ident is None:
        return

    echo = EchoThread(link)
    echo.start()
    try:
        # 1. cancel a long, event-flooding job mid-run
        send_req(link, ident, 1, fake_payload(15000, 100))
        f = link.wait_type(10, correlation=1, timeout=4.0)
        check("c08_job_accepted", f is not None, repr(f))
        seen_events = []
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline and len(seen_events) < 3:
            seen_events += link.pop_all(11, correlation=1)
            time.sleep(0.02)
        f = send_cancel(link, ident, 910, 1)
        check("cancel_stop_requested", f is not None and f.status == ST_STOP_REQUESTED, repr(f))
        c = link.wait_type(12, correlation=1, timeout=4.0)
        check("cancel_terminal_cancelled", c is not None and c.status == ST_CANCELLED, repr(c))
        time.sleep(0.4)
        seen_events += link.pop_all(11, correlation=1)
        generated = struct.unpack_from("<I", c.payload, 2)[0] if c is not None and len(c.payload) >= 10 else -1
        ok = generated >= 0 and len(seen_events) <= generated \
            and all(e.event_seq <= generated for e in seen_events)
        check("events_stop_after_cancel", ok,
              f"seen={len(seen_events)} generated={generated}")

        # 2. repeated cancel returns the known state, not a new effect
        f = send_cancel(link, ident, 911, 1)
        check("cancel_repeat_known_state", f is not None and f.status == ST_CANCELLED, repr(f))

        # 3. cancelling the old job never touches the newer one
        send_req(link, ident, 2, fake_payload(2500, 0))
        a2 = link.wait_type(10, correlation=2, timeout=4.0)
        f = send_cancel(link, ident, 912, 1)
        c2 = link.wait_type(12, correlation=2, timeout=6.0)
        check("old_cancel_newer_untouched",
              a2 is not None and f is not None and f.status == ST_CANCELLED
              and c2 is not None and c2.status == S_OK,
              f"{repr(f)} {repr(c2)}")

        # 4. unknown job / wrong generation
        f = send_cancel(link, ident, 913, 999)
        check("cancel_unknown_job", f is not None and f.status == ST_OUTCOME_UNAVAILABLE, repr(f))
        f = send_cancel(link, (ident[0], ident[1], ident[2] + 5), 914, 2)
        check("cancel_stale_generation", f is not None and f.status == 7, repr(f))  # STALE_CONNECTION
        f = get_result(link, 915, ident, 1)
        ok = f is not None and len(f.payload) >= 4 and f.payload[1] == 3 \
            and struct.unpack_from("<H", f.payload, 2)[0] == ST_CANCELLED
        check("get_result_shows_cancelled", ok, repr(f))

        # 5. absolute duration is not extended by healthy heartbeats
        t0 = time.monotonic()
        send_req(link, ident, 3, fake_payload(5000, 0))
        a3 = link.wait_type(10, correlation=3, timeout=4.0)
        c3 = link.wait_type(12, correlation=3, timeout=9.0)
        elapsed = time.monotonic() - t0
        check("job_runs_full_duration_under_heartbeats",
              a3 is not None and c3 is not None and c3.status == S_OK and 4.5 <= elapsed <= 8.0,
              f"elapsed={elapsed:.2f}")

        # 6. lease expiry stops a running job; stop latency from trace ticks
        send_req(link, ident, 4, fake_payload(12000, 0))
        a4 = link.wait_type(10, correlation=4, timeout=4.0)
        check("expiry_job_accepted", a4 is not None, repr(a4))
        echo.paused = True
        c4 = link.wait_type(12, correlation=4, timeout=9.0)
        check("expiry_stop_link_lost", c4 is not None and c4.status == ST_LINK_LOST, repr(c4))
        echo.paused = False
        time.sleep(0.8)
        recs, ok = fetch_trace(link, ident)
        exp_tick = stop_tick = None
        for r in recs:
            if r["code"] == 6:  # LEASE_EXPIRED
                exp_tick = r["tick"]
            if r["code"] == 11 and r["job"] == 4 and exp_tick is not None and stop_tick is None:
                stop_tick = r["tick"]
        delta = (stop_tick - exp_tick) if (exp_tick is not None and stop_tick is not None) else -1
        check("get_trace_pages", ok and len(recs) >= 10, f"records={len(recs)}")
        check("expiry_stop_within_100ms", 0 <= delta <= 100, f"delta_ticks={delta}")
        hwq = any(r["code"] == 12 and r["job"] == 4 for r in recs)
        check("hw_quiescent_traced", hwq, "")
    finally:
        echo.stop()


def case_back(link):
    """Manual proof: a 60 s job runs under a live lease; the operator presses
    Back on the Flipper; the app must stop the job and exit promptly."""
    ident = handshake_a(link, NONCE_1)
    check("back_session", ident is not None, "")
    if ident is None:
        return
    echo = EchoThread(link)
    echo.start()
    try:
        send_req(link, ident, 1, fake_payload(60000, 1000))
        f = link.wait_type(10, correlation=1, timeout=4.0)
        check("back_job_running", f is not None, repr(f))
        print("JOB_RUNNING: press Back on the Flipper now", flush=True)
        # Keep the lease alive; when Back lands, the app stops the job,
        # exits, and the line simply goes quiet.
        time.sleep(75)
        check("back_window_elapsed", True, "")
    finally:
        echo.stop()


def resume_session(link, ident, nonce, corr0=760):
    """Same-boot resume: HELLO with prior ids, fresh nonce, CONFIRM."""
    f = do_hello(link, corr0, CLIENT_A, nonce,
                 prior_boot=ident[0], prior_session=ident[1])
    rep = parse_hello_reply(f) if f is not None and f.status == 0 else None
    if rep is None:
        return None
    do_confirm(link, corr0 + 1, CLIENT_A, nonce, rep["challenge"],
               rep["boot"], rep["session"], rep["gen"])
    ready = link.wait_type(bc.T_SESSION_READY, correlation=corr0 + 1)
    if ready is None:
        return None
    return (rep["boot"], rep["session"], rep["gen"])


def case_c09(link):
    corr = 860
    link.send(bc.T_QUERY, bc.OP_GET_INFO, corr, bytes([0]))
    f = link.wait_frame(corr)
    build = ""
    if f is not None and len(f.payload) >= 17:
        blen = f.payload[16]
        build = f.payload[17:17 + blen].decode("ascii", "replace")
    check("get_info_c09_build", build == "mb-0.7-c08-1", build)

    ident = handshake_a(link, NONCE_1)
    check("c09_session", ident is not None, "")
    if ident is None:
        return
    echo = EchoThread(link)
    echo.start()
    try:
        # L12: 500 ms break in BOTH directions mid-job: nothing changes.
        send_req(link, ident, 1, fake_payload(6000, 500))
        a = link.wait_type(10, correlation=1, timeout=4.0)
        time.sleep(1.5)
        echo.held = True
        time.sleep(0.55)
        echo.held = False
        c = link.wait_type(12, correlation=1, timeout=9.0)
        check("l12_pause_500ms_job_unaffected",
              a is not None and c is not None and c.status == S_OK,
              f"{repr(a)} {repr(c)}")
        send_req(link, ident, 2, fake_payload(300, 0))
        a = link.wait_type(10, correlation=2, timeout=4.0)
        c = link.wait_type(12, correlation=2, timeout=4.0)
        check("l12_session_intact", a is not None and c is not None and c.status == S_OK, "")

        # L13: ~2 s without proofs: SUSPECT refuses, consumes nothing.
        echo.paused = True
        time.sleep(2.0)
        send_req(link, ident, 3, fake_payload(400, 0))
        f = link.wait_type(9, correlation=3, timeout=4.0)
        check("l13_suspect_try_later", f is not None and f.status == S_TRYLATER, repr(f))
        echo.paused = False
        time.sleep(0.8)
        send_req(link, ident, 3, fake_payload(400, 0))
        a = link.wait_type(10, correlation=3, timeout=4.0)
        c = link.wait_type(12, correlation=3, timeout=5.0)
        check("l13_retry_after_proof", a is not None and c is not None and c.status == S_OK, "")

        # L14/L15: Pi stops answering >3 s during a job. The Flipper
        # keeps sending challenges (we count them) yet the lease dies.
        send_req(link, ident, 4, fake_payload(12000, 0))
        a = link.wait_type(10, correlation=4, timeout=4.0)
        check("l14_job_accepted", a is not None, repr(a))
        echo.paused = True
        seen0 = echo.challenges_seen
        t0 = time.monotonic()
        c = link.wait_type(12, correlation=4, timeout=9.0)
        mute_s = time.monotonic() - t0
        n_ch = echo.challenges_seen - seen0
        check("l14_expiry_stops_job", c is not None and c.status == ST_LINK_LOST,
              f"{repr(c)} after {mute_s:.1f}s")
        check("l15_flipper_kept_sending", n_ch >= 2, f"challenges={n_ch}")
        echo.paused = False
        time.sleep(0.3)

        # Resume: same boot, generation rotates, ledger survives.
        ident2 = resume_session(link, ident, NONCE_2)
        check("c09_resume", ident2 is not None and ident2[2] == ident[2] + 1,
              repr(ident2))
        if ident2 is None:
            return
        ident = ident2
        f = get_result(link, 961, ident, 4)
        ok = f is not None and len(f.payload) >= 4 and f.payload[0] == 1 \
            and struct.unpack_from("<H", f.payload, 2)[0] == ST_LINK_LOST
        check("c09_ledger_survives_resume", ok, repr(f))

        # L18: an action framed with the dead generation is refused.
        send_req(link, (ident[0], ident[1], ident[2] - 1), 5, fake_payload(300, 0))
        f = link.wait_type(9, correlation=5, timeout=4.0)
        check("l18_old_generation_rejected", f is not None and f.status == 7, repr(f))
        send_req(link, ident, 5, fake_payload(300, 0))
        a = link.wait_type(10, correlation=5, timeout=4.0)
        c = link.wait_type(12, correlation=5, timeout=4.0)
        check("c09_next_seq_admitted", a is not None and c is not None and c.status == S_OK, "")

        # L19: duplicate CONFIRM returns the same READY (no gen loop).
        f = do_hello(link, 770, CLIENT_A, NONCE_2,
                     prior_boot=ident[0], prior_session=ident[1])
        rep = parse_hello_reply(f) if f is not None and f.status == 0 else None
        check("l19_rehello_proposal", rep is not None, repr(f))
        if rep is not None:
            do_confirm(link, 771, CLIENT_A, NONCE_2, rep["challenge"],
                       rep["boot"], rep["session"], rep["gen"])
            r1 = link.wait_type(bc.T_SESSION_READY, correlation=771)
            do_confirm(link, 772, CLIENT_A, NONCE_2, rep["challenge"],
                       rep["boot"], rep["session"], rep["gen"])
            r2 = link.wait_type(bc.T_SESSION_READY, correlation=772)
            ok = r1 is not None and r2 is not None and r1.generation == r2.generation
            check("l19_duplicate_confirm_same_ready", ok,
                  f"{repr(r1)} {repr(r2)}")

        # L16/L17: replaying a stale echo cannot keep the lease alive.
        send_req(link, ident, 6, fake_payload(12000, 0))
        a = link.wait_type(10, correlation=6, timeout=4.0)
        check("l17_job_accepted", a is not None, repr(a))
        echo.held = True  # main thread owns the wire now
        chs = []
        t_end = time.monotonic() + 2.5
        while time.monotonic() < t_end and not chs:
            link._pump(0.3)
            chs = link.drain_challenges()
        stale_counter = struct.unpack("<Q", chs[-1].payload)[0] if chs else None
        check("l17_challenge_captured", stale_counter is not None, "")
        time.sleep(1.3)  # let the captured counter age past proof_age
        t0 = time.monotonic()
        blasted = 0
        while time.monotonic() - t0 < 3.6 and stale_counter is not None:
            link.send_id(ident[0], ident[1], ident[2], bc.T_ECHO, 0, 0,
                         struct.pack("<Q", stale_counter))
            blasted += 1
            link._pump(0.1)
        c = None
        t_end = time.monotonic() + 5.0
        while time.monotonic() < t_end and c is None:
            link._pump(0.1)
            c = link._pop_match(12, 6)
        check("l17_replay_storm_lease_dies",
              c is not None and c.status == ST_LINK_LOST,
              f"blasted={blasted} {repr(c)}")
        echo.held = False
        time.sleep(0.3)
        ident3 = resume_session(link, ident, NONCE_1, corr0=780)
        check("c09_resume_after_replay", ident3 is not None, repr(ident3))
        if ident3 is not None:
            ident = ident3

        # L25: frames lost in transit must surface as a sequence gap
        # at the client, never as a complete capture. The harness
        # deliberately discards alternate event batches (simulating
        # byte loss); the COMPLETE summary's generated count is the
        # truth it reconciles against. (Producer-side drop counting
        # under a full pool is native-proven in C05 queue-full-stop.)
        send_req(link, ident, 7, fake_payload(3000, 2))
        a = link.wait_type(10, correlation=7, timeout=4.0)
        kept = []
        t_end = time.monotonic() + 1.5
        flip = False
        while time.monotonic() < t_end:
            batch = link.pop_all(11, correlation=7)
            if flip:
                kept.extend(batch)
            flip = not flip
            time.sleep(0.03)
        f = send_cancel(link, ident, 962, 7)
        c = link.wait_type(12, correlation=7, timeout=5.0)
        kept += link.pop_all(11, correlation=7)
        gen_n = struct.unpack_from("<I", c.payload, 2)[0] if c is not None and len(c.payload) >= 10 else -1
        drop_n = struct.unpack_from("<I", c.payload, 6)[0] if c is not None and len(c.payload) >= 10 else -1
        seqs = [e.event_seq for e in kept]
        holes = (max(seqs) - min(seqs) + 1 - len(seqs)) if seqs else 0
        check("l25_terminal_exposes_gap",
              c is not None and c.status == ST_CANCELLED and drop_n == 0
              and gen_n > len(seqs) and holes > 0,
              f"gen={gen_n} kept={len(seqs)} holes={holes} dropped={drop_n}")

        # L10: 600 bytes of delimiter-less junk, a delimiter to end
        # the oversize discard, then the next frame must parse clean.
        link.send(0, 0, 0, b"", raw=b"A" * 600 + b"\x00")
        link.send(bc.T_QUERY, bc.OP_PING, 963, bytes([3]) + b"abc")
        f = link.wait_frame(963, timeout=4.0)
        check("l10_junk_bounded_resync", f is not None and f.status == S_OK, repr(f))
    finally:
        echo.stop()

    # L20: the Pi client dies mid-session. A new client instance must
    # not silently take over the live session; after expiry it takes
    # over and the old job's outcome is unavailable to it.
    ident_b = None
    link2 = None
    try:
        send_req(link, ident, 8, fake_payload(15000, 0))
        a = link.wait_type(10, correlation=8, timeout=4.0)
        check("l20_job_running_before_death", a is not None, repr(a))
        link.close()  # client process death: no goodbye
        time.sleep(0.4)
        link2 = Link(link.ser.port, 230400)
        f = do_hello(link2, 880, CLIENT_B, NONCE_1)
        check("l20_no_silent_takeover", f is not None and f.status == S_BUSY, repr(f))
        ok = False
        for _try in range(6):
            time.sleep(1.2)
            ident_b = handshake_a(link2, NONCE_1, CLIENT_B)
            if ident_b is not None:
                ok = True
                break
        check("l20_takeover_after_expiry", ok, repr(ident_b))
        if ident_b is not None:
            f = get_result(link2, 964, ident_b, 8)
            ok = f is not None and len(f.payload) >= 1 and f.payload[0] == 0
            check("l20_old_outcome_unavailable", ok, repr(f))
            echo2 = EchoThread(link2)
            echo2.start()
            try:
                send_req(link2, ident_b, 1, fake_payload(300, 0))
                a = link2.wait_type(10, correlation=1, timeout=4.0)
                c = link2.wait_type(12, correlation=1, timeout=4.0)
                check("l20_new_client_works", a is not None and c is not None
                      and c.status == S_OK, "")
            finally:
                echo2.stop()
    finally:
        if link2 is not None:
            link2.close()


def case_c09r(link):
    """L21: FAP restarts with an unresolved action. The client must
    classify it INDETERMINATE_AFTER_RESTART and never resubmit it."""
    import os
    import subprocess
    import sys

    def get_info(c):
        link.send(bc.T_QUERY, bc.OP_GET_INFO, c, bytes([0]))
        return link.wait_frame(c, timeout=4.0)

    f = get_info(880)
    boot1 = f.boot if f is not None else None
    check("c09r_boot1", boot1 is not None, repr(f))
    ident = handshake_a(link, NONCE_1)
    check("c09r_session", ident is not None, "")
    if ident is None or boot1 is None:
        return
    echo = EchoThread(link)
    echo.start()
    try:
        # Idle until ~35 s into the app's life, then start a 60 s job:
        # the 90 s auto-exit lands while it is still unresolved.
        time.sleep(30)
        send_req(link, ident, 1, fake_payload(60000, 1000))
        a = link.wait_type(10, correlation=1, timeout=4.0)
        check("c09r_job_unresolved_at_exit", a is not None, repr(a))
        # Poll the bootstrap PING until the app is gone (2 timeouts).
        gone = 0
        c = 890
        t0 = time.monotonic()
        while time.monotonic() - t0 < 90 and gone < 2:
            c += 1
            link.send(bc.T_QUERY, bc.OP_PING, c, bytes([2]) + b"hi")
            r = link.wait_frame(c, timeout=1.5)
            gone = 0 if (r is not None and r.op == bc.OP_PING) else gone + 1
            time.sleep(1.0)
        check("c09r_app_exit_detected", gone >= 2, f"after {time.monotonic()-t0:.0f}s")
    finally:
        echo.stop()
    link.close()

    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run([sys.executable, os.path.join(here, "spike_launch.py")],
                       capture_output=True, text=True, timeout=90)
    check("c09r_relaunch", "APP_START_OK" in (r.stdout or ""), (r.stdout or "")[-120:])

    link2 = Link(link.ser.port if hasattr(link, "ser") else "/dev/ttyAMA0", 230400)
    try:
        link2.send(bc.T_QUERY, bc.OP_GET_INFO, 900, bytes([0]))
        f = link2.wait_frame(900, timeout=5.0)
        boot2 = f.boot if f is not None else None
        check("c09r_new_boot", boot2 is not None and boot2 != boot1, "")
        # The old identity means nothing to the new boot.
        link2.send_id(boot1, ident[1], ident[2], bc.T_QUERY, bc.OP_GET_RESULT,
                      901, struct.pack("<Q", 1))
        f = link2.wait_frame(901, timeout=4.0)
        check("c09r_old_outcome_unavailable",
              f is not None and f.status == ST_OUTCOME_UNAVAILABLE, repr(f))
        print("INDETERMINATE_AFTER_RESTART: old seq 1 not resubmitted", flush=True)
        ident2 = handshake_a(link2, NONCE_1)
        check("c09r_fresh_session", ident2 is not None, "")
        if ident2 is not None:
            echo2 = EchoThread(link2)
            echo2.start()
            try:
                send_req(link2, ident2, 1, fake_payload(400, 0))
                a = link2.wait_type(10, correlation=1, timeout=4.0)
                cc = link2.wait_type(12, correlation=1, timeout=5.0)
                check("c09r_new_action_runs", a is not None and cc is not None
                      and cc.status == S_OK, "")
                recs, ok = fetch_trace(link2, ident2)
                starts = sum(1 for x in recs if x["code"] == 10)
                check("c09r_only_one_execution", ok and starts == 1,
                      f"job_started_events={starts}")
            finally:
                echo2.stop()
    finally:
        link2.close()


OP_GPIO_CONFIG, OP_GPIO_READ, OP_GPIO_WRITE, OP_GPIO_RELEASE = 0x0101, 0x0102, 0x0103, 0x0104
S_RES_UNAVAIL = 12


def gpio_config_payload(pin, mode, pull, initial, hold_ms):
    return struct.pack("<BBBBI", pin, mode, pull, initial, hold_ms)


def action_outcome(link, seq, timeout=5.0):
    """One action: (12, COMPLETE) if it ran, (9, RESULT) if it was a
    consumed rejection, None on silence."""
    first = link.wait_frame(seq, timeout=timeout)
    if first is None:
        return None
    if first.type == 10:  # ACCEPTED -> the COMPLETE follows
        return link.wait_type(12, correlation=seq, timeout=timeout)
    return first


def result_status(frame):
    if frame is None or len(frame.payload) < 2:
        return None
    return struct.unpack_from("<H", frame.payload, 0)[0]


class Observer:
    """Independent witness on the Pi side of the bench wire (Pi GPIO17,
    physical pin 11 <-> Flipper header pin 2). pinctrl via raspi-utils."""

    def __init__(self, gpio=17):
        self.gpio = gpio

    def _run(self, *args):
        import subprocess
        out = subprocess.run(
            ["pinctrl", *[str(a) for a in args]],
            capture_output=True, text=True, timeout=5)
        return out.stdout

    def watch(self):
        """Become a weak-pull-down input: a driven Flipper pin wins, a
        released (analog) pin reads low. Distinguishes both."""
        self._run("set", self.gpio, "ip", "pd")

    def drive(self, level):
        self._run("set", self.gpio, "op", "dh" if level else "dl")

    def idle(self):
        self._run("set", self.gpio, "ip", "pn")

    def level(self):
        out = self._run("get", self.gpio)
        import re
        m = re.search(r"\|\s*(hi|lo)\b", out)
        return (1 if m.group(1) == "hi" else 0) if m else None

    def wait_level(self, want, timeout=3.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.level() == want:
                return True
            time.sleep(0.05)
        return False


def case_c10(link):
    obs = Observer()
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    # Bootstrap: build + capabilities enumeration.
    corr = 610
    link.send(bc.T_QUERY, bc.OP_GET_INFO, corr, bytes([0]))
    f = link.wait_frame(corr)
    build = ""
    if f is not None and len(f.payload) >= 17:
        blen = f.payload[16]
        build = f.payload[17 : 17 + blen].decode("ascii", "replace")
    check("get_info_c10_build", build == "mb-0.8-c10-1", build)

    link.send(bc.T_QUERY, bc.OP_GET_CAPABILITIES, corr + 1, struct.pack("<H", 0))
    f = link.wait_frame(corr + 1)
    caps = {}
    ok = f is not None and len(f.payload) >= 4 and f.payload[0] == 3
    if ok:
        next_cursor = struct.unpack_from("<H", f.payload, 1)[0]
        count = f.payload[3]
        ok = next_cursor == 0xFFFF and len(f.payload) == 4 + count * 7
        for i in range(count):
            op, kind, limit = struct.unpack_from("<HBI", f.payload, 4 + i * 7)
            caps[op] = (kind, limit)
    check("capabilities_page", ok and caps.get(OP_GPIO_CONFIG) == (1, 60000)
          and caps.get(OP_GPIO_READ, (None,))[0] == 1
          and caps.get(OP_FAKE_RUN, (None,))[0] == 1
          and 0x0004 not in caps, repr(caps))
    link.send(bc.T_QUERY, bc.OP_GET_CAPABILITIES, corr + 2, b"\x00")
    f = link.wait_frame(corr + 2)
    check("capabilities_bad_len", f is not None and f.status == S_INVAL, repr(f))

    ident = handshake_a(link, NONCE_1)
    check("session_for_gpio", ident is not None, "")
    if ident is None:
        return
    echo = EchoThread(link)
    echo.start()
    try:
        obs.watch()
        # Output direction: config low, write high/low/high, all seen.
        s = next_seq()
        send_req(link, ident, s, gpio_config_payload(2, 1, 0, 0, 30000), op=OP_GPIO_CONFIG)
        f = action_outcome(link, s)
        ok = (f is not None and f.status == S_OK and f.payload[2:] == bytes([2, 1, 0]))
        check("config_output_low", ok, repr(f))
        check("obs_sees_low", obs.wait_level(0), f"level={obs.level()}")
        for value in (1, 0, 1):
            s = next_seq()
            send_req(link, ident, s, bytes([2, value]), op=OP_GPIO_WRITE)
            f = action_outcome(link, s)
            ok = (f is not None and f.status == S_OK
                  and f.payload[2:] == bytes([2, value]))
            check(f"write_{value}_complete", ok, repr(f))
            check(f"obs_sees_{value}", obs.wait_level(value), f"level={obs.level()}")
        dup = f
        s_dup = s
        send_req(link, ident, s_dup, bytes([2, 1]), op=OP_GPIO_WRITE)
        f = action_outcome(link, s_dup)
        check("dup_write_replayed",
              f is not None and dup is not None and f.payload == dup.payload, repr(f))

        # Semantic refusals: consumed, and the pin never changes state.
        refusals = [
            ("conflict_input_mode", OP_GPIO_CONFIG, gpio_config_payload(2, 0, 0, 0, 0), S_RES_UNAVAIL),
            ("read_of_output", OP_GPIO_READ, bytes([2]), S_INVAL),
            ("write_reserved_pin13", OP_GPIO_WRITE, bytes([13, 1]), S_INVAL),
            ("write_value_2", OP_GPIO_WRITE, bytes([2, 2]), S_INVAL),
            ("config_pin1_power", OP_GPIO_CONFIG, gpio_config_payload(1, 1, 0, 1, 1000), S_INVAL),
            ("config_pin17_ibutton", OP_GPIO_CONFIG, gpio_config_payload(17, 1, 0, 1, 1000), S_INVAL),
            ("config_mode_2", OP_GPIO_CONFIG, gpio_config_payload(2, 2, 0, 0, 0), S_INVAL),
            ("config_output_pull", OP_GPIO_CONFIG, gpio_config_payload(2, 1, 1, 1, 1000), S_INVAL),
            ("config_input_initial", OP_GPIO_CONFIG, gpio_config_payload(2, 0, 0, 1, 0), S_INVAL),
            ("config_hold_too_big", OP_GPIO_CONFIG, gpio_config_payload(2, 1, 0, 1, 60001), S_INVAL),
        ]
        for name, op, payload, want in refusals:
            s = next_seq()
            send_req(link, ident, s, payload, op=op)
            f = action_outcome(link, s)
            check(name, f is not None and f.type == 9
                  and f.status == want, repr(f))
        check("pin_undisturbed_by_refusals", obs.wait_level(1), f"level={obs.level()}")

        # Hold expiry: a mid-hold write does NOT buy more time.
        s = next_seq()
        send_req(link, ident, s, bytes([2]), op=OP_GPIO_RELEASE)
        f = action_outcome(link, s)
        check("release_before_hold_test", f is not None and f.status == S_OK, repr(f))
        check("obs_low_after_release", obs.wait_level(0), f"level={obs.level()}")
        t_cfg = time.monotonic()
        s = next_seq()
        send_req(link, ident, s, gpio_config_payload(2, 1, 0, 1, 2500), op=OP_GPIO_CONFIG)
        f = action_outcome(link, s)
        check("config_short_hold", f is not None and f.status == S_OK, repr(f))
        check("obs_high_hold", obs.wait_level(1), f"level={obs.level()}")
        while time.monotonic() - t_cfg < 1.4:
            time.sleep(0.05)
        s = next_seq()
        send_req(link, ident, s, bytes([2, 1]), op=OP_GPIO_WRITE)
        f = action_outcome(link, s)
        check("mid_hold_write", f is not None and f.status == S_OK, repr(f))
        expired = obs.wait_level(0, timeout=2.0)
        dt = time.monotonic() - t_cfg
        check("hold_expires_unrefreshed", expired and dt < 3.4, f"dt={dt:.2f}")

        # Input direction: the Pi drives, the Flipper reads.
        s = next_seq()
        send_req(link, ident, s, gpio_config_payload(2, 0, 0, 0, 0), op=OP_GPIO_CONFIG)
        f = action_outcome(link, s)
        check("config_input", f is not None and f.status == S_OK
              and f.payload[2:] == bytes([2, 0, 0]), repr(f))
        for drive, name in ((1, "high"), (0, "low")):
            obs.drive(drive)
            time.sleep(0.2)
            s = next_seq()
            send_req(link, ident, s, bytes([2]), op=OP_GPIO_READ)
            f = action_outcome(link, s)
            ok = (f is not None and f.status == S_OK
                  and f.payload[2:] == bytes([2, 0, 0, drive]))
            check(f"read_input_{name}", ok, repr(f))
        s = next_seq()
        send_req(link, ident, s, bytes([2, 1]), op=OP_GPIO_WRITE)
        f = action_outcome(link, s)
        check("write_to_input_refused", f is not None and f.type == 9
              and f.status == S_INVAL, repr(f))
        s = next_seq()
        send_req(link, ident, s, bytes([2]), op=OP_GPIO_RELEASE)
        f = action_outcome(link, s)
        check("release_input", f is not None and f.status == S_OK, repr(f))
        obs.watch()

        # Lease loss: the pin is restored even though nobody asked.
        s = next_seq()
        send_req(link, ident, s, gpio_config_payload(2, 1, 0, 1, 30000), op=OP_GPIO_CONFIG)
        f = action_outcome(link, s)
        check("config_before_lease_loss", f is not None and f.status == S_OK, repr(f))
        check("obs_high_before_loss", obs.wait_level(1), f"level={obs.level()}")
        echo.stop()
        time.sleep(3.6)  # lease expires at 3 s; revocation restores pins
        check("obs_released_on_lease_loss", obs.wait_level(0, timeout=2.0),
              f"level={obs.level()}")
        ident2 = resume_session(link, ident, NONCE_2)
        check("resume_after_loss", ident2 is not None, "")
        if ident2 is not None:
            echo2 = EchoThread(link)
            echo2.start()
            try:
                s = next_seq()
                send_req(link, ident2, s, bytes([2]), op=OP_GPIO_READ)
                f = action_outcome(link, s)
                check("pin_stays_unclaimed",
                      f is not None and f.type == 9 and f.status == S_INVAL,
                      repr(f))
            finally:
                echo2.stop()
    finally:
        echo.stop()
        obs.idle()


OP_ADC_READ, OP_NOTIFY, OP_GET_CAPABILITIES = 0x0201, 0x0301, 0x0003
S_RES_UNAVAIL_C11 = 12


def case_c11(link):
    """C11: ADC reads against Pi-driven levels on header pin 2;
    finite notification effects (effects observed by the human at the
    bench; wire side checks completion, bounds, and no-dup)."""
    obs = Observer()
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC11ADC01).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    try:
        check("c11_session", ident is not None, repr(ident))
        if ident is None:
            return

        # Capabilities: ADC + NOTIFY pages present, GET_STATUS absent.
        link.send_id(ZERO16, ZERO16, 0, bc.T_QUERY, OP_GET_CAPABILITIES, 990,
                     struct.pack("<H", 0))
        caps = link.wait_frame(990, timeout=5.0)
        ops = []
        if caps is not None:
            body = caps.payload[4:]
            for i in range(len(body) // 7):
                ops.append(struct.unpack_from("<HB I", body, i * 7))
        by_op = {o: (k, lim) for o, k, lim in ops}
        check("caps_adc_notify",
              by_op.get(OP_ADC_READ) == (1, 16)
              and by_op.get(OP_NOTIFY) == (1, 250)
              and 0x0004 not in by_op, repr(ops))

        def adc_read(pin, samples, s=None):
            s = s or next_seq()
            send_req(link, ident, s, struct.pack("<BB", pin, samples),
                     op=OP_ADC_READ)
            return s, action_outcome(link, s)

        def adc_mv(f):
            # status:u16, pin:u8, channel:u8, samples:u8, raw:u16, mv:u16
            if f is None or len(f.payload) < 9:
                return None
            return struct.unpack_from("<H", f.payload, 7)[0], \
                struct.unpack_from("<H", f.payload, 5)[0], f.payload[3]

        # Drive 0 V; 1, 4, and 16 samples all read near zero.
        obs.drive(0)
        time.sleep(0.1)
        s, f = adc_read(2, 4)
        info = adc_mv(f)
        check("adc_gnd_4", f is not None and f.type == 12
              and result_status(f) == S_OK and info is not None
              and info[0] < 100 and info[2] == 12, repr(f))
        s, f = adc_read(2, 1)
        check("adc_gnd_1", f is not None and result_status(f) == S_OK,
              repr(f))
        s, f = adc_read(2, 16)
        info = adc_mv(f)
        check("adc_gnd_16", f is not None and result_status(f) == S_OK
              and info is not None and info[0] < 100, repr(f))

        # Drive 3.3 V: saturates the 0-2048 mV scale (documented).
        obs.drive(1)
        time.sleep(0.1)
        s, f = adc_read(2, 8)
        info = adc_mv(f)
        check("adc_33v_saturated", f is not None and result_status(f) == S_OK
              and info is not None and info[0] >= 1950 and info[1] >= 3900,
              repr((f, info)))
        obs.drive(0)
        time.sleep(0.1)
        s, f = adc_read(2, 4)
        info = adc_mv(f)
        check("adc_recovers_low", f is not None and info is not None
              and info[0] < 100, repr(f))

        # Rejections: bad sample counts and non-ADC/reserved pins.
        for name, pin, samples, want in (
                ("adc_zero_samples", 2, 0, S_INVAL),
                ("adc_17_samples", 2, 17, S_INVAL),
                ("adc_pin5_no_adc", 5, 4, S_INVAL),
                ("adc_pin13_reserved", 13, 4, S_INVAL)):
            s, f = adc_read(pin, samples)
            check(name, f is not None and f.status == want
                  and (f.type != 10), repr(f))

        # Claim conflict: a GPIO-claimed pin refuses ADC; release frees
        # it; ADC leaves the pin unclaimed afterwards (CONFIG works).
        s = next_seq()
        send_req(link, ident, s, gpio_config_payload(2, 0, 0, 0, 0),
                 op=OP_GPIO_CONFIG)
        f = action_outcome(link, s)
        check("adc_claim_setup", f is not None and result_status(f) == S_OK,
              repr(f))
        s, f = adc_read(2, 4)
        check("adc_claimed_conflict", f is not None
              and f.status == S_RES_UNAVAIL_C11 and f.type != 10, repr(f))
        s = next_seq()
        send_req(link, ident, s, bytes([2]), op=OP_GPIO_RELEASE)
        f = action_outcome(link, s)
        check("adc_claim_release", f is not None and result_status(f) == S_OK,
              repr(f))
        s, f = adc_read(2, 4)
        check("adc_after_release", f is not None and result_status(f) == S_OK,
              repr(f))
        s = next_seq()
        send_req(link, ident, s, gpio_config_payload(2, 1, 0, 0, 60000),
                 op=OP_GPIO_CONFIG)
        f = action_outcome(link, s)
        check("adc_left_pin_free", f is not None and result_status(f) == S_OK,
              repr(f))
        s = next_seq()
        send_req(link, ident, s, bytes([2]), op=OP_GPIO_RELEASE)
        action_outcome(link, s)

        # NOTIFY: validation, all three finite effects, dup = no re-run.
        def notify(effect, s=None):
            s = s or next_seq()
            send_req(link, ident, s, bytes([effect]), op=OP_NOTIFY)
            return s, action_outcome(link, s)

        for name, effect, want in (("notify_effect0", 0, S_INVAL),
                                   ("notify_effect4", 4, S_INVAL)):
            s, f = notify(effect)
            check(name, f is not None and f.status == want and f.type != 10,
                  repr(f))
        for name, effect in (("notify_green_flash", 1),
                             ("notify_short_beep", 2),
                             ("notify_short_vibro", 3)):
            t0 = time.monotonic()
            s, f = notify(effect)
            dt = time.monotonic() - t0
            check(name, f is not None and f.type == 12
                  and result_status(f) == S_OK and dt < 1.0,
                  f"{repr(f)} dt={dt:.2f}")

        # Concurrency: PING answered while an effect plays.
        s_notify = next_seq()
        send_req(link, ident, s_notify, bytes([2]), op=OP_NOTIFY)
        t0 = time.monotonic()
        link.send(bc.T_QUERY, bc.OP_PING, 991, ping_payload(b"c11ping"))
        pong = link.wait_frame(991, timeout=2.0)
        pong_dt = time.monotonic() - t0
        check("notify_ping_concurrent", pong is not None and pong_dt < 0.5,
              f"dt={pong_dt:.2f}")
        f = link.wait_type(12, correlation=s_notify, timeout=2.0)
        check("notify_after_ping", f is not None
              and result_status(f) == S_OK, repr(f))

        # Duplicate NOTIFY replays the completion; no second effect.
        s = next_seq()
        send_req(link, ident, s, bytes([3]), op=OP_NOTIFY)
        first = action_outcome(link, s)
        send_req(link, ident, s, bytes([3]), op=OP_NOTIFY)
        dup = action_outcome(link, s)
        check("notify_dup_replay", first is not None and dup is not None
              and dup.type == 12 and dup.payload == first.payload, repr(dup))

        # Executor BUSY: ADC during a running fake job is consumed.
        s = next_seq()
        send_req(link, ident, s, fake_payload(3000, 500))
        check("busy_setup_accepted",
              link.wait_type(10, correlation=s, timeout=4.0) is not None, "")
        sdup, f = adc_read(2, 4)
        check("adc_busy", f is not None and f.status == S_BUSY, repr(f))
        done = link.wait_type(12, correlation=s, timeout=5.0)
        check("busy_job_completes", done is not None, repr(done))
    finally:
        obs.idle()
        echo.stop()


OP_IR_RX_START, OP_GET_CAPABILITIES_C12 = 0x0401, 0x0003
S_CANCELLED_C12, S_LINKLOST_C12 = 15, 16


def ir_summary(f):
    """COMPLETE payload: status:u16 + decoded/emitted/dropped:u32."""
    if f is None or len(f.payload) < 14:
        return None
    return struct.unpack_from("<HIII", f.payload, 0)


def rx_collect(link, seq, timeout=15.0):
    """Collect EVENT frames for one RX job until its COMPLETE."""
    events, complete = [], None
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        f = link.wait_frame(seq, timeout=max(0.1, end - time.monotonic()))
        if f is None:
            break
        if f.type == 11 and len(f.payload) >= 14:
            ev_seq, proto, rep = struct.unpack_from("<IBB", f.payload, 0)
            addr, cmd = struct.unpack_from("<II", f.payload, 6)
            events.append((ev_seq, proto, rep, addr, cmd))
        elif f.type == 12:
            complete = f
            break
    return events, complete


def case_c12(link):
    """C12: decoded NEC receive. Choreography (told to the human at
    the bench): during Job A (~10 s), 3 short presses of one remote
    button then a ~2 s hold, pointed at the Flipper's top edge."""
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC12DEC01).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    try:
        check("c12_session", ident is not None, repr(ident))
        if ident is None:
            return

        link.send_id(ZERO16, ZERO16, 0, bc.T_QUERY,
                     OP_GET_CAPABILITIES_C12, 990, struct.pack("<H", 0))
        caps = link.wait_frame(990, timeout=5.0)
        by_op = {}
        if caps is not None:
            body = caps.payload[4:]
            for i in range(len(body) // 7):
                o, k, lim = struct.unpack_from("<HBI", body, i * 7)
                by_op[o] = (k, lim)
        check("caps_ir", by_op.get(OP_IR_RX_START) == (1, 60000),
              repr(by_op.get(OP_IR_RX_START)))

        def rx_start(filt, timeout_ms, s=None):
            s = s or next_seq()
            send_req(link, ident, s,
                     struct.pack("<HI", filt, timeout_ms),
                     op=OP_IR_RX_START)
            return s

        # Rejection matrix: unqualified protocol / bad timeout.
        for name, filt, tmo in (("ir_filter2", 2, 10000),
                                ("ir_timeout0", 1, 0),
                                ("ir_timeout60001", 1, 60001)):
            s = rx_start(filt, tmo)
            f = action_outcome(link, s)
            check(name, f is not None and f.status == S_INVAL
                  and f.type != 10, repr(f))

        # Job A: real remote capture (human presses at the bench).
        print("JOB A: point the remote at the Flipper and press now "
              "(3 short + 1 hold)", flush=True)
        s = rx_start(1, 15000)
        events, complete = rx_collect(link, s, timeout=21.0)
        plain = [e for e in events if e[2] == 0]
        repeats = [e for e in events if e[2] == 1]
        summ = ir_summary(complete)
        check("ir_a_events", len(plain) >= 1, repr(events))
        check("ir_a_repeat_seen", len(repeats) >= 1, repr(events))
        check("ir_a_protocol_nec", all(e[1] == 1 for e in events),
              repr(events))
        check("ir_a_seqs_contiguous",
              [e[0] for e in events] == list(range(1, len(events) + 1)),
              repr([e[0] for e in events]))
        check("ir_a_summary", complete is not None
              and result_status(complete) == S_OK and summ is not None
              and summ[1] >= len(events) and summ[1] == summ[2] + summ[3],
              repr(complete))

        # Job B: cancel mid-receive.
        s = rx_start(1, 30000)
        acc = link.wait_type(10, correlation=s, timeout=4.0)
        check("ir_b_accepted", acc is not None, "")
        time.sleep(2.0)
        r = send_cancel(link, ident, 900, s)
        check("ir_b_cancel_ack", r is not None and r.status == 1, repr(r))
        evs, comp = rx_collect(link, s, timeout=6.0)
        check("ir_b_cancelled", comp is not None
              and result_status(comp) == S_CANCELLED_C12, repr(comp))

        # Job C: no-signal timeout completes OK with zero decodes.
        s = rx_start(1, 4000)
        evs, comp = rx_collect(link, s, timeout=10.0)
        summ = ir_summary(comp)
        check("ir_c_timeout_ok", comp is not None
              and result_status(comp) == S_OK and evs == []
              and summ is not None and summ[1] == 0, repr((comp, evs)))

        # Job D: link expiry stops the job; outcome survives resume.
        s = rx_start(1, 20000)
        acc = link.wait_type(10, correlation=s, timeout=4.0)
        check("ir_d_accepted", acc is not None, "")
        echo.paused = True
        time.sleep(6.0)
        echo.paused = False
        ident2 = resume_session(link, ident,
                                (0xC12DEC02).to_bytes(16, "little"))
        check("ir_d_resumed", ident2 is not None, repr(ident2))
        if ident2 is not None:
            gr = get_result(link, 950, ident2, s)
            term_status = None
            if gr is not None and len(gr.payload) >= 4:
                term_status = struct.unpack_from("<H", gr.payload, 2)[0]
            check("ir_d_link_lost", term_status == S_LINKLOST_C12,
                  repr(gr))

        # Job E: a fresh RX job starts and completes after all that.
        s = next_seq()
        send_req(link, ident2, s,
                 struct.pack("<HI", 1, 3000), op=OP_IR_RX_START)
        evs, comp = rx_collect(link, s, timeout=8.0)
        check("ir_e_restart_ok", comp is not None
              and result_status(comp) == S_OK, repr(comp))
    finally:
        echo.stop()


def case_c12x(link):
    """C12X probe (no remote needed): is the bridge healthy after an
    IR-job lease expiry? cancel-ack, expiry, resume, GET_RESULT,
    fresh RX admission, GET_TRACE dump."""
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC12DEC11).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    try:
        check("c12x_session", ident is not None, repr(ident))
        if ident is None:
            return

        def rx_start(timeout_ms, s=None):
            s = s or next_seq()
            send_req(link, ident, s,
                     struct.pack("<HI", 1, timeout_ms),
                     op=OP_IR_RX_START)
            return s

        # Cancel mid-RX, capturing the RESULT ack this time.
        s = rx_start(30000)
        acc = link.wait_type(10, correlation=s, timeout=4.0)
        check("c12x_accepted", acc is not None, "")
        time.sleep(1.5)
        r = send_cancel(link, ident, 900, s)
        check("c12x_cancel_ack", r is not None and r.status == 1, repr(r))
        evs, comp = rx_collect(link, s, timeout=6.0)
        check("c12x_cancelled", comp is not None
              and result_status(comp) == S_CANCELLED_C12, repr(comp))

        # Expiry during an RX job.
        s = rx_start(20000)
        acc = link.wait_type(10, correlation=s, timeout=4.0)
        check("c12x_exp_accepted", acc is not None, "")
        echo.paused = True
        time.sleep(6.0)
        echo.paused = False
        ident2 = resume_session(link, ident,
                                (0xC12DEC12).to_bytes(16, "little"))
        check("c12x_resumed", ident2 is not None, repr(ident2))
        if ident2 is None:
            return
        gr = get_result(link, 950, ident2, s)
        term_status = None
        if gr is not None and len(gr.payload) >= 4:
            term_status = struct.unpack_from("<H", gr.payload, 2)[0]
        check("c12x_get_result", gr is not None, repr(gr))
        check("c12x_link_lost", term_status == S_LINKLOST_C12, repr(gr))

        # Fresh RX job after expiry + cleanup (resumed identity, and
        # the action sequence continues monotonically across the
        # resume — a jump is a correct SEQUENCE_GAP refusal).
        s = next_seq()
        send_req(link, ident2, s,
                 struct.pack("<HI", 1, 3000), op=OP_IR_RX_START)
        out = action_outcome(link, s)
        check("c12x_restart_admit", out is not None, repr(out))
        evs, comp = ([], out) if out is not None and out.type == 12 else ([], None)
        if out is not None and out.type == 12:
            comp = out
        check("c12x_restart_ok", comp is not None
              and result_status(comp) == S_OK, repr(comp))

        # Trace dump for the record.
        recs, ok = fetch_trace(link, ident2)
        codes = [r["code"] for r in recs] if recs else []
        check("c12x_trace_present", ok and len(codes) > 0, repr(codes))
    finally:
        echo.stop()


OP_IR_TX_DECODED = 0x0402


def tx_payload(protocol, address, command, frame_count, timeout_ms):
    return struct.pack("<HIIBI", protocol, address, command,
                       frame_count, timeout_ms)


def tx_summary(f):
    """COMPLETE payload: status:u16 + supplied:u32 + sent:u32."""
    if f is None or len(f.payload) < 10:
        return None
    return struct.unpack_from("<HII", f.payload, 0)


def case_c13(link):
    """C13: one finite NEC transmission + lost-reply proof. An
    independent Pi-side receiver (tools/witness_ir.py on GPIO27)
    counts physical frames: exactly one per admitted action, zero for
    the replayed duplicate, zero during the link-expiry window."""
    import json
    import os
    import subprocess

    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC13DEC01).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    wit = None
    try:
        check("c13_session", ident is not None, repr(ident))
        if ident is None:
            return

        link.send_id(ZERO16, ZERO16, 0, bc.T_QUERY,
                     OP_GET_CAPABILITIES_C12, 990, struct.pack("<H", 0))
        caps = link.wait_frame(990, timeout=5.0)
        by_op = {}
        if caps is not None:
            body = caps.payload[4:]
            for i in range(len(body) // 7):
                o, k, lim = struct.unpack_from("<HBI", body, i * 7)
                by_op[o] = (k, lim)
        check("caps_ir_tx", by_op.get(OP_IR_TX_DECODED) == (1, 60000),
              repr(by_op.get(OP_IR_TX_DECODED)))

        def tx(address, command, s=None, protocol=1, frame_count=1,
               timeout_ms=5000):
            s = s or next_seq()
            send_req(link, ident, s,
                     tx_payload(protocol, address, command, frame_count,
                                timeout_ms),
                     op=OP_IR_TX_DECODED)
            return s

        # Rejection matrix: every row a consumed INVALID_ARGUMENT,
        # never an ACCEPTED (nothing may reach the LED).
        for name, kw in (
                ("tx_fc0", dict(frame_count=0)),
                ("tx_fc2", dict(frame_count=2)),
                ("tx_addr_wide", dict(address=0x100)),
                ("tx_cmd_wide", dict(command=0x100)),
                ("tx_proto2", dict(protocol=2)),
                ("tx_timeout0", dict(timeout_ms=0)),
                ("tx_timeout60001", dict(timeout_ms=60001))):
            kw.setdefault("address", 0x04)
            kw.setdefault("command", 0x08)
            s = tx(**kw)
            f = action_outcome(link, s)
            check(name, f is not None and f.status == S_INVAL
                  and f.type != 10, repr(f))

        # Arm the independent witness, then transmit.
        out_path = "/tmp/witness_c13.json"
        try:
            os.unlink(out_path)
        except FileNotFoundError:
            pass
        wit = subprocess.Popen(
            ["/usr/bin/python3" if os.path.exists("/usr/bin/python3")
             else sys.executable,
             os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "witness_ir.py"),
             "--seconds", "34", "--out", out_path],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(2.0)  # witness armed before the first frame

        # Action A: exactly one NEC frame addr 4 cmd 8.
        sa = tx(0x04, 0x08)
        comp = action_outcome(link, sa, timeout=8.0)
        summ = tx_summary(comp)
        check("tx_a_complete", comp is not None and comp.type == 12
              and result_status(comp) == S_OK, repr(comp))
        check("tx_a_summary", summ is not None and summ[1] == 1
              and summ[2] == 1, repr(summ))

        # Lost-reply proof: retry the same action ID verbatim. The
        # ledger replays the retained COMPLETE; no second emission.
        send_req(link, ident, sa, tx_payload(1, 0x04, 0x08, 1, 5000),
                 op=OP_IR_TX_DECODED)
        rep = link.wait_frame(sa, timeout=5.0)
        check("tx_a_replay", rep is not None and rep.type == 12
              and result_status(rep) == S_OK
              and tx_summary(rep) == summ, repr(rep))

        # Action B: a genuinely new ID transmits again after cleanup.
        sb = tx(0x04, 0x08)
        comp = action_outcome(link, sb, timeout=8.0)
        summ = tx_summary(comp)
        check("tx_b_complete", comp is not None and comp.type == 12
              and result_status(comp) == S_OK and summ is not None
              and summ[1] == 1 and summ[2] == 1, repr(comp))

        # Link expiry + resume: reconciliation finds action A terminal
        # and the emitter stays silent through the whole window.
        echo.paused = True
        time.sleep(6.0)
        echo.paused = False
        ident2 = resume_session(link, ident,
                                (0xC13DEC02).to_bytes(16, "little"))
        check("tx_resumed", ident2 is not None, repr(ident2))
        if ident2 is not None:
            gr = get_result(link, 950, ident2, sa)
            term_status = None
            if gr is not None and len(gr.payload) >= 4:
                term_status = struct.unpack_from("<H", gr.payload, 2)[0]
            check("tx_a_get_result", term_status == S_OK, repr(gr))

        # Physical verdict from the independent receiver.
        try:
            wout, _ = wit.communicate(timeout=45)
        except subprocess.TimeoutExpired:
            wit.kill()
            wout, _ = wit.communicate()
        wit = None
        verdict = None
        try:
            with open(out_path) as fh:
                verdict = json.load(fh)
        except (OSError, ValueError):
            pass
        frames = verdict["frames"] if verdict else []
        data = [f for f in frames if f["kind"] == "data"]
        reps = [f for f in frames if f["kind"] == "repeat"]
        check("witness_heard", verdict is not None and verdict["edges"] > 0,
              (repr(verdict) if verdict else (wout or ""))[:300])
        check("witness_exactly_two_frames", len(data) == 2
              and all(f["addr"] == 0x04 and f["cmd"] == 0x08 for f in data),
              repr(frames))
        check("witness_no_repeats", len(reps) == 0, repr(frames))
    finally:
        if wit is not None:
            wit.kill()
        echo.stop()


def case_c14l(link):
    """C14L (L02 on a real module): drop the ACCEPTED of a running IR
    RX job, retry the identical request — the firmware replays
    ACCEPTED for the SAME job (no second worker, no second start),
    and the job still reaches exactly one terminal."""
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC14DEC01).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    try:
        check("c14l_session", ident is not None, repr(ident))
        if ident is None:
            return
        s = next_seq()
        payload = struct.pack("<HI", 1, 20000)
        send_req(link, ident, s, payload, op=OP_IR_RX_START)
        time.sleep(0.6)  # ACCEPTED arrives unread: "dropped"
        link.pop_all(10, correlation=s)  # discard whatever queued
        send_req(link, ident, s, payload, op=OP_IR_RX_START)  # retry
        acc = link.wait_type(10, correlation=s, timeout=4.0)
        check("l02_replay_accepted", acc is not None, repr(acc))
        r = send_cancel(link, ident, 900, s)
        check("l02_cancel_ack", r is not None and r.status == 1, repr(r))
        evs, comp = rx_collect(link, s, timeout=6.0)
        check("l02_single_terminal", comp is not None
              and result_status(comp) == S_CANCELLED_C12, repr(comp))
        summ = ir_summary(comp)
        check("l02_summary_sane", summ is not None and summ[1] == 0,
              repr(summ))
    finally:
        echo.stop()


def case_c14_cycles(link, module):
    """C14 lifecycle: 100 start/stop cycles of one module inside a
    single launch. Heap drift is judged from result.txt afterwards
    (heap_sf vs heap_ef written at this launch's exit)."""
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC14C7C01).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    ok_count = [0]
    try:
        check(f"cyc_{module}_session", ident is not None, repr(ident))
        if ident is None:
            return

        def run(payload, op):
            s = next_seq()
            send_req(link, ident, s, payload, op=op)
            f = action_outcome(link, s, timeout=8.0)
            if f is not None and f.type == 12 and result_status(f) == S_OK:
                ok_count[0] += 1
                return True
            print(f"cycle failure at {ok_count[0]}: {f!r}", flush=True)
            return False

        if module == "gpio":
            for _ in range(100):
                if not run(gpio_config_payload(2, 1, 0, 0, 0),
                           OP_GPIO_CONFIG):
                    break
                if not run(bytes([2]), OP_GPIO_RELEASE):
                    break
        elif module == "adc":
            for _ in range(100):
                if not run(struct.pack("<BB", 2, 4), OP_ADC_READ):
                    break
        elif module == "notify":
            for _ in range(100):
                if not run(bytes([1]), OP_NOTIFY):
                    break
        elif module == "irrx":
            for _ in range(100):
                s = next_seq()
                send_req(link, ident, s, struct.pack("<HI", 1, 300),
                         op=OP_IR_RX_START)
                evs, comp = rx_collect(link, s, timeout=8.0)
                if comp is not None and result_status(comp) == S_OK \
                        and evs == []:
                    ok_count[0] += 1
                else:
                    print(f"cycle failure at {ok_count[0]}: {comp!r}",
                          flush=True)
                    break
        want = 200 if module == "gpio" else 100  # gpio cycle = 2 actions
        check(f"cyc_{module}_100", ok_count[0] == want,
              f"ok={ok_count[0]}")
    finally:
        echo.stop()


def case_c14_soak(link):
    """C14 soak: 30 minutes of continuous IR receive. Normal jobs run
    the full 60 s window; every 5th job takes an injected 6 s link
    pause mid-flight (lease expiry -> LINK_LOST), then the session
    resumes and work continues. PING round-trip is sampled each job.
    Progress lines go to stdout for the detached runner's log."""
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC1450A01).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    t_start = time.monotonic()
    jobs_ok = jobs_lost = jobs_bad = pings = 0
    ping_ms = []
    try:
        check("soak_session", ident is not None, repr(ident))
        if ident is None:
            return
        job_no = 0
        while time.monotonic() - t_start < 1800:
            job_no += 1
            # PING latency sample (bootstrap query, no session state).
            t0 = time.monotonic()
            link.send_id(ZERO16, ZERO16, 0, bc.T_QUERY, bc.OP_PING, 990,
                         b"\x00\x00")
            pf = link.wait_frame(990, timeout=3.0)
            if pf is not None:
                pings += 1
                ping_ms.append((time.monotonic() - t0) * 1000.0)
            s = next_seq()
            send_req(link, ident, s, struct.pack("<HI", 1, 60000),
                     op=OP_IR_RX_START)
            if job_no % 5 == 0:
                acc = link.wait_type(10, correlation=s, timeout=4.0)
                time.sleep(20.0)
                echo.paused = True
                time.sleep(6.0)
                echo.paused = False
                ident2 = resume_session(
                    link, ident, (0xC1450A02 + job_no).to_bytes(16, "little"))
                if ident2 is None:
                    jobs_bad += 1
                    print(f"soak job {job_no}: RESUME FAILED", flush=True)
                    break
                ident = ident2
                gr = get_result(link, 950, ident, s)
                term = None
                if gr is not None and len(gr.payload) >= 4:
                    term = struct.unpack_from("<H", gr.payload, 2)[0]
                if term == S_LINKLOST_C12:
                    jobs_lost += 1
                else:
                    jobs_bad += 1
                print(f"soak job {job_no}: injected expiry term={term} "
                      f"elapsed={time.monotonic() - t_start:.0f}s",
                      flush=True)
            else:
                evs, comp = rx_collect(link, s, timeout=75.0)
                if comp is not None and result_status(comp) == S_OK:
                    jobs_ok += 1
                else:
                    jobs_bad += 1
                    print(f"soak job {job_no}: BAD {comp!r}", flush=True)
                print(f"soak job {job_no}: ok elapsed="
                      f"{time.monotonic() - t_start:.0f}s", flush=True)
        check("soak_jobs_ok", jobs_ok >= 20, f"ok={jobs_ok}")
        check("soak_injected_recovered", jobs_lost >= 4 and jobs_bad == 0,
              f"lost={jobs_lost} bad={jobs_bad}")
        if ping_ms:
            print(f"soak ping: n={len(ping_ms)} min={min(ping_ms):.1f}ms "
                  f"max={max(ping_ms):.1f}ms "
                  f"avg={sum(ping_ms) / len(ping_ms):.1f}ms", flush=True)
        check("soak_pings", pings >= 20, f"pings={pings}")
    finally:
        echo.stop()


def case_c14t(link):
    """C14 TX lifecycle: 100 finite NEC transmissions on one launch.
    The independent witness (started externally, see evidence) must
    count exactly 100 data frames and 0 repeats."""
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC14DEC13).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    ok = 0
    try:
        check("c14t_session", ident is not None, repr(ident))
        if ident is None:
            return
        for _ in range(100):
            s = next_seq()
            send_req(link, ident, s, tx_payload(1, 0x04, 0x08, 1, 5000),
                     op=OP_IR_TX_DECODED)
            comp = action_outcome(link, s, timeout=8.0)
            summ = tx_summary(comp)
            if comp is not None and result_status(comp) == S_OK \
                    and summ is not None and summ[1] == 1 and summ[2] == 1:
                ok += 1
            else:
                print(f"tx cycle failure at {ok}: {comp!r}", flush=True)
                break
        check("c14t_100", ok == 100, f"ok={ok}")
    finally:
        echo.stop()


def case_c14_l21a(link):
    """L21 phase A: admit one IR TX, deliberately never read its
    COMPLETE (the 'reply' is dropped), then wait while the human
    presses Back to restart the FAP. Phase B runs on the new boot."""
    seq = [0]

    def next_seq():
        seq[0] += 1
        return seq[0]

    nonce = (0xC14DEC21).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    try:
        check("l21a_session", ident is not None, repr(ident))
        if ident is None:
            return
        s = next_seq()
        send_req(link, ident, s, tx_payload(1, 0x04, 0x08, 1, 5000),
                 op=OP_IR_TX_DECODED)
        acc = link.wait_type(10, correlation=s, timeout=4.0)
        check("l21a_accepted", acc is not None, repr(acc))
        with open("/tmp/l21_seq.txt", "w") as fh:
            fh.write(str(s))
        print("L21: press Back on the Flipper ONCE now", flush=True)
        time.sleep(15.0)  # human exits the app; COMPLETE stays unread
    finally:
        echo.stop()


def case_c14_l21b(link):
    """L21 phase B (new boot): the old action must not be replayed or
    auto-retried; GET_RESULT for it reports OUTCOME_UNAVAILABLE, and
    the client records the outcome as indeterminate."""
    try:
        with open("/tmp/l21_seq.txt") as fh:
            old_seq = int(fh.read().strip())
    except (OSError, ValueError):
        old_seq = 1
    nonce = (0xC14DEC22).to_bytes(16, "little")
    ident = handshake_a(link, nonce)
    echo = EchoThread(link)
    echo.start()
    try:
        check("l21b_session_new_boot", ident is not None, repr(ident))
        if ident is None:
            return
        # Recovery path (zero envelope): the new boot simply has no
        # record of the old action — present=0 in the reply payload.
        gr = get_result(link, 950, ident, old_seq)
        check("l21b_not_present_new_boot",
              gr is not None and len(gr.payload) >= 1
              and gr.payload[0] == 0, repr(gr))
        # Identity path: a query carrying a foreign identity is
        # refused OUTCOME_UNAVAILABLE in the frame header.
        link.send_id(b"\xaa" * 16, b"\xbb" * 16, 1, bc.T_QUERY,
                     bc.OP_GET_RESULT, 951, struct.pack("<Q", old_seq))
        f = link.wait_frame(951, timeout=4.0)
        check("l21b_foreign_identity_23",
              f is not None and f.status == 23, repr(f))
    finally:
        echo.stop()


def case_c10b(link):
    """Manual proof: a claimed-high pin under a LIVE lease; the operator
    presses Back; the exit path restores the pin (observed on the Pi)."""
    obs = Observer()
    ident = handshake_a(link, NONCE_1)
    check("c10b_session", ident is not None, "")
    if ident is None:
        return
    echo = EchoThread(link)
    echo.start()
    try:
        obs.watch()
        send_req(link, ident, 1, gpio_config_payload(2, 1, 0, 1, 60000),
                 op=OP_GPIO_CONFIG)
        f = action_outcome(link, 1)
        check("c10b_claimed_high", f is not None and f.status == S_OK
              and obs.wait_level(1), repr(f))
        print("PIN_HIGH: press Back on the Flipper once, now", flush=True)
        t0 = time.monotonic()
        # Hold is 60 s; watching 85 s spans all three possible causes
        # (Back exit, hold expiry, auto-exit teardown). The harness dt
        # plus result.txt run_ms afterwards attributes the release.
        released = obs.wait_level(0, timeout=85.0)
        check("back_exit_restores_pin", released,
              f"after {time.monotonic() - t0:.1f}s")
    finally:
        echo.stop()
        obs.idle()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyAMA0")
    ap.add_argument("--baud", type=int, default=230400)
    ap.add_argument("--case", default="C04")
    args = ap.parse_args()
    link = Link(args.port, args.baud)
    try:
        if args.case == "C04":
            case_c04(link)
        elif args.case == "C06":
            case_c06(link)
        elif args.case == "C07":
            case_c07(link)
        elif args.case == "C08":
            case_c08(link)
        elif args.case == "BACK":
            case_back(link)
        elif args.case == "C09":
            case_c09(link)
        elif args.case == "C09R":
            case_c09r(link)
        elif args.case == "C10":
            case_c10(link)
        elif args.case == "C10B":
            case_c10b(link)
        elif args.case == "C11":
            case_c11(link)
        elif args.case == "C12":
            case_c12(link)
        elif args.case == "C12X":
            case_c12x(link)
        elif args.case == "C13":
            case_c13(link)
        elif args.case == "C14L":
            case_c14l(link)
        elif args.case == "C14G":
            case_c14_cycles(link, "gpio")
        elif args.case == "C14A":
            case_c14_cycles(link, "adc")
        elif args.case == "C14N":
            case_c14_cycles(link, "notify")
        elif args.case == "C14R":
            case_c14_cycles(link, "irrx")
        elif args.case == "SOAK":
            case_c14_soak(link)
        elif args.case == "C14T":
            case_c14t(link)
        elif args.case == "C14L21A":
            case_c14_l21a(link)
        elif args.case == "C14L21B":
            case_c14_l21b(link)
        else:
            print(f"unknown case {args.case}")
            return 2
    finally:
        link.close()
    failed = [n for n, ok in RESULTS if not ok]
    print(f"SUMMARY {len(RESULTS) - len(failed)}/{len(RESULTS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
