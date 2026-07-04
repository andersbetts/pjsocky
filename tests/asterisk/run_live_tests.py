#!/usr/bin/env python3
"""
Run tests/protocol/test_live_call.py against a throwaway dockerized
Asterisk PBX - the resolution of CONTEXT.md's build-order step 16
("automatic setup of asterisk server to test against").

    python3 tests/asterisk/run_live_tests.py [path/to/pjsocky]

What it does:
 1. builds the Asterisk image (tests/asterisk/Dockerfile - cached after
    the first run) and starts it with --network=host, so SIP and RTP
    stay on 127.0.0.1 with no NAT/port-mapping between the PBX and the
    daemon under test (Asterisk listens on 5080; pjsocky's own UDP
    transport has 5060),
 2. waits until the PBX answers CLI commands,
 3. runs test_live_call.py with the PJSOCKY_TEST_ASTERISK_* environment
    pointing at it,
 4. watches the test output for the "waiting ... for an incoming call"
    prompt (printed by both test_incoming_call_and_answer and
    test_ring_timeout_auto_rejects) and answers it by originating a call
    from the PBX to the registered endpoint (`channel originate
    PJSIP/1000 application Echo`) - the tests that otherwise need a
    human,
 5. tears the container down.

Requires: docker usable without sudo. Plain python3 stdlib, no pytest -
same convention as the rest of tests/ (see tests/protocol/client.py).
"""
import os
import subprocess
import sys
import threading
import time

TESTS_ASTERISK_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(TESTS_ASTERISK_DIR))
LIVE_TEST = os.path.join(REPO_ROOT, "tests", "protocol", "test_live_call.py")

IMAGE = "pjsocky-test-asterisk"
CONTAINER = "pjsocky-test-asterisk"

# Must match tests/asterisk/etc/pjsip.conf.
PBX_HOST_PORT = "127.0.0.1:5080"
TEST_ENV = {
    "PJSOCKY_TEST_ASTERISK_REGISTRAR": f"sip:{PBX_HOST_PORT}",
    "PJSOCKY_TEST_ASTERISK_SIP_URI": f"sip:1000@{PBX_HOST_PORT}",
    "PJSOCKY_TEST_ASTERISK_USERNAME": "1000",
    "PJSOCKY_TEST_ASTERISK_PASSWORD": "pjsocky-test",
    "PJSOCKY_TEST_ASTERISK_PEER_URI": f"sip:600@{PBX_HOST_PORT}",
}

# Printed by test_live_call.py's test_incoming_call_and_answer and
# test_ring_timeout_auto_rejects right after they register and start
# waiting - our cue to originate.
INCOMING_PROMPT = "waiting up to 30s for an incoming call"


def asterisk_cli(cmd, check=False, capture=True):
    return subprocess.run(
        ["docker", "exec", CONTAINER, "asterisk", "-rx", cmd],
        check=check,
        capture_output=capture,
        text=True,
    )


def build_image():
    print(f"[runner] building {IMAGE} image (cached after first run) ...")
    subprocess.run(
        ["docker", "build", "-q", "-t", IMAGE, TESTS_ASTERISK_DIR],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def start_container():
    subprocess.run(["docker", "rm", "-f", CONTAINER],
                   capture_output=True)
    subprocess.run(
        [
            "docker", "run", "--rm", "-d",
            "--name", CONTAINER,
            "--network=host",
            "-v", os.path.join(TESTS_ASTERISK_DIR, "etc") + ":/etc/asterisk:ro",
            IMAGE,
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def wait_for_pbx(timeout=30):
    print("[runner] waiting for the PBX to come up ...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = asterisk_cli("pjsip show endpoint 1000")
        if r.returncode == 0 and "1000" in r.stdout:
            print("[runner] PBX is up")
            return
        time.sleep(0.5)
    subprocess.run(["docker", "logs", CONTAINER])
    raise TimeoutError("Asterisk container did not become ready in time")


def originate_incoming_call():
    """Fires the PBX->pjsocky call that test_incoming_call_and_answer
    waits for. Retries a few times: registration from the *fresh daemon
    that test starts* may land a moment after the prompt is printed."""
    for attempt in range(10):
        r = asterisk_cli("channel originate PJSIP/1000 application Echo")
        out = (r.stdout + r.stderr).strip()
        if r.returncode == 0 and "failed" not in out.lower():
            print("[runner] originated incoming call to endpoint 1000")
            return
        time.sleep(2)
    print("[runner] WARNING: could not originate the incoming test call")


def run_tests(exe_args):
    env = dict(os.environ, **TEST_ENV)
    # -u: unbuffered - the incoming-call prompt must reach us the moment
    # it's printed (a block-buffered pipe would deliver it only after
    # the test already timed out waiting for our originate).
    proc = subprocess.Popen(
        [sys.executable, "-u", LIVE_TEST] + exe_args,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    # Stream output, watching for the incoming-call prompt; originate
    # from a thread so we keep draining output (a full pipe would
    # deadlock the test).
    for line in proc.stdout:
        print(line, end="", flush=True)
        if INCOMING_PROMPT in line:
            threading.Thread(target=originate_incoming_call,
                             daemon=True).start()
    return proc.wait()


def main():
    build_image()
    start_container()
    try:
        wait_for_pbx()
        return run_tests(sys.argv[1:])
    finally:
        subprocess.run(["docker", "rm", "-f", CONTAINER],
                       capture_output=True)


if __name__ == "__main__":
    sys.exit(main())
