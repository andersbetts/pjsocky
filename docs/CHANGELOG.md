# Changelog

Daemon releases follow semver. The wire protocol is versioned
separately against docs/PROTOCOL.md (reported in the `hello` event);
protocol changes are called out explicitly here because they are the
public API.

## Unreleased (0.2.0) — protocol 1.1.0

### Media

- **Video the far end turns on mid-call is now accepted.** pjsua answers an
  incoming offer from `call->opt`, whose `vid_cnt` is fixed when the call is set
  up — so a peer that answers with its camera off left the call at `vid_cnt = 0`,
  and every later re-INVITE offering video was answered inactive: the m-line came
  back as `video none dir=inactive` and nothing was ever sent or decoded. That is
  the normal way a video call starts on this hardware, since the operator answers
  first and enables the camera a moment later, so video could not work in the
  usual flow at all. `on_call_rx_offer` now raises `vid_cnt` when the offer
  carries a video m-line with a non-zero port (capped at one stream: one camera,
  and less than one Cortex-A7 to spare). Verified on the device — `video active
  dir=sendrecv codec=H264`, RTP flowing both ways.
- **Per-call media counters, every 5s**: `call N media: audio tx=… rx=… video
  tx=… rx=…`, with `-` for a media type the call does not have. "No media" was
  previously answerable only by attaching to the control socket, which the
  launching application already holds — i.e. not while the product is running.
  This turns it into a specific question with the answer already in the log:
  nothing sent, or nothing received, and for which stream. Those have completely
  different causes.
- **The negotiation result is logged on every offer/answer**, including a
  mid-call re-INVITE: status, direction, codec, and `tx_pt`/`rx_pt` per m-line,
  plus how many streams the remote actually offered. Both payload types are
  printed because dynamic PTs (96–127) are negotiated per direction (RFC 3264
  §6.1) — `tx_pt != rx_pt` is legal and normal, and having it visible stops it
  being re-investigated as a mismatch.

### Diagnostics

- **stdout is line-buffered** (`setvbuf`). On a device stdout is a pipe to the
  launching application, so glibc chose full buffering and nothing reached the
  reader until 4KB had accumulated. An observed run had log lines arriving in the
  system journal **one hour and forty-four minutes** after the events they
  described, in a burst, with their own timestamps intact and the journal's
  wrong. That is worse than losing them: a log whose ordering against the rest of
  the system is silently false invites conclusions drawn from a sequence that
  never happened.

### NAT

- **`PJSOCKY_STUN_SRV` and `PJSOCKY_ICE`**, both unset by default — the
  mechanism only. STUN does correct the SDP `c=` line on a NAT'd device, but on
  the hardware this was tested against it broke registration: that NAT hands out
  a different external port per destination, so the mapping STUN discovered was
  not the one in use towards the SIP server, and REGISTER began failing with 408
  where it had always succeeded. `rport`/`received` gets SIP right precisely
  because it never has to predict the mapping. Kept because it is the correct fix
  on a network whose NAT keeps one mapping; off because it is the wrong one here.

Protocol change, additive: a 1.0.0 client works against this daemon
unchanged, and a client written against 1.1.0 degrades to `hello` alone
against a 1.0.0 daemon.

- New command `version.get`: params none, result
  `{"protocol_version", "daemon_version", "pjsip_version"}` — the same
  object the `hello` event carries, on demand. `hello` only ever reaches
  a client that was connected at the moment the daemon came up; a
  controller that reconnected, or that wants to log what it is talking
  to at a point of its own choosing, had no way to ask. Reads no SIP
  state and cannot fail, so it also answers on a daemon too broken to
  answer anything else.
- New field `pjsip_version` in `version.get` and in the `hello` event:
  the pjproject the daemon was built against (`pj_get_version()`). It is
  reachable nowhere else — a controller links no pjsip of its own and
  the daemon takes no command-line flags — so a SIP-stack fault in the
  field could be traced to a pjsocky release but not to the library
  underneath it, which is usually where such faults live.
- The startup log line names the protocol version and pjsip version
  beside the daemon version, so the same three numbers are on the
  console whether or not anything ever connects.
- `hello` and `version.get` are built from one place in the daemon
  (`src/proto/version.c`), so being told on connect and asking later
  cannot come to disagree.

## 0.1.0 — protocol 1.0.0

Initial release. Everything is new:

- Daemon lifecycle: starts idle (no accounts, no SIP traffic), clean
  shutdown on SIGINT/SIGTERM, config via environment variables
  (`PJSOCKY_SOCK_PATH`, `PJSOCKY_LOG_LEVEL`,
  `PJSOCKY_WRITE_TIMEOUT_MSEC`, `PJSOCKY_VAD`, `PJSOCKY_VIDEO_SIZE`,
  `PJSOCKY_VIDEO_FPS`).
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
- `call.get_info` reports RTP packet counters per media type
  (`audio_tx_packets`/`audio_rx_packets`, `video_tx_packets`/
  `video_rx_packets`), present only for a media type with an active
  stream. `has_video` says a stream was negotiated; these say whether
  anything is travelling through it, which is the difference between a
  capture/encode fault on this side and a problem at the far end.
- `device.set_video` rejects a `capture_id` that is not a capture
  device, and the account's own fallback pick (used when no
  `device.set_video` arrived) skips render-only devices rather than
  taking the first device enumerated. The null render device likewise
  refuses a capture-direction stream instead of creating one it can
  never produce a frame from. All three are the same failure: pjsua
  opens whatever device id it is handed in whichever direction it was
  asked for, so a render-only pick made a call negotiate video, report
  media active, and send no picture at all — with nothing logged to say
  why. Reachable whenever the camera is missing from the enumeration,
  which the v4l2 factory performs once at startup.
- Silence detection (VAD) is off by default, inverting pjsua-lib's own
  default and matching pjsua's demo app (`--no-vad`). With it on, a
  microphone sitting near the detector's threshold — an ordinary quiet
  electret in a quiet room — makes the daemon stop sending RTP mid-call
  and resume later, which on a headless box is indistinguishable from a
  broken media path and depends on the room rather than on anything the
  operator controls. It also makes `call.get_info`'s packet counters
  usable as a health signal. `PJSOCKY_VAD=1` restores it.
- The video encoder's capture size and frame rate can be set
  (`PJSOCKY_VIDEO_SIZE=640x480`, `PJSOCKY_VIDEO_FPS=15`), applied to
  every registered video codec at startup. pjmedia's H.264 default asks
  for 720x480; a camera that cannot produce that is opened at whatever
  it does support and every frame is then converted and rescaled before
  encoding, which on a small ARM target costs most of the negotiated
  frame rate. A malformed value is logged and ignored, never fatal.
- A video fault no longer costs the call, or floods the log. A capture
  device that fails or stops delivering frames used to be reported by
  pjmedia per frame — an unbounded repeat of the same line, at frame
  rate, while the call stayed up and audio kept working. The daemon now
  gives up on video once per call: one warning, the video stream is
  removed (re-INVITE without the `m=video` line, so the far end stops
  waiting for a picture), and the call continues as an audio call, with
  a `call_media_state` event carrying `has_video` false. Both the
  event-reporting failures (`PJMEDIA_EVENT_VID_DEV_ERROR`) and the
  silent kind (a camera that simply stops, which pjsua-lib never times
  out) are covered — the latter by watching the video RTP transmit
  counter. See docs/PROTOCOL.md's `call_media_state`.
- Startup says so when there is no camera: the fallback capture pick
  warns when it lands on pjmedia's colorbar generator (a synthetic test
  source — video calls would send a test pattern rather than a picture)
  and when there is no capture device at all.
- systemd packaging (packaging/), protocol test suite
  (tests/protocol/), automated live-call verification against a
  dockerized Asterisk (tests/asterisk/).
