"""
Minimal client + process harness for pjsocky's control protocol
(../../docs/PROTOCOL.md). Deliberately dependency-free (stdlib only) so
it runs anywhere python3 runs, with no pytest/other package needed -
mirrors the "run with plain python3" convention used by pjproject's own
tests/pjsua harness (see tests/pjsua/run.py in the pjproject checkout).

"""
import json
import os
import socket
import subprocess
import time


class PjsockyProcess:
    """Launches a pjsocky binary against a fresh Unix socket path and
    tears it down with SIGTERM. Shutdown can take close to a second -
    see CONTEXT.md's build-order step 5 notes on pjsua_destroy()'s own
    teardown time - so stop() uses a generous default timeout."""

    def __init__(self, exe_path, sock_path):
        self.exe_path = exe_path
        self.sock_path = sock_path
        self.proc = None

    def start(self, timeout=5, extra_env=None):
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)
        # See main.c: shrinks SIP transaction timers so a registration
        # attempt against an unreachable address fails in ~6s instead of
        # the real-world default of 32s (RFC 3261 Timer F). Test-only.
        env = dict(
            os.environ,
            PJSOCKY_SOCK_PATH=self.sock_path,
            PJSOCKY_TEST_FAST_TIMERS="1",
        )
        if extra_env:
            env.update(extra_env)
        self.proc = subprocess.Popen(
            [self.exe_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
        )
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.exists(self.sock_path):
                return
            if self.proc.poll() is not None:
                raise RuntimeError(
                    "pjsocky exited before creating its control socket:\n"
                    + self.proc.stdout.read().decode(errors="replace")
                )
            time.sleep(0.05)
        raise TimeoutError("pjsocky did not create its control socket in time")

    def stop(self, timeout=5):
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()

    def log(self):
        """Daemon output so far. Blocks until the daemon exits (stdout
        EOF), so call stop() first - reading the log of a still-running
        daemon would hang forever waiting for that EOF (this bit once:
        a test failure with a healthy daemon deadlocked the suite)."""
        if self.proc is None:
            return ""
        self.stop()
        if self.proc.stdout:
            return self.proc.stdout.read().decode(errors="replace")
        return ""


class Client:
    """A single control connection: NDJSON request/response + async
    event reads, per docs/PROTOCOL.md."""

    def __init__(self, sock_path, timeout=5):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(sock_path)
        self._next_id = 1
        # docs/PROTOCOL.md "Versioning": every connection starts with an
        # unsolicited "hello" event, before anything else - consume it
        # here so callers never have to special-case the first read.
        self.hello = self.read_event()
        assert self.hello["event"] == "hello", self.hello

    def _recv_line(self):
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.sock.recv(1)
            if not chunk:
                raise ConnectionError("peer closed while waiting for a line")
            buf += chunk
        return json.loads(buf.decode())

    def call(self, cmd, params=None):
        """Send a request, return its response. Assumes no event is
        pending ahead of it on the wire - use read_event() first if one
        is expected (e.g. right after account.register)."""
        req_id = str(self._next_id)
        self._next_id += 1
        req = {"id": req_id, "cmd": cmd}
        if params is not None:
            req["params"] = params
        self.sock.sendall((json.dumps(req) + "\n").encode())
        resp = self._recv_line()
        assert resp.get("id") == req_id, f"id mismatch: sent {req_id}, got {resp}"
        return resp

    def read_event(self):
        msg = self._recv_line()
        assert "event" in msg and "id" not in msg, f"expected an event, got {msg}"
        return msg

    def close(self):
        self.sock.close()
