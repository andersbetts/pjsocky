# Changelog

Daemon releases follow semver. The wire protocol is versioned
separately against docs/PROTOCOL.md (reported in the `hello` event);
protocol changes are called out explicitly here because they are the
public API.

## Unreleased (0.1.0) — protocol 1.0.0

Initial release. Everything is new:

- Daemon lifecycle: starts idle (no accounts, no SIP traffic), clean
  shutdown on SIGINT/SIGTERM, config via environment variables
  (`PJSOCKY_SOCK_PATH`, `PJSOCKY_LOG_LEVEL`,
  `PJSOCKY_WRITE_TIMEOUT_MSEC`).
- Control protocol over a Unix domain socket, newline-delimited JSON,
  single control connection (a concurrent second connection is refused
  with a `connection_refused` error event). Spec: docs/PROTOCOL.md.
- Commands: `ping`, `status.get`, `device.list_audio`,
  `device.list_video`, `device.set_audio`, `device.set_video`,
  `account.configure`, `account.register`, `account.unregister`,
  `account.remove`, `call.dial`, `call.answer`, `call.hangup`,
  `call.hangup_all`, `call.get_info`, `config.set_ring_timeout`,
  `config.get_ring_timeout`, `im.send`, `im.typing`.
- Events: `hello`, `error`, `reg_state`, `incoming_call`, `call_state`,
  `call_media_state`, `incoming_message`, `message_status`, `typing`.
- Media: audio G.711 ulaw/alaw; video H.264 via OpenH264 (build-time
  pinned codec set — nothing to negotiate over the protocol).
- Robustness guarantees (docs/PROTOCOL.md "Robustness"/"Backpressure"):
  malformed input never kills the daemon; SIP state survives control
  disconnect (active calls keep running); a control client that stops
  reading is dropped after a bounded write deadline.
- Incoming-call ring timeout is a runtime-configurable parameter
  (`config.set_ring_timeout`/`config.get_ring_timeout`), disabled
  (unbounded ring) by default; auto-rejects with `480 Temporarily
  Unavailable` once configured and elapsed.
- The daemon version carries a build number: the Unix epoch at configure
  time, as a fourth version component (`0.1.0.1786652435`), matching the
  scheme tp4-app uses. It appears in the `hello` event's
  `daemon_version`, in a startup log line, and in the SIP `User-Agent`.
  A release number that moves once a year cannot tell you whether the
  binary on a device is the one you just built; this can.
- systemd packaging (packaging/), protocol test suite
  (tests/protocol/), automated live-call verification against a
  dockerized Asterisk (tests/asterisk/).
