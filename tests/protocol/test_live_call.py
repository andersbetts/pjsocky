#!/usr/bin/env python3
"""
Live call tests: exercise a real SIP registration and a real audio+video
call against an actual registrar/PBX (e.g. Asterisk). Unlike
test_basic.py, these need real infrastructure this sandbox doesn't have
(see CONTEXT.md's build-order steps 8-10 notes: no SIP server is
available here, and getting pjproject's own `pjsua` CLI to act as a
peer didn't pan out either).

These are written now, deliberately, rather than deferred - so the
moment real Asterisk access exists (e.g. against lift-emg-tel's actual
PBX), running this file is what verifies the whole outgoing +
incoming + video call path end to end, instead of starting from
scratch. Until then, they skip cleanly (exit 0, printed as SKIP) rather
than failing, since the gap is "no test peer available", not a
suspected pjsocky defect.

Configure via environment variables and run directly:

    PJSOCKY_TEST_ASTERISK_REGISTRAR=sip:pbx.example.com \\
    PJSOCKY_TEST_ASTERISK_SIP_URI=sip:1000@pbx.example.com \\
    PJSOCKY_TEST_ASTERISK_USERNAME=1000 \\
    PJSOCKY_TEST_ASTERISK_PASSWORD=secret \\
    PJSOCKY_TEST_ASTERISK_PEER_URI=sip:1001@pbx.example.com \\
        python3 tests/protocol/test_live_call.py [path/to/pjsocky]

PJSOCKY_TEST_ASTERISK_PEER_URI should be a second extension on the same
PBX that either answers automatically (e.g. an echo test extension) or
that a human/second pjsocky instance answers manually during the test
run - test_incoming_call_and_answer needs *something* to originate a
call back to this instance.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from client import Client, PjsockyProcess

SOCK_PATH = "/tmp/pjsocky-live-test.sock"

REQUIRED_ENV = [
    "PJSOCKY_TEST_ASTERISK_REGISTRAR",
    "PJSOCKY_TEST_ASTERISK_SIP_URI",
    "PJSOCKY_TEST_ASTERISK_USERNAME",
    "PJSOCKY_TEST_ASTERISK_PASSWORD",
]


def missing_env():
    return [name for name in REQUIRED_ENV if not os.environ.get(name)]


def account_params():
    return {
        "sip_uri": os.environ["PJSOCKY_TEST_ASTERISK_SIP_URI"],
        "registrar_uri": os.environ["PJSOCKY_TEST_ASTERISK_REGISTRAR"],
        "username": os.environ["PJSOCKY_TEST_ASTERISK_USERNAME"],
        "password": os.environ["PJSOCKY_TEST_ASTERISK_PASSWORD"],
    }


def wait_for_event(c, event_name, timeout=10):
    """Reads events until one matching event_name arrives or timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = max(0.1, deadline - time.time())
        c.sock.settimeout(remaining)
        event = c.read_event()
        if event["event"] == event_name:
            return event
    raise TimeoutError(f"no {event_name!r} event within {timeout}s")


def test_register_against_real_pbx(c):
    resp = c.call("account.configure", account_params())
    assert resp["ok"] is True, resp

    resp = c.call("account.register")
    assert resp["ok"] is True, resp

    event = wait_for_event(c, "reg_state")
    assert event["data"]["registered"] is True, (
        f"registration failed against real PBX: {event}"
    )


def test_outgoing_audio_call(c):
    c.call("account.configure", account_params())
    c.call("account.register")
    wait_for_event(c, "reg_state")

    peer_uri = os.environ["PJSOCKY_TEST_ASTERISK_PEER_URI"]
    resp = c.call("call.dial", {"uri": peer_uri, "video": False})
    assert resp["ok"] is True, resp
    call_id = resp["result"]["call_id"]

    event = wait_for_event(c, "call_state", timeout=15)
    while event["data"]["state"] not in ("CONFIRMED", "DISCONNECTED"):
        event = wait_for_event(c, "call_state", timeout=15)
    assert event["data"]["state"] == "CONFIRMED", (
        f"call did not reach CONFIRMED: {event}"
    )

    media_event = wait_for_event(c, "call_media_state", timeout=10)
    assert media_event["data"]["has_audio"] is True, media_event

    resp = c.call("call.hangup", {"call_id": call_id, "code": 200})
    assert resp["ok"] is True, resp
    wait_for_event(c, "call_state", timeout=10)


def test_outgoing_video_call(c):
    """The step-10 test: dial with video, confirm both has_audio and
    has_video come up, and that device.set_video's selected capture
    device doesn't break call setup (full verification that the
    selected device is actually what's transmitted needs inspecting
    RTP/SDP, out of scope for this protocol-level test)."""
    c.call("account.configure", account_params())
    c.call("account.register")
    wait_for_event(c, "reg_state")

    video_devices = c.call("device.list_video")["result"]["devices"]
    capture_devs = [d for d in video_devices if d["dir"] in ("capture", "capture_render")]
    assert capture_devs, "no capture-capable video device available"
    resp = c.call("device.set_video", {"capture_id": capture_devs[0]["id"]})
    assert resp["ok"] is True, resp

    peer_uri = os.environ["PJSOCKY_TEST_ASTERISK_PEER_URI"]
    resp = c.call("call.dial", {"uri": peer_uri, "video": True})
    assert resp["ok"] is True, resp
    call_id = resp["result"]["call_id"]

    event = wait_for_event(c, "call_state", timeout=15)
    while event["data"]["state"] not in ("CONFIRMED", "DISCONNECTED"):
        event = wait_for_event(c, "call_state", timeout=15)
    assert event["data"]["state"] == "CONFIRMED", (
        f"call did not reach CONFIRMED: {event}"
    )

    media_event = wait_for_event(c, "call_media_state", timeout=10)
    assert media_event["data"]["has_audio"] is True, media_event
    assert media_event["data"]["has_video"] is True, (
        f"expected video to come up (peer must support/accept video): {media_event}"
    )

    resp = c.call("call.hangup", {"call_id": call_id, "code": 200})
    assert resp["ok"] is True, resp


def test_im_send_and_message_status(c):
    """Sends an out-of-dialog SIP MESSAGE to PJSOCKY_TEST_ASTERISK_PEER_URI
    and waits for the message_status event that reports whether it was
    delivered. Doesn't require the peer to *reply* (unlike
    test_incoming_call_and_answer, which needs someone to call back) -
    the PBX itself generates the delivery outcome, so this can run
    unattended as long as the peer extension exists."""
    c.call("account.configure", account_params())
    c.call("account.register")
    wait_for_event(c, "reg_state")

    peer_uri = os.environ["PJSOCKY_TEST_ASTERISK_PEER_URI"]
    resp = c.call("im.send", {"to": peer_uri, "content": "pjsocky test message"})
    assert resp["ok"] is True, resp

    event = wait_for_event(c, "message_status", timeout=15)
    assert event["data"]["to"] == peer_uri, event
    assert event["data"]["status"] // 100 == 2, (
        f"expected a 2xx delivery status: {event}"
    )


def test_incoming_call_and_answer(c):
    """Requires something (a human, a second pjsocky, an Asterisk
    autodial) to call PJSOCKY_TEST_ASTERISK_SIP_URI during this test's
    window - there's no way to originate that from this side."""
    c.call("account.configure", account_params())
    c.call("account.register")
    wait_for_event(c, "reg_state")

    print("  waiting up to 30s for an incoming call to "
          f"{os.environ['PJSOCKY_TEST_ASTERISK_SIP_URI']} ...")
    event = wait_for_event(c, "incoming_call", timeout=30)
    call_id = event["data"]["call_id"]

    resp = c.call("call.answer", {"call_id": call_id})
    assert resp["ok"] is True, resp

    event = wait_for_event(c, "call_state", timeout=10)
    while event["data"]["state"] not in ("CONFIRMED", "DISCONNECTED"):
        event = wait_for_event(c, "call_state", timeout=10)
    assert event["data"]["state"] == "CONFIRMED", event

    c.call("call.hangup_all")


TESTS = [
    test_register_against_real_pbx,
    test_outgoing_audio_call,
    test_outgoing_video_call,
    test_im_send_and_message_status,
    test_incoming_call_and_answer,
]


def main():
    missing = missing_env()
    if missing:
        print("SKIP: test_live_call.py requires a real SIP PBX to test against.")
        print(f"Missing environment variables: {', '.join(missing)}")
        print(__doc__)
        return 0

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
        if name == "test_incoming_call_and_answer" and not os.environ.get(
            "PJSOCKY_TEST_ASTERISK_PEER_URI"
        ):
            print(f"SKIP  {name}: needs a caller, but no way to prompt one "
                  "automatically - run interactively if needed")
            continue

        proc = PjsockyProcess(exe_path, SOCK_PATH)
        try:
            proc.start()
            client = Client(SOCK_PATH, timeout=35)
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
