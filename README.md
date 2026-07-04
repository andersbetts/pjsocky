# pjsocky

A small, headless SIP audio/video call daemon built on
[pjproject](https://github.com/pjsip/pjproject)'s `pjsua-lib`,
controlled entirely over a local socket with a minimal, versioned,
newline-delimited JSON protocol.

pjsocky does one fixed job: **register a SIP account, place and receive
audio/video calls, and send and receive SIP instant messages** — driven
by whatever program (or human with `socat`) sits on the other end of a
Unix socket. It is what's left of pjproject's reference `pjsua` console
app after removing the interactive console, presence/buddy lists, and
the dozens of runtime knobs: a scriptable calling agent for unattended
systems.

## Quick look

```
$ PJSOCKY_SOCK_PATH=/tmp/pjsocky.sock ./build/pjsocky &
$ socat - UNIX-CONNECT:/tmp/pjsocky.sock
{"event":"hello","data":{"protocol_version":"1.0.0","daemon_version":"0.1.0"}}
{"id":"1","cmd":"account.configure","params":{"sip_uri":"sip:1000@pbx.example.com","registrar_uri":"sip:pbx.example.com","username":"1000","password":"secret"}}
{"id":"1","ok":true,"result":{"acc_id":0}}
{"id":"2","cmd":"account.register"}
{"id":"2","ok":true,"result":{}}
{"event":"reg_state","data":{"acc_id":0,"status":200,"status_text":"OK","registered":true}}
{"id":"3","cmd":"call.dial","params":{"uri":"sip:600@pbx.example.com"}}
{"event":"call_state","data":{"call_id":0,"state":"CALLING","last_status":0,"last_status_text":""}}
{"id":"3","ok":true,"result":{"call_id":0}}
{"event":"call_state","data":{"call_id":0,"state":"CONFIRMED","last_status":200,"last_status_text":"OK"}}
{"event":"call_media_state","data":{"call_id":0,"has_audio":true,"has_video":false}}
{"id":"4","cmd":"call.hangup","params":{"call_id":0,"code":200}}
{"id":"4","ok":true,"result":{}}
```

The full wire protocol — every command, event, and error code, plus the
robustness and backpressure guarantees — is specified in
[docs/PROTOCOL.md](docs/PROTOCOL.md). That document is the source of
truth; protocol changes are treated as public API changes (semver,
recorded in [docs/CHANGELOG.md](docs/CHANGELOG.md)).

## Design in one paragraph

One account, one call, one control connection at a time (v1). The
daemon starts idle — no SIP traffic until a client configures and
registers an account. Events (registration state, incoming calls, call
state, incoming messages) are pushed asynchronously on the same socket.
Control connection and SIP state are independent: if the controller
crashes mid-call, the call stays up and a reconnecting controller
resyncs with `status.get`. Codecs are pinned at build time: G.711
ulaw/alaw for audio, H.264 via OpenH264 (not ffmpeg) for video.

## Building

pjsocky links against a **sibling checkout of pjproject** (not vendored,
not a fork — no modifications to pjproject source):

```
work/
  pjproject/      # github.com/pjsip/pjproject checkout
  pjsocky/        # this repository
```

```sh
git clone https://github.com/pjsip/pjproject.git
git clone <this repo> pjsocky
cd pjsocky
cmake -B build -S .
cmake --build build -j"$(nproc)"
./build/pjsocky
```

If pjproject lives elsewhere, copy `cmake.local.example` to
`cmake.local` and set `PJSOCKY_PJPROJECT_DIR`. See
`.github/workflows/ci.yml` for the exact pjproject commit this is
currently built and tested against, and the Debian/Ubuntu build
dependencies (`cmake`, a C/C++ toolchain, `libasound2-dev`,
`libopenh264-dev`, `libsdl2-dev`).

## Configuration

Environment variables only — no config file, no CLI flags:

| Variable | Default | Meaning |
|---|---|---|
| `PJSOCKY_SOCK_PATH` | `/tmp/pjsocky.sock` | Control socket path |
| `PJSOCKY_LOG_LEVEL` | pjsua defaults (5/4) | Log verbosity 0–6 |
| `PJSOCKY_WRITE_TIMEOUT_MSEC` | `5000` | Drop a control client that stops reading past this deadline |

For running under systemd (dedicated user, socket in `/run/pjsocky/`,
restart policy, hardening), see [packaging/](packaging/) — the unit
file's header comment has the install steps.

## Tests

```sh
python3 tests/protocol/test_basic.py        # protocol suite, no SIP peer needed
python3 tests/asterisk/run_live_tests.py    # real calls against a dockerized Asterisk
```

Both are plain python3 stdlib, no pytest. The live suite provisions a
throwaway Asterisk PBX in docker (rootless docker is the only
requirement) and exercises real registration, an audio call, an H.264
video call, SIP MESSAGE delivery, and an incoming call answered — see
`tests/asterisk/run_live_tests.py`'s docstring.

## License

GPLv2, matching pjproject — see [COPYING](COPYING). pjsocky is a
general-purpose daemon: features shaped around a single proprietary
consumer don't belong here (see [CONTRIBUTING.md](CONTRIBUTING.md)).
