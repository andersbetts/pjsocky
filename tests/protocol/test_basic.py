#!/usr/bin/env python3
"""
Basic protocol regression test suite for pjsocky. Run directly against a
built binary:

    python3 tests/protocol/test_basic.py [path/to/pjsocky]

Defaults to ../../build/pjsocky relative to this file. Exits nonzero if
any test fails, printing a PASS/FAIL summary. No pytest dependency - see
client.py.

Each test gets its own freshly-started daemon process (not just a fresh
connection), so tests can't leak state into each other via the v1
single-account model (there's no "reset everything" command yet). This
costs roughly a daemon startup + shutdown per test (~1-2s each, see
CONTEXT.md's build-order step 5 notes on shutdown latency) - fine for
now; worth revisiting if this suite grows large enough for that to hurt.
"""
import json
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(__file__))
from client import Client, PjsockyProcess

SOCK_PATH = "/tmp/pjsocky-test.sock"

# Port 1 is reserved/unprivileged-inaccessible on any normal system, so
# connecting to it fails fast (connection refused) instead of timing
# out - used as a registrar address in tests that need a registration
# *attempt* to resolve quickly without a real SIP server available.
UNREACHABLE_REGISTRAR = "sip:127.0.0.1:1"


def test_hello_on_connect(c):
    # Client.__init__ already consumed it (see client.py) - just check
    # its shape is what docs/PROTOCOL.md promises.
    assert set(c.hello["data"].keys()) == {"protocol_version", "daemon_version"}, c.hello
    assert c.hello["data"]["protocol_version"].startswith("1."), c.hello


def test_ping(c):
    resp = c.call("ping")
    assert resp == {"id": "1", "ok": True, "result": {}}, resp


def test_status_get_idle(c):
    resp = c.call("status.get")
    assert resp["ok"] is True, resp
    assert resp["result"] == {
        "state": "idle", "acc_id": -1, "reg_status": 0, "call_id": -1
    }, resp


def test_unknown_command(c):
    resp = c.call("bogus")
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "unknown_command", resp


def test_bad_request_missing_cmd(c):
    # Bypasses Client.call() to send a request with no "cmd" at all.
    c.sock.sendall(b'{"id":"1"}\n')
    resp = c._recv_line()
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "bad_request", resp


def test_multiple_requests_one_connection(c):
    for _ in range(3):
        resp = c.call("ping")
        assert resp["ok"] is True, resp


def test_account_configure(c):
    resp = c.call("account.configure", {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    })
    assert resp["ok"] is True, resp
    assert resp["result"]["acc_id"] == 0, resp


def test_duplicate_configure_rejected(c):
    params = {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    }
    first = c.call("account.configure", params)
    assert first["ok"] is True, first

    second = c.call("account.configure", params)
    assert second["ok"] is False, second
    assert second["error"]["code"] == "invalid_state", second


def test_status_get_after_configure(c):
    c.call("account.configure", {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    })
    resp = c.call("status.get")
    assert resp["result"]["acc_id"] == 0, resp
    assert resp["result"]["state"] == "registering", resp


def test_account_configure_missing_param(c):
    resp = c.call("account.configure", {"sip_uri": "sip:1000@127.0.0.1"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_register_produces_reg_state_event(c):
    c.call("account.configure", {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    })

    resp = c.call("account.register")
    assert resp["ok"] is True, resp  # accepted for processing, not "succeeded"

    event = c.read_event()
    assert event["event"] == "reg_state", event
    assert event["data"]["acc_id"] == 0, event
    # Deliberately not asserting a specific SIP status code here: what a
    # refused-connection attempt maps to is a pjsip/network detail, not
    # part of pjsocky's contract. That the event arrives at all -
    # proving the cross-thread event-push path (proto/events.c) actually
    # works - is what this test checks.


def test_register_without_account_fails(c):
    resp = c.call("account.register")
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_state", resp


def test_device_list_audio(c):
    resp = c.call("device.list_audio")
    assert resp["ok"] is True, resp
    devices = resp["result"]["devices"]
    # Not asserting specific device names/counts - that's whatever audio
    # hardware/backend the test machine has. An *empty* list is
    # legitimate too: a truly headless box (e.g. a CI container) has no
    # ALSA devices at all, and the daemon reports that truthfully while
    # falling back to pjsua's null sound device internally (see main.c).
    # What matters here is the shape of the response.
    assert isinstance(devices, list), resp
    for dev in devices:
        assert set(dev.keys()) == {
            "id", "name", "input_channels", "output_channels"
        }, dev


def test_device_list_video(c):
    resp = c.call("device.list_video")
    assert resp["ok"] is True, resp
    devices = resp["result"]["devices"]
    assert isinstance(devices, list) and len(devices) > 0, resp
    for dev in devices:
        assert set(dev.keys()) == {"id", "name", "driver", "dir"}, dev
        assert dev["dir"] in ("capture", "render", "capture_render", "none"), dev


def test_device_set_audio(c):
    devices = c.call("device.list_audio")["result"]["devices"]
    combined = [d for d in devices if d["input_channels"] > 0 and d["output_channels"] > 0]
    if not combined:
        print("  (skipped: no single device with both input and output on this machine)")
        return

    dev_id = combined[0]["id"]
    resp = c.call("device.set_audio", {"capture_id": dev_id, "playback_id": dev_id})
    assert resp["ok"] is True, resp
    assert resp["result"] == {}, resp


def test_device_set_audio_missing_param(c):
    resp = c.call("device.set_audio", {"capture_id": 0})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_device_set_video(c):
    devices = c.call("device.list_video")["result"]["devices"]
    capture_devs = [d for d in devices if d["dir"] in ("capture", "capture_render")]
    assert capture_devs, "expected at least one capture-capable video device"

    resp = c.call("device.set_video", {"capture_id": capture_devs[0]["id"]})
    assert resp["ok"] is True, resp
    assert resp["result"] == {}, resp


# No SIP server is available in this environment (checked - only a SIP
# *library*, sofia-sip, is installed, not a server/registrar), so a real
# call can't be placed end to end here yet. These tests cover what's
# reachable without one: the registration gate on call.dial, and graceful
# handling of calls that don't exist. See CONTEXT.md's tests/protocol
# notes - a real loopback call is still a documented gap.


def test_dial_without_registration_fails(c):
    c.call("account.configure", {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    })
    # Deliberately not calling account.register - dial must be gated on
    # actually being registered, not just having an account configured.
    resp = c.call("call.dial", {"uri": "sip:2000@127.0.0.1:1"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_state", resp


def test_dial_with_no_account_fails(c):
    resp = c.call("call.dial", {"uri": "sip:2000@127.0.0.1:1"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_state", resp


def test_dial_missing_uri(c):
    resp = c.call("call.dial", {})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_hangup_all_noop_when_idle(c):
    resp = c.call("call.hangup_all")
    assert resp["ok"] is True, resp
    assert resp["result"] == {}, resp


def test_hangup_invalid_call_id(c):
    resp = c.call("call.hangup", {"call_id": 999})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_get_info_invalid_call_id(c):
    resp = c.call("call.get_info", {"call_id": 999})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_answer_invalid_call_id(c):
    # Same crash class as test_hangup_invalid_call_id/
    # test_get_info_invalid_call_id (see CONTEXT.md step 8's notes on
    # PJ_ASSERT_RETURN aborting the process on an out-of-range call_id) -
    # call.answer takes the same is_valid_call_id() guard in call.c.
    resp = c.call("call.answer", {"call_id": 999})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_answer_missing_call_id(c):
    resp = c.call("call.answer", {})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_im_send_without_registration_fails(c):
    c.call("account.configure", {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    })
    # Deliberately not registering - same gate as call.dial.
    resp = c.call("im.send", {"to": "sip:2000@127.0.0.1:1", "content": "hi"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_state", resp


def test_im_send_with_no_account_fails(c):
    resp = c.call("im.send", {"to": "sip:2000@127.0.0.1:1", "content": "hi"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_state", resp


def test_im_send_missing_params(c):
    resp = c.call("im.send", {"to": "sip:2000@127.0.0.1:1"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp

    resp = c.call("im.send", {"content": "hi"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_account_remove_lifecycle(c):
    # configure -> remove -> configure again must work: account.remove
    # exists exactly so the single v1 account slot can be reused.
    resp = c.call("account.configure", {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    })
    assert resp["ok"] is True, resp

    resp = c.call("account.remove")
    assert resp["ok"] is True, resp

    resp = c.call("status.get")
    assert resp["result"]["state"] == "idle", resp
    assert resp["result"]["acc_id"] == -1, resp

    resp = c.call("account.configure", {
        "sip_uri": "sip:2000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "2000",
        "password": "secret",
    })
    assert resp["ok"] is True, resp


def test_account_remove_without_account_fails(c):
    resp = c.call("account.remove")
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_state", resp


# --- Robustness: docs/PROTOCOL.md "Robustness" ------------------------
#
# A line the daemon can't answer with a response (no usable "id") gets
# an `error` event instead, and - except for the oversized-line case -
# the connection survives it.

def _expect_bad_request_event_then_ping(c, raw_line):
    c.sock.sendall(raw_line)
    ev = c.read_event()
    assert ev["event"] == "error", ev
    assert ev["data"]["code"] == "bad_request", ev
    # The connection survived: the next request works normally.
    resp = c.call("ping")
    assert resp["ok"] is True, resp


def test_unparseable_json_survives(c):
    _expect_bad_request_event_then_ping(c, b"this is not json\n")


def test_non_object_json_survives(c):
    _expect_bad_request_event_then_ping(c, b"[1,2,3]\n")


def test_missing_id_survives(c):
    _expect_bad_request_event_then_ping(c, b'{"cmd":"ping"}\n')


def test_non_string_id_survives(c):
    _expect_bad_request_event_then_ping(c, b'{"id":5,"cmd":"ping"}\n')


def test_empty_line_survives(c):
    _expect_bad_request_event_then_ping(c, b"\n")


def test_pipelined_requests_one_write(c):
    # Two complete requests arriving in a single TCP segment must both
    # be answered, in order (the select-driven server drains every
    # complete line after one read, not just the first).
    c.sock.sendall(b'{"id":"a","cmd":"ping"}\n{"id":"b","cmd":"ping"}\n')
    ra = c._recv_line()
    rb = c._recv_line()
    assert ra["id"] == "a" and ra["ok"] is True, ra
    assert rb["id"] == "b" and rb["ok"] is True, rb


def test_oversized_line_closes_connection(c):
    # >8192 bytes with no newline: error event, then the daemon closes
    # (framing can't be trusted mid-line - docs/PROTOCOL.md "Robustness").
    c.sock.sendall(b"x" * 9000)
    ev = c.read_event()
    assert ev["event"] == "error", ev
    assert ev["data"]["code"] == "bad_request", ev
    try:
        c._recv_line()
        raise AssertionError("expected the daemon to close the connection")
    except ConnectionError:
        pass
    # The daemon itself survived: a fresh connection works.
    c2 = Client(SOCK_PATH)
    try:
        assert c2.call("ping")["ok"] is True
    finally:
        c2.close()


def test_second_connection_refused(c):
    # docs/PROTOCOL.md "Transport": second concurrent connection gets a
    # single connection_refused error event (no hello) and is closed.
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(SOCK_PATH)
    try:
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(64)
            if not chunk:
                raise AssertionError(
                    "connection closed before the refusal was sent")
            buf += chunk
        msg = json.loads(buf.decode())
        assert msg.get("event") == "error", msg
        assert msg["data"]["code"] == "connection_refused", msg
        # ... and then closed.
        assert s.recv(64) == b"", "expected EOF after the refusal"
    finally:
        s.close()
    # The active connection was not disturbed.
    resp = c.call("ping")
    assert resp["ok"] is True, resp


def test_state_survives_reconnect(c):
    # docs/PROTOCOL.md "Robustness" ("Client disconnect"): control
    # connection and SIP state are independent - a configured account
    # survives the controlling client going away.
    resp = c.call("account.configure", {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    })
    acc_id = resp["result"]["acc_id"]
    c.close()

    c2 = Client(SOCK_PATH)  # fresh hello expected (consumed by Client)
    try:
        resp = c2.call("status.get")
        assert resp["result"]["acc_id"] == acc_id, resp
        assert resp["result"]["state"] != "idle", resp
    finally:
        c2.close()


def test_slow_reader_gets_dropped(c):
    # docs/PROTOCOL.md "Backpressure": a client that stops reading past
    # the write deadline (shrunk to 300ms via extra_env below) is
    # declared dead and dropped - the daemon must survive and accept a
    # replacement connection. Spam requests without reading responses
    # until both directions' socket buffers fill and the daemon's write
    # deadline expires.
    req = b'{"id":"x","cmd":"ping"}\n'
    dropped = False
    try:
        for _ in range(200000):
            c.sock.sendall(req)
    except (BrokenPipeError, ConnectionResetError, socket.timeout):
        dropped = True
    assert dropped, "daemon never dropped a client that stopped reading"

    # The daemon is still alive and serving.
    c2 = Client(SOCK_PATH)
    try:
        assert c2.call("ping")["ok"] is True
    finally:
        c2.close()


test_slow_reader_gets_dropped.extra_env = {"PJSOCKY_WRITE_TIMEOUT_MSEC": "300"}


def test_ring_timeout_default_is_disabled(c):
    resp = c.call("config.get_ring_timeout")
    assert resp["ok"] is True, resp
    assert resp["result"] == {"seconds": 0}, resp


def test_ring_timeout_set_and_get_round_trip(c):
    resp = c.call("config.set_ring_timeout", {"seconds": 30})
    assert resp["ok"] is True, resp
    assert resp["result"] == {}, resp

    resp = c.call("config.get_ring_timeout")
    assert resp["ok"] is True, resp
    assert resp["result"] == {"seconds": 30}, resp


def test_ring_timeout_can_be_disabled_again(c):
    c.call("config.set_ring_timeout", {"seconds": 15})
    resp = c.call("config.set_ring_timeout", {"seconds": 0})
    assert resp["ok"] is True, resp

    resp = c.call("config.get_ring_timeout")
    assert resp["result"] == {"seconds": 0}, resp


def test_ring_timeout_missing_param(c):
    resp = c.call("config.set_ring_timeout", {})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_ring_timeout_negative_rejected(c):
    resp = c.call("config.set_ring_timeout", {"seconds": -1})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


def test_im_typing_without_registration_fails(c):
    c.call("account.configure", {
        "sip_uri": "sip:1000@127.0.0.1:1",
        "registrar_uri": UNREACHABLE_REGISTRAR,
        "username": "1000",
        "password": "secret",
    })
    # Deliberately not registering - same gate as im.send.
    resp = c.call("im.typing", {"to": "sip:2000@127.0.0.1:1",
                                "is_typing": True})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_state", resp


def test_im_typing_with_no_account_fails(c):
    resp = c.call("im.typing", {"to": "sip:2000@127.0.0.1:1",
                                "is_typing": True})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_state", resp


def test_im_typing_missing_params(c):
    resp = c.call("im.typing", {"to": "sip:2000@127.0.0.1:1"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp

    resp = c.call("im.typing", {"is_typing": True})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp

    # is_typing must be a JSON bool, not a string.
    resp = c.call("im.typing", {"to": "sip:2000@127.0.0.1:1",
                                "is_typing": "yes"})
    assert resp["ok"] is False, resp
    assert resp["error"]["code"] == "invalid_params", resp


TESTS = [
    test_hello_on_connect,
    test_ping,
    test_status_get_idle,
    test_unknown_command,
    test_bad_request_missing_cmd,
    test_multiple_requests_one_connection,
    test_account_configure,
    test_duplicate_configure_rejected,
    test_status_get_after_configure,
    test_account_configure_missing_param,
    test_register_produces_reg_state_event,
    test_register_without_account_fails,
    test_device_list_audio,
    test_device_list_video,
    test_device_set_audio,
    test_device_set_audio_missing_param,
    test_device_set_video,
    test_dial_without_registration_fails,
    test_dial_with_no_account_fails,
    test_dial_missing_uri,
    test_hangup_all_noop_when_idle,
    test_hangup_invalid_call_id,
    test_get_info_invalid_call_id,
    test_answer_invalid_call_id,
    test_answer_missing_call_id,
    test_im_send_without_registration_fails,
    test_im_send_with_no_account_fails,
    test_im_send_missing_params,
    test_account_remove_lifecycle,
    test_account_remove_without_account_fails,
    test_unparseable_json_survives,
    test_non_object_json_survives,
    test_missing_id_survives,
    test_non_string_id_survives,
    test_empty_line_survives,
    test_pipelined_requests_one_write,
    test_oversized_line_closes_connection,
    test_second_connection_refused,
    test_state_survives_reconnect,
    test_slow_reader_gets_dropped,
    test_im_typing_without_registration_fails,
    test_im_typing_with_no_account_fails,
    test_im_typing_missing_params,
    test_ring_timeout_default_is_disabled,
    test_ring_timeout_set_and_get_round_trip,
    test_ring_timeout_can_be_disabled_again,
    test_ring_timeout_missing_param,
    test_ring_timeout_negative_rejected,
]


def main():
    exe_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "..", "build", "pjsocky"
    )
    exe_path = os.path.abspath(exe_path)
    if not os.path.exists(exe_path):
        print(f"pjsocky binary not found at {exe_path}", file=sys.stderr)
        return 2

    failures = []
    for test_fn in TESTS:
        name = test_fn.__name__
        proc = PjsockyProcess(exe_path, SOCK_PATH)
        try:
            # Tests can override daemon env (e.g. a short write deadline
            # for the slow-reader test) via an `extra_env` attribute.
            proc.start(extra_env=getattr(test_fn, "extra_env", None))
            client = Client(SOCK_PATH)
            try:
                test_fn(client)
                print(f"PASS  {name}")
            finally:
                client.close()
        except Exception as e:
            print(f"FAIL  {name}: {e}")
            log = proc.log()
            if log:
                print("--- daemon output ---")
                print(log)
                print("---------------------")
            failures.append(name)
        finally:
            proc.stop()

    print()
    print(f"{len(TESTS) - len(failures)}/{len(TESTS)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
