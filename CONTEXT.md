# pjsocky

A small, standalone SIP audio/video call daemon built on top of
[pjproject](https://github.com/pjsip/pjproject). It links directly against
`pjsua-lib` and exposes a minimal, generic, versioned JSON control protocol
over a local socket. It is a sibling project to pjproject, not a fork of it.

## Why this project exists

`pjsua` (the reference console app shipped with pjproject) is a good
demonstration of the pjsua-lib API but it's a kitchen-sink CLI aimed at
developers: presence, buddy lists, IM, a text console, dozens of runtime
knobs. pjsocky trims that down to a small, fixed job: **register/unregister a
SIP account, place/receive audio+video calls, and send/receive SIP instant
messages**, controlled entirely over a socket instead of a terminal.

pjsocky is a **general-purpose** daemon, not something written to be
inseparable from any one proprietary application. It should be genuinely
usable by anyone who wants a scriptable, headless SIP calling agent — that
independence is a deliberate design goal, not an afterthought. See
`docs/PROTOCOL.md` for the stable, versioned wire protocol; treat protocol
changes as public API changes (semver-style, documented in a changelog).

## License

GPLv2, matching pjproject. `COPYING` is a verbatim copy of pjproject's
GPLv2 text. This repository is public. Do not add code here that only makes
sense in the context of a specific downstream consumer — if a feature is
proprietary-shaped, it doesn't belong in pjsocky.

## Relationship to pjproject

- Sibling checkout: `~/work/pjproject` and `~/work/pjsocky` side by side.
- pjsocky's CMake build adds pjproject as a subdirectory
  (`PJSOCKY_PJPROJECT_DIR`, defaults to `../pjproject`) and links against
  the `pjsua-lib` CMake target (which transitively pulls in pjsip, pjmedia,
  pjnath, pjlib-util, pjlib).
- No modifications to pjproject source. If we need something pjproject
  doesn't expose, that's a signal to either extend pjsocky's own code or
  propose the change upstream — not to patch our local checkout silently.

## Architecture

```
pjsocky/
  CMakeLists.txt
  COPYING                  # GPLv2 (verbatim from pjproject)
  README.md
  CONTRIBUTING.md
  CONTEXT.md                # this file
  .github/workflows/ci.yml  # build against pinned pjproject + both test suites
  docs/
    PROTOCOL.md             # versioned wire protocol spec
    CHANGELOG.md
  src/
    main.c                  # entry point: pjsua init/start/destroy, idle lifecycle,
                              # signal handling. No separate app.c/app.h in the end -
                              # main.c's own scope stayed small enough not to need one;
                              # revisit only if it actually grows unwieldy.
    account.c / account.h    # pjsua_acc_add/set_registration wrapper + on_reg_state2
    call.c / call.h          # on_call_state/on_call_media_state/on_incoming_call,
                              # pjsua_call_make_call/hangup/get_info/answer2 wrapper
    im.c / im.h               # on_pager2/on_pager_status2, pjsua_im_send wrapper
    device.c / device.h      # audio+video device enumeration & selection
    proto/
      server.c / server.h    # socket accept loop, one control connection at a time
      framing.c / framing.h  # newline-delimited JSON framing over the socket
      jsonutil.c / jsonutil.h # shared pj_json_elem build/read helpers (mind the
                              # lifetime warning at the top of jsonutil.h)
      dispatch.c / dispatch.h # command name -> handler table
      events.c / events.h    # single-writer coordinator: routes both response writes
                              # (server.c) and pushed events (account.c et al, called
                              # from pjsua's worker thread) through one lock, so the
                              # two can never interleave bytes on the connection socket
    log.c / log.h             # bridge PJ_LOG into an event, or syslog, not both by default
                              # (not yet written)
  tests/
    protocol/                # client.py (harness) + test_basic.py (asserts, no real
                              # SIP peer needed), plain python3 + stdlib only, no
                              # pytest - see client.py's docstring. Grows alongside
                              # each new command/event. test_live_call.py: real
                              # register/dial/answer/video/hangup tests against an
                              # actual PBX, gated on PJSOCKY_TEST_ASTERISK_* env vars,
                              # skips cleanly (exit 0) when unset - see step 10.
    asterisk/                # dockerized throwaway Asterisk PBX + run_live_tests.py,
                              # which runs test_live_call.py against it fully
                              # automatically (see step 16) - 5/5 passing locally
  packaging/
    pjsocky.service           # systemd unit for target deployment
    pjsocky.conf.sample
```

## Design decisions (already made)

- **Transport**: Unix domain socket by default (path configurable via CLI
  flag / config file). TCP-on-loopback optionally enabled for
  development/debugging, off by default in production builds.
- **Framing**: newline-delimited JSON (one JSON object per line, UTF-8).
  Use pjlib-util's existing `pj_json_parse`/`pj_json_write`
  (`pjlib-util/include/pjlib-util/json.h`) instead of pulling in a new JSON
  dependency — keeps it in the pjproject family, pool-allocated, no
  malloc/free in the core path.
- **Control model v1**: single active control connection. A second
  connection attempt is refused (not queued, not silently bumping the
  first) until the daemon supports multi-client properly. This is a
  deliberate v1 simplification, revisit once concurrent controllers are
  actually needed.
- **Idle by default**: the daemon starts with `pjsua_init`/`pjsua_start`
  and zero accounts. No SIP traffic happens until a client sends
  `account.configure` followed by `account.register`.
- **One account at a time in v1.** Multi-account is a later extension, not
  a v1 requirement — keep the protocol namespaced (`acc_id` present in
  messages) so it's not a breaking change to add later.
- **No GUI/video rendering window.** Video capture/render are headless;
  frames go through whatever pjmedia video device/port is wired to the
  target hardware. Exact hardware video path is a TODO/open question (see
  below).
- **Thread-safety**: pjsua-lib API calls are documented thread-safe, so the
  socket server thread can call into pjsua directly from command handlers.
  pjsua callbacks (on_call_state etc.) may fire from pjsua's own worker
  thread — event serialization/push to the socket must not block on
  application-level locks pjsua itself might be holding (matches
  pjproject's own group-lock guidance: never invoke a callback while
  holding a lock that could deadlock the caller).
- **Codecs are pinned explicitly, not inherited from pjproject's CMake
  defaults.** Audio: G.711 ulaw/alaw only (`PJMEDIA_WITH_G711_CODEC`,
  core pjmedia, no external dependency). Video: H.264 only, via OpenH264
  (`PJMEDIA_WITH_OPEN_H264_CODEC`) — **not** ffmpeg. All other optional
  codecs (GSM, Speex, iLBC, G.722/G.722.1, AMR-NB/WB, SILK, Opus, BCG729,
  Lyra, VPX) are explicitly turned off in `CMakeLists.txt`. This keeps the
  build lightweight and dependency-light, and matches the actual
  requirement rather than pjproject's kitchen-sink default. See the
  ffmpeg note below — pjproject's `FindFFMPEG.cmake` is also broken in
  this checkout, which we avoid entirely by not needing it.
- **ffmpeg is disabled (`PJMEDIA_WITH_FFMPEG=OFF`,
  `PJMEDIA_WITH_VIDEODEV_FFMPEG=OFF`).** Not just a workaround: pjproject's
  `cmake/FindFFMPEG.cmake` uses an `if()` operator (`IS_READABLE`) that
  doesn't exist in CMake and fails configure outright, and our H.264 need
  is fully covered by OpenH264 instead. If a future codec requirement
  genuinely needs ffmpeg, that bug has to be dealt with upstream or
  worked around properly at that point — don't quietly re-enable it.

## Deliberately removed vs. pjsua (the "fluff")

- Presence / BUDDY list (instant messaging is in scope, presence/buddy
  list is not — see Command surface below)
- Interactive text console / telnet CLI (pjsocky's whole point is to
  replace this with a real protocol)
- Conference bridge manipulation beyond the single active call
- Call recording/playback file management (revisit only if a real need
  shows up)
- Manual STUN/TURN/ICE protocol-level knobs (still compiled in and active
  via pjproject defaults for NAT traversal, just not separately exposed as
  control-protocol commands in v1)

## Command surface (v1 draft — see docs/PROTOCOL.md for the real spec)

Requests (`{"id":..,"cmd":..,"params":{...}}` -> `{"id":..,"ok":..,"result"|"error":..}`):

- `ping`
- `status.get` — idle / registered / registering / in-call, current acc_id/call_id if any
- `device.list_audio` — wraps `pjsua_enum_aud_devs`
- `device.list_video` — wraps `pjsua_vid_enum_devs`
- `device.set_audio {capture_id, playback_id}` — wraps `pjsua_set_snd_dev`
- `device.set_video {capture_id}` — used at call setup time via vid param
- `account.configure {sip_uri, registrar_uri, username, password, realm}` — wraps `pjsua_acc_add`, does NOT register
- `account.register` — wraps `pjsua_acc_set_registration(acc_id, PJ_TRUE)`
- `account.unregister` — wraps `pjsua_acc_set_registration(acc_id, PJ_FALSE)`
- `call.dial {uri, video}` — wraps `pjsua_call_make_call`
- `call.answer {call_id, code, video}` — wraps `pjsua_call_answer2`
- `call.hangup {call_id, code}` — wraps `pjsua_call_hangup`
- `call.hangup_all`
- `call.get_info {call_id}` — wraps `pjsua_call_get_info`
- `im.send {to, content, mime_type}` — wraps `pjsua_im_send`. Out-of-dialog
  SIP MESSAGE, not tied to a call. Async, like `account.register` — the
  response only confirms the request was accepted; delivery outcome
  arrives via `message_status`.
- `im.typing {to, is_typing}` — wraps `pjsua_im_typing`. Out-of-dialog
  composing indication (RFC 3994), same "no dialog needed" model as
  `im.send`. Fire-and-forget — no delivery-outcome event exists for this
  (pjsua's own typing-send path doesn't report one).

Events (`{"event":..,"data":{...}}`, no `id`, pushed async):

- `reg_state {acc_id, code, reason}` — from `on_reg_state2`
- `incoming_call {call_id, acc_id, from}` — from `on_incoming_call`
- `call_state {call_id, state, last_status}` — from `on_call_state`
- `call_media_state {call_id, has_audio, has_video}` — from `on_call_media_state`
- `incoming_message {from, to, mime_type, body}` — from `on_pager2`
- `message_status {to, body, status, reason}` — from `on_pager_status2`
- `typing {from, to, is_typing}` — from `on_typing2`

## Open questions / TODO before coding

- [ ] Confirm exact target audio device backend (ALSA? custom pjmedia
      audiodev driver?) and how it enumerates through `pjsua_enum_aud_devs`.
- [ ] Confirm target video hardware path — is there already a pjmedia
      video device driver, or does pjsocky need one written?
- [x] Decide daemon supervision: systemd unit vs. whatever init the
      target firmware uses. - **Decided: systemd.** Implemented in step 14
      (packaging): `packaging/pjsocky.service` + `packaging/pjsocky.conf.sample`,
      plus a CMake `install(TARGETS ...)` rule.
- [x] Decide config file format for the socket path, log level, etc.
      (separate from the runtime protocol). - **Decided: env vars, no
      config file.** Implemented: `PJSOCKY_SOCK_PATH` (since step 6),
      `PJSOCKY_LOG_LEVEL` (0-6, overrides both `pjsua_logging_config.level`
      and `.console_level`; unset uses pjsua's own defaults 5/4) and
      `PJSOCKY_WRITE_TIMEOUT_MSEC` (control-socket write deadline,
      default 5000 - since step 12) - see `main.c`.
- [x] Decide protocol version negotiation: a `hello`/handshake message
      exchanged on connect, or just documented compatibility per release
      tag? - **Decided: keep `hello`.** Implemented in `server.c`
      (`build_hello_data`, sent via `pjsocky_events_push()` right after
      `pjsocky_events_set_conn()`, before the request loop starts) -
      `docs/PROTOCOL.md` already specified the wire format, it just
      wasn't wired up in code until now. `protocol_version` was
      `"1.0.0-draft"` until step 17, then `"1.0.0"`, now `"1.1.0"` (see
      `PJSOCKY_PROTOCOL_VERSION`, which moved to `proto/version.h` when
      `version.get` gave it a second reader) - see step 17's notes on
      the tag. The `hello` payload itself is built by
      `pjsocky_version_fill()`, shared with that command so the two
      cannot disagree. `daemon_version` comes from `PJSOCKY_VERSION`, a compile definition
      derived from `CMakeLists.txt`'s `project(... VERSION ...)` rather
      than a second hardcoded string.
- [x] Typing indicators (`on_typing2`/`SIP MESSAGE` composing state) are
      not in v1's `im.send`/`incoming_message` pair — decide if this
      actually needs them before adding. - **Decided: add them.**
      Implemented as `im.typing` (wraps `pjsua_im_typing`, out-of-dialog,
      same model as `im.send`) and a `typing` event (from `on_typing2`).
      Out-of-dialog only for v1 - in-dialog typing indications during an
      active call (`pjsua_call_send_typing_ind` and its receive-side
      counterpart) are a possible future addition, not implemented now -
      see `docs/PROTOCOL.md`'s `typing` event section and Non-goals.
- [x] OpenH264: confirm the target build environment can provide the
      OpenH264 library (Cisco's prebuilt binary carries their H.264 patent
      royalty coverage; a self-compiled-from-source build may not carry
      the same coverage) — worth a quick check, not just an assumption. -
      confirmed, OpenH264 will be provided. Getting it to actually build
      in required a CMake workaround, not just installing the library -
      see step 10's notes on the `config_auto.h.cm` bug.
- [x] Event push thread-safety: resolved in step 6 via `proto/events.c` -
      a mutex-guarded singleton that both `server.c`'s response writes
      and pjsua-worker-thread-triggered event pushes (`account.c`'s
      `on_reg_state2`) go through, so the two can never interleave bytes
      on the connection socket. See step 6's notes below.

## TODO — build order

1. [x] Repo scaffolding: CMakeLists.txt, COPYING (GPLv2 text), README.md,
       directory layout above, `.gitignore`.
2. [x] CMake: sibling-link to `../pjproject`, build `pjsocky` binary
       linking `pjsua-lib`. Verify a "hello world that calls
       `pjsua_init`/`pjsua_start`/`pjsua_destroy` and exits" builds and
       runs clean with zero warnings (`-Wall`).
3. [x] `docs/PROTOCOL.md`: write the full versioned spec (message
       envelope, error format, all v1 commands/events) before writing the
       dispatcher, so the protocol is designed once, not organically grown
       inline in C.
4. [x] Socket server skeleton: accept one Unix-domain connection, NDJSON
       framing in/out, `ping` command round-trip. No pjsua logic yet.
       Implemented as `src/proto/framing.c` (NDJSON read/write, including
       compacting `pj_json_write()`'s pretty-printed output into a single
       line - it doesn't have a compact mode), `src/proto/dispatch.c`
       (request parsing, envelope building, command table with `ping`
       registered), `src/proto/server.c` (Unix-domain accept loop, one
       connection served to completion at a time - see the "second
       connection" scope limit noted in `server.h`). Verified against a
       raw-socket test client: `ping` round-trip, `unknown_command` and
       `bad_request` error paths, multiple requests on one connection,
       and a fresh connection after the first closes.
5. [x] Daemon lifecycle: idle start (`pjsua_init`/`start`, zero accounts,
       already in place since step 2), clean shutdown on signal
       (`pjsua_destroy`), `status.get`. `status.get` is implemented in
       `dispatch.c` but currently returns hardcoded idle/no-account
       constants - it needs to report real state once account.c/call.c
       exist (step 6+).

       Signal handling (`main.c`): SIGINT/SIGTERM set a flag via
       `pjsocky_server_stop()`. First attempt had the handler close the
       listening socket to unblock a plain blocking `accept()` - flaky
       on Linux, because `pjsua_start()` spawns its own worker thread,
       and a process-directed signal (plain `kill`) isn't guaranteed to
       land on the thread actually blocked in `accept()`; closing an fd
       from a different thread than the one blocked on it in `accept()`
       doesn't reliably wake it on Linux (unlike BSD). Replaced with a
       `select()`-with-timeout poll loop (`PJSOCKY_ACCEPT_POLL_MSEC` =
       500ms in `server.c`) so shutdown no longer depends on which
       thread receives the signal. Verified 10/10 clean shutdowns under
       both SIGINT and SIGTERM. Total shutdown latency after the signal
       is up to ~500ms (poll interval) plus `pjsua_destroy()`'s own
       teardown time (~1s observed: hangup/presence/media/transport
       layer shutdown) - expect ~1-1.5s end to end, not instant.
6. [x] Account commands: `account.configure`, `account.register`,
       `account.unregister`, `reg_state` event via `on_reg_state2`.
       Implemented as `src/account.c` (pjsua account wrapper, no JSON
       dependency) plus new `src/proto/events.c` and
       `src/proto/jsonutil.c`. Also required two prerequisites that
       turned out to be missing, not part of the original step 6 scope:
       `pjsua_transport_create(PJSIP_TRANSPORT_UDP, ...)` in `main.c`
       (`pjsua_acc_add()` asserts without at least one SIP transport -
       nothing before now had ever added one), and the event-push
       thread-safety design flagged as an open question after step 5.

       `proto/events.c` is the resolution to that open question: a
       singleton (`pjsocky_events_instance()`, since pjsua's callback
       signatures have no user_data slot to thread a pointer through)
       holding a mutex and the current connection's `pjsocky_framing_t*`.
       Both `server.c`'s response writes and `account.c`'s
       `on_reg_state2`-triggered event pushes (which fire on pjsua's own
       worker thread) now go through
       `pjsocky_events_write_response()`/`pjsocky_events_push()` instead
       of calling `framing.c` directly, so the two can never interleave
       bytes on the same socket. `pjsocky_events_set_conn()` (called on
       accept and again with NULL right before the connection socket is
       closed) blocks until any in-flight write finishes first.

       Two real bugs surfaced and were fixed, both worth remembering:
       - `pjsua_acc_info.has_registration` means "has a `reg_uri`
         configured" (static, true immediately after `account.configure`),
         **not** "is currently registered" - despite the name. Both
         `docs/PROTOCOL.md`'s original description of the `reg_state`
         event and the first cut of `dispatch.c`/`account.c` conflated
         the two, which made `status.get`/`reg_state` report
         `"registered"` before any registration attempt ever happened.
         Fixed by having `account.c` track real state itself
         (`pjsocky_account_is_registered()`), set from `on_reg_state2`
         using `pjsua_reg_info.renew` (register vs. unregister direction)
         together with the resulting SIP status - the status code alone
         is ambiguous since a successful un-REGISTER is also a 2xx.
       - A dangling-pointer-to-stack bug in `dispatch.c`: an error
         message built via `pj_strerror()` into a local `char[]` and
         passed through `pjsocky_json_add_string()` produced garbled
         bytes on the wire, because that helper only copies the
         `pj_str_t` (pointer+length), not the characters - the stack
         frame was gone by the time `pj_json_write()` actually read it
         in `server.c`. Fixed by allocating from the request's pool
         instead. `jsonutil.h` now carries an explicit lifetime warning
         about this so it isn't repeated for `call.c`/`im.c`.

       `tests/protocol/` (see below) is what caught both of these -
       manual spot-checks had missed them.
7. [x] Device commands: `device.list_audio`, `device.list_video`,
       `device.set_audio`. Also implemented `device.set_video`, which
       `docs/PROTOCOL.md` already specified but this checklist item's
       wording had dropped. `src/device.c` wraps
       `pjsua_enum_aud_devs`/`pjsua_vid_enum_devs`/`pjsua_set_snd_dev`
       with no JSON dependency, same pattern as `account.c`;
       `device.set_video` just records the selected capture device id
       for `call.c` (step 8+) to use later - it doesn't open anything
       itself yet. Applied the `jsonutil.h` lifetime lesson from step 6
       up front this time: the `pjmedia_aud_dev_info`/`pjmedia_vid_dev_info`
       arrays populated by `pjsua_enum_*_devs()` are pool-allocated in
       `dispatch.c`'s command handlers, not stack-allocated, since their
       `name`/`driver` fields get referenced (not copied) by the response
       JSON tree. Verified against real device data
       (`tests/protocol/test_basic.py`, 16/16 passing): ALSA audio
       devices (`lavrate`, `samplerate`, `jack`, ...) and video devices
       correctly split into `capture` (Colorbar generators) vs. `render`
       (SDL) by the `dir` mapping.
8. [x] Outgoing audio call: `call.dial`, `call_state`/`call_media_state`
       events, `call.hangup`. Also implemented `call.hangup_all` and
       `call.get_info` (both in `docs/PROTOCOL.md` already, just not
       called out in this checklist wording) since they're needed to
       make outgoing calls actually usable/testable, not deferred pieces.
       `src/call.c` follows the same no-JSON-dependency pattern as
       `account.c`/`device.c`; `status.get` now reports real `call_id`
       and an `"in_call"` state once a call exists.

       Found and fixed a real crash bug: `pjsua_call_hangup()` and
       `pjsua_call_get_info()` range-check `call_id` internally via
       `PJ_ASSERT_RETURN()`, which in this build configuration actually
       **aborts the process** on failure rather than returning an error
       (assertions are compiled in) - confirmed by sending an
       out-of-range `call_id` through `call.hangup`/`call.get_info` from
       `tests/protocol` and watching the daemon crash. An in-range
       `call_id` that just isn't an active call is handled cleanly by
       pjsua (a normal error, not a crash) - it's specifically the
       bounds check that's unsafe to delegate to pjsua. Fixed with an
       explicit `is_valid_call_id()` check in `call.c` before ever
       calling into pjsua, for both functions. This is a pattern worth
       remembering for `call.answer` (step 9) and anything else that
       takes a caller-supplied ID: don't assume pjsua's own validation
       fails safely just because it returns a `pj_status_t` - some of it
       asserts instead.

       No SIP server is available in this sandbox to test a real
       end-to-end loopback call (checked: only `sofia-sip`, a library,
       is installed - no server/registrar). `tests/protocol` covers what
       is reachable without one: the registration gate on `call.dial`,
       and out-of-range/invalid `call_id` handling (both the crash fix
       above and the normal-but-inactive case). A genuine loopback call
       stays a documented gap - see step 13's notes.
9. [x] Incoming call: `incoming_call` event, `call.answer`. Resolved
       `docs/PROTOCOL.md`'s open question along the way: `call.answer`'s
       `video` default (when not specified) mirrors whether the incoming
       offer had video (`pjsua_call_info.rem_vid_cnt > 0`), not a fixed
       audio-only default - matches how most softphones behave ("accept
       as offered"). Also changed `incoming_call`'s `"from"` field from
       the originally-documented bare-URI example to the same format as
       `call.get_info`'s `"remote_info"` (display name + URI) - simpler
       and consistent, rather than writing separate URI-extraction code
       for one field. The `is_valid_call_id()` crash-guard from step 8
       covers `call.answer` too (verified via `tests/protocol` - an
       out-of-range `call_id` returns `invalid_params` cleanly, no crash).

       Spent real effort trying to get a genuine end-to-end incoming
       call test working (`pjsua`, the reference CLI app built alongside
       pjproject, dialing pjsocky directly with no registrar needed) and
       it didn't pan out - worth recording what was learned rather than
       just the outcome:
       - `pjsua`'s interactive console (even with `--no-cli-console`,
         even with stdin explicitly kept open) fights running headlessly
         from a script - not fully resolved.
       - Real, reproducible finding along the way: `pjsua --local-port=N
         <uri>` without an explicit `;transport=udp` on the destination
         URI attempted a **TCP** connection to pjsocky's UDP-only
         transport and failed with connection-refused - genuinely worth
         knowing for whoever revisits this, not a pjsocky bug.
       - Even with the transport fixed, pjsocky's own log showed zero
         RX activity - the INVITE never actually arrived - which is
         `pjsua`-side request delivery/resolution behavior this
         investigation didn't get to the bottom of, not something in
         `pjsocky`'s own code path (no incoming SIP traffic ever reached
         pjsocky's transport to exercise).
       Given no SIP server is available in this sandbox either (see step
       8's notes) and this was costing time without surfacing any actual
       pjsocky defect, stopped here rather than continuing to fight the
       test tooling. `tests/protocol` covers what's reliably testable
       without a working peer: the `call.answer` crash-guard and
       missing-param validation. A real incoming-call-answered-confirmed
       end-to-end test remains a genuine gap - see step 13's notes.
10. [x] Video call support end to end (dial + answer with video, device
        selection wired through, OpenH264 confirmed available in the
        build environment). `call.c`'s `on_call_media_state` now applies
        `device.set_video`'s selected capture device via
        `pjsua_call_set_vid_strm(..., PJSUA_CALL_VID_STRM_CHANGE_CAP_DEV, ...)`
        once the call's video stream actually exists - has to happen
        there, not at dial()/answer() time, since the stream doesn't
        exist yet when `pjsua_call_setting.vid_cnt` just requests one be
        negotiated.

        Found and fixed a second real bug in pjproject's (documented-
        experimental) CMake build, same class as the `FindFFMPEG.cmake`
        issue from earlier: `pjmedia/include/pjmedia-codec/config_auto.h.cm`
        (the generated-header template) is simply missing the
        `#cmakedefine01 PJMEDIA_HAS_OPENH264_CODEC` line, even though
        `pjmedia/CMakeLists.txt` sets that CMake variable right before
        generating the header from the template. Result:
        `PJMEDIA_WITH_OPEN_H264_CODEC=ON` had zero effect at the C level
        - the macro was simply never defined, so `openh264.cpp`'s own
        `#if defined(PJMEDIA_HAS_OPENH264_CODEC)` guard was false and the
        whole file silently compiled to an empty translation unit
        (confirmed with `nm`: zero symbols in the object file, not even
        the expected undefined `WelsCreateSVCEncoder` reference). Same
        macro is *also* checked in `pjsua_vid.c` (the `pjsua-lib`
        target, not `pjmedia-codec`) around the call to
        `pjmedia_codec_openh264_vid_init()` - fixing only the codec's
        own target wasn't enough, since nothing would ever call its init
        function and the linker would drop the whole unreferenced object
        anyway. Worked around in `CMakeLists.txt` with
        `target_compile_definitions(... PJMEDIA_HAS_OPENH264_CODEC=1)`
        on both `pjmedia-codec` and `pjsua-lib`, rather than patching
        pjproject's tracked template file. Verified with `nm`/`ldd`:
        `pjmedia_codec_openh264_vid_init` is defined and reachable, the
        real `Wels*` symbols are referenced, and `libopenh264.so.7` shows
        up as a genuine runtime dependency of the final binary - this
        was previously just silently absent despite the CMake option
        being "on". (Needed `libopenh264-dev` installed locally, which
        wasn't present initially - see the earlier "OpenH264 build
        environment" open question, now resolved for this machine.)

        No SIP server or working test peer is available in this
        sandbox (see steps 8-9's notes), so a real video call can't be
        exercised end to end here either. Rather than leave this
        untested, wrote `tests/protocol/test_live_call.py` - full
        register/dial/answer/video-call/hangup tests written against a
        real registrar, gated on `PJSOCKY_TEST_ASTERISK_*` environment
        variables and skipping cleanly (exit 0) when they're unset. The
        intent is that whoever has real Asterisk access runs this file
        as the actual verification, rather than starting test-writing
        from scratch at that point. Per direct instruction, these were
        written but not required to pass in this environment.
11. [x] Instant messaging: `im.send`, `incoming_message`, `message_status`.
        `src/im.c` follows the same no-JSON-dependency pattern as
        account.c/call.c/device.c - out-of-dialog SIP MESSAGE via
        `pjsua_im_send`, no buddy list involved (matches the earlier
        conversation that settled this: buddies are a presence concept,
        unrelated to sending/receiving SIP MESSAGE). `on_pager2`/
        `on_pager_status2` push events through the same
        `proto/events.c` coordinator as every other async event.
        `mime_type` defaults to `"text/plain"` when the client omits it,
        matching `pjsua_im_send`'s own default.

        No real SIP peer is available in this sandbox to verify actual
        delivery (same gap as steps 8-10), so `tests/protocol/test_basic.py`
        covers what's reachable without one - the registration gate and
        missing-param validation (27/27 passing) - and a real
        `im.send` + `message_status` round trip was added to
        `test_live_call.py` per direct instruction: write it against
        real Asterisk semantics, don't spend time trying to make it
        pass without real infrastructure. Unlike the call/video tests
        there, this one doesn't need a human or second peer to answer
        anything - the PBX itself generates the delivery outcome - so
        it should be one of the easier `test_live_call.py` tests to
        actually exercise once real access exists.
12. [x] Robustness pass: malformed JSON input, client disconnect
        mid-call, command timeouts, daemon behavior on pjsua callback
        errors — write down expected behavior in PROTOCOL.md, then
        implement. Done spec-first as instructed: PROTOCOL.md gained a
        "Robustness" section (malformed-input table, disconnect
        semantics, no-command-timeouts-by-design, callback-error
        policy), a rewritten "Backpressure" section, and a new generic
        `error` event. The audit that preceded it found the spec and
        the implementation had diverged in several places — worth
        listing, since each one was "documented but not true":
        - **Second concurrent connection**: PROTOCOL.md always promised
          an immediate `connection_refused` + close; the implementation
          actually left the second client hanging silently in the
          listen backlog until the first disconnected. Fixed by
          reworking `server.c` from blocking-accept-then-serve into a
          single select() loop over listen + connection sockets
          (connection socket now non-blocking). This also fixed the
          other documented limitation in server.h: SIGTERM/SIGINT are
          now honored within one poll interval even while a client is
          connected (previously the loop couldn't notice the flag until
          the client went away on its own).
        - **Backpressure**: the spec promised drop-oldest event
          queueing, which was never implemented — the real behavior was
          a blocking send() under the shared events lock, so one
          stalled client could freeze pjsua's worker thread (event
          pushes) and all responses, indefinitely. Rather than build
          the queue, the spec was changed (it's still -draft): all
          control-socket writes get a bounded deadline
          (`PJSOCKY_WRITE_TIMEOUT_MSEC` env var, default 5000ms,
          see proto/server.h); a client that stays unreadable past it
          is declared dead and dropped. Reasoning in PROTOCOL.md's
          Backpressure section: connection drop is detectable and
          forces an explicit resync; silent per-event loss isn't.
        - **`account.remove`**: documented in PROTOCOL.md since step 6
          (account.configure's "remove first" reference) but never
          implemented — no dispatch entry existed at all. Implemented
          (refuses while registered, so registration teardown stays an
          explicit observable step).
        - **Malformed JSON / missing `id`**: previously the daemon
          silently closed the connection on any line it couldn't parse
          well enough to answer. Now (per the new spec table): a
          garbage line gets an `error` event with code `bad_request`
          and the connection survives (NDJSON framing means the next
          line is independent); only an oversized (>8192B) line still
          closes, since framing can't be resynchronized mid-line.
        - **Disconnect mid-call** (was an open question in both docs):
          resolved as "the call stays up" — control connection and SIP
          state are fully independent; events while disconnected are
          discarded; a reconnecting client resyncs via `status.get`.
          For the headless call-box use case a crashed controller must
          never tear down an active call.

        Two genuine bugs found by the new tests, both worth the
        pattern-lesson:
        - **Latent framing corruption on pipelined requests** (present
          since step 4, in read_line originally): extract-a-line
          memmove'd the *rest* of the buffer to the front while
          returning a pointer to that same front — two requests
          arriving in one recv() made the daemon parse the second
          request twice and lose the first response entirely. Never
          triggered before because every existing test did one
          request per write, so `remaining` was always 0. Fixed by
          making the framing buffer offset-based: data only moves in
          fill() (when no extracted line is live), never in
          extract_line(). Lesson: "works under the test suite's I/O
          rhythm" is not "works" — the new
          test_pipelined_requests_one_write covers the coalesced case.
        - **Test harness deadlock on failure** (client.py): log()
          read the daemon's stdout to EOF *before* stop() sent
          SIGTERM, so any test failure where the daemon stayed healthy
          hung the whole suite forever. Never seen before because
          earlier failures were daemon *crashes* (EOF arrived). log()
          now stops the daemon first.

        tests/protocol/test_basic.py: 43/43 passing, including the
        robustness matrix (unparseable/non-object/missing-id/
        non-string-id/empty lines survive; oversized line closes;
        second connection refused; state survives reconnect; slow
        reader dropped via a 300ms `PJSOCKY_WRITE_TIMEOUT_MSEC`
        override — tests can now pass per-test env via an `extra_env`
        attribute on the test function).
13. [x] `tests/protocol`: started early (during step 6) rather than
        deferred to the end, after ad-hoc manual verification missed two
        real bugs that an assert-based test caught immediately - see
        step 6's notes. `client.py` (harness: launches an isolated
        daemon via `PJSOCKY_SOCK_PATH`, NDJSON request/response + event
        client) and `test_basic.py` (43 tests and growing - ping,
        status.get, account.* incl. account.remove, device.*, call.*
        incl. call.answer error/gating paths, im.send/im.typing
        error/gating paths, the async reg_state event path, the call_id
        bounds-check crash fix from step 8/9, and step 12's robustness
        matrix: malformed-input handling, second-connection refusal,
        reconnect state survival, slow-reader disconnect, pipelined
        requests) exist now; grows alongside each future command/event
        instead of being written from scratch at the end.
        A real end-to-end call (dial-or-receive, answer, confirmed,
        hangup, incl. video) and a real im.send/message_status round
        trip live in `test_live_call.py`, gated on
        `PJSOCKY_TEST_ASTERISK_*` env vars, skipping cleanly without
        them (step 9's notes cover why a fake local peer via
        pjproject's own `pjsua` CLI didn't pan out). **As of step 16
        these are runnable and passing locally** - see
        `tests/asterisk/run_live_tests.py`, which supplies the PBX and
        those env vars automatically. Uses
        `PJSOCKY_TEST_FAST_TIMERS=1` (main.c) to shrink pjsip's
        transaction timers via `pjsip_tsx_set_timers()` for tests that
        need a registration attempt to fail - see main.c's comment on
        that env var for why (RFC 3261 Timer F is 32s by default, and
        getting the override right took two tries - `pjsip_cfg()->tsx.t1`
        alone does nothing, and the `td` param controls the actual
        give-up deadline despite being documented as INVITE-specific).
14. [x] Packaging: systemd unit + sample config for target deployment.
        `packaging/pjsocky.service` (install instructions in its header
        comment) + `packaging/pjsocky.conf.sample` (environment file
        for `/etc/default/pjsocky` - since config is env-vars-only,
        that file is the entire configuration surface), plus an
        `install(TARGETS ...)` rule in CMakeLists.txt so `cmake
        --install build` puts the binary where the unit's ExecStart
        expects it (/usr/local/bin). Design choices worth knowing:
        - Socket lives in `RuntimeDirectory=pjsocky` ->
          `/run/pjsocky/pjsocky.sock`, mode 0750: the socket's
          filesystem permissions are v1's entire trust boundary
          (docs/PROTOCOL.md "Transport"), so controller processes
          join the pjsocky group rather than the socket being
          world-writable in /tmp.
        - Dedicated `User=pjsocky` with `SupplementaryGroups=audio
          video` for ALSA/V4L2 device access - flagged in the unit as
          the thing to adjust once the target's real device ownership
          model is known.
        - `Restart=always` (unattended call box), `TimeoutStopSec=10`
          (clean SIGTERM shutdown measured ~1-2s, step 5's notes).
        - Moderate hardening only; deliberately NOT PrivateDevices/
          ProtectSystem=strict since calls need real device nodes -
          revisit on target hardware.
        - The unit and env file are NOT installed by CMake - where
          they go is distro/firmware policy; also `cmake --install`
          runs pjproject's own install rules too (subdirectory), so
          the header comment documents manual copying.
        Verified: `systemd-analyze verify` clean (syntax), and a live
        smoke test via a transient systemd unit (`systemd-run --user`)
        - daemon started under systemd, answered hello/ping over the
        socket, `systemctl stop` produced a clean full teardown
        (unit "Stopped", not "Failed") and the socket file was
        removed on shutdown.
15. [x] Public release prep: README with real usage examples, CHANGELOG,
        CONTRIBUTING, confirm COPYING is verbatim GPLv2, CI (GitHub
        Actions build against a pinned pjproject sibling checkout).
        All five delivered:
        - `README.md`: what/why, a real socat session transcript
          (including the event-before-response ordering a client must
          expect after call.dial), sibling-checkout build steps, env-var
          config table, test instructions, license.
        - `docs/CHANGELOG.md`: 0.1.0 (unreleased) with the full v1
          surface; protocol changes get called out here explicitly
          since the wire protocol is the public API.
        - `CONTRIBUTING.md`: the house rules that already existed
          implicitly - spec-first (PROTOCOL.md leads), keep it
          general-purpose, no pjproject modifications, tests grow with
          the surface, -Wall -Wextra clean.
        - `COPYING`: verbatim GPLv2 copied from pjproject's own COPYING
          (339 lines, checked).
        - `.github/workflows/ci.yml`: single job - checkout pjsocky +
          pjproject pinned to eccdebae7 (2.17-5-geccdebae7, the exact
          commit this checkout developed against), apt deps
          (build-essential cmake libasound2-dev libopenh264-dev
          libsdl2-dev), build, test_basic.py, then the full dockerized
          live-call suite (GitHub's Ubuntu runners ship docker).
          **The recipe was rehearsed locally in a clean ubuntu:24.04
          container before writing the YAML** - and that rehearsal
          immediately caught a real issue: a truly headless box
          enumerates *zero* audio devices, which (a) falsified
          test_device_list_audio's "pjmedia always enumerates
          something" assumption (empty list is now accepted - it's a
          truthful answer) and (b) prompted a real daemon improvement:
          main.c now falls back to pjsua's null sound device
          (`pjsua_set_null_snd_dev()`) when `pjmedia_aud_dev_count()`
          is 0, so calls work on headless/containerized systems instead
          of failing to open a sound device at first media. Verified:
          43/43 in the clean container, 43/43 + 5/5 live locally.
        Not done here deliberately: bumping protocol_version from
        "1.0.0-draft" to "1.0.0" - PROTOCOL.md still has two open
        questions (ring timeout, control-socket auth before TCP) that
        could shape v1; tag the protocol when those are settled. (Ring
        timeout was resolved in step 17; the tag itself followed there.)
16. [x] Automatic setup of asterisk server to test against ? Is it
        feasible? - **Yes, done.** `tests/asterisk/` contains a
        throwaway dockerized Asterisk PBX (plain `ubuntu:24.04` + distro
        asterisk package, no third-party image; config bind-mounted
        from `tests/asterisk/etc/`) and `run_live_tests.py`, which
        builds/starts it, runs `tests/protocol/test_live_call.py`
        against it, and tears it down:

            python3 tests/asterisk/run_live_tests.py

        Requires only rootless docker. **All 5 live tests pass**: real
        REGISTER with auth, outgoing audio call to CONFIRMED with media
        up, outgoing H.264 *video* call (has_video confirmed - and
        since OpenH264 is the only video codec compiled in, video-up
        means H.264 genuinely negotiated), im.send with a real 2xx
        delivery status, and an incoming call answered. This closes the
        verification gap carried since steps 8-10 ("no SIP server in
        this sandbox") - the whole call path is now exercised
        automatically, before any target hardware exists.

        Design notes:
        - Container runs with `--network=host`; Asterisk binds
          127.0.0.1:5080 (pjsocky's own UDP transport owns 5060), so
          SIP and RTP stay on loopback with zero NAT/SDP-address
          complications.
        - The "peer" is not a second SIP endpoint: extension 600 is
          `Answer()` + `Echo()`, which bounces audio *and video* frames
          back - answers automatically, no human needed.
        - MESSAGE delivery: a `message_context` dialplan entry is what
          makes Asterisk return the 2xx that `message_status` reports.
        - The incoming-call test (needs someone to dial *us*) is
          automated by the runner watching test output for the
          "waiting for an incoming call" prompt and firing
          `asterisk -rx "channel originate PJSIP/1000 application Echo"`
          in the container.

        Two test-harness bugs surfaced (both fixed; neither was a
        daemon bug):
        - `client.py`'s `call()` assumed no event could arrive between
          request and response. Wrong per spec *and* in practice:
          pjsua fires `on_call_state(CALLING)` synchronously inside
          `pjsua_call_make_call()`, so the `call_state` event hits the
          wire before `call.dial`'s own response. `call()` now queues
          events for `read_event()`. PROTOCOL.md deliberately makes no
          response/event ordering promise - clients must handle this.
        - The runner initially watched the test's stdout for the
          incoming-call prompt through a block-buffered pipe, so the
          prompt arrived only after the test had already timed out;
          fixed with `python -u`.
17. [x] Incoming-call ring timeout made configurable over the protocol,
        resolving the open question carried since v1 (docs/PROTOCOL.md
        "Open questions"). `call.c` gains a `pj_timer_entry` (armed only
        while an incoming call is INCOMING/EARLY, disarmed the moment it
        leaves that state via `on_call_state`, regardless of what caused
        the transition) plus `pjsocky_call_set_ring_timeout()`/
        `_get_ring_timeout()`; `0` (the default) keeps the original
        unbounded-ring behavior. New commands `config.set_ring_timeout`/
        `config.get_ring_timeout` in `dispatch.c`. On expiry the call is
        auto-rejected with `480 Temporarily Unavailable` via
        `pjsua_call_hangup()` - chosen over `486 Busy Here` since the
        daemon isn't actually busy, it just gave up waiting.

        One subtlety worth remembering for any future pjsua timer use:
        `pjsua_schedule_timer2()` (the simpler callback+user_data form)
        gives no handle back to cancel with, so it can't be used here -
        answering/hanging up before the timeout must be able to cancel
        the pending auto-reject. Used the lower-level
        `pjsua_schedule_timer()`/`pjsua_cancel_timer()` with an explicit
        `pj_timer_entry` instead, storing the target `call_id` in
        `entry.user_data` so the callback can re-check (via
        `pjsua_call_get_info()`) that the call is still actually ringing
        before acting - pjsip's timer heap can already have dequeued the
        callback by the time a same-tick cancel would run.

        Verified: `tests/protocol/test_basic.py` covers the config
        set/get round trip and validation (48/48 passing); a new
        `test_ring_timeout_auto_rejects` in `tests/protocol/
        test_live_call.py` sets a 3s timeout, deliberately does not
        answer an incoming call, and confirms `call_state` reaches
        `DISCONNECTED` with `last_status: 480` - passing against the
        real dockerized Asterisk PBX from step 16 (6/6 live tests).

        With this resolved, only one of the two open questions blocking
        the protocol tag (step 15's note) remained - control-socket auth
        beyond filesystem permissions - and that one is explicitly scoped
        to "before exposing the TCP transport option outside development
        builds" (docs/PROTOCOL.md "Open questions"), not the Unix-socket
        default transport. Judged not blocking for a v1 tag: bumped
        `protocol_version` from `"1.0.0-draft"` to `"1.0.0"` in
        `server.c`, and dropped the "-draft" framing from
        `docs/PROTOCOL.md`'s header, `README.md`'s example session, and
        `docs/CHANGELOG.md`. Per `PROTOCOL.md`'s own versioning note,
        the document is now append-only within the 1.x line.

18. [x] First real hardware bring-up on the STM32MP15 target
        (root@192.168.7.2, OpenSTLinux, tp4 firmware; pjsocky runs as a
        child of `tp4-app` from `/opt/tp4/current/bin/`, socket
        `/tmp/pjsocky.sock`). Reported symptom: "media throughput does
        not work on my device, others report it works on theirs".
        Investigated by stopping `tp4`, running a freshly cross-built
        pjsocky by hand (`PJSOCKY_LOG_LEVEL=5`), and calling the
        dockerized test PBX from `tests/asterisk/` (bound to
        `0.0.0.0:5080` instead of loopback so the device can reach it)
        while watching the RTP counters `call.get_info` gained in
        commit d896550. Four separate things were found; only the first
        is fixed in the tree.

        Bench tooling note: the target's SSH server does not support
        `direct-streamlocal`, so `ssh -L <port>:/tmp/pjsocky.sock` fails
        with "unknown channel type", and the target has no python3,
        socat, or `nc -U`. Driving the control socket from a laptop
        needs a tiny stdio<->unix-socket relay cross-compiled and run
        over plain `ssh` stdio instead.

        - **The null video renderer could not change format** (fixed in
          `src/null_video_dev.c`). `info.caps` was 0 and `stream_set_cap`
          returned `PJMEDIA_EVID_INVCAP` for everything, including
          `PJMEDIA_VID_DEV_CAP_FORMAT`. pjmedia's `vid_port`
          (`handle_format_change()`) calls exactly that whenever the
          size a render port was opened with differs from the real one -
          which is always, for both render ports pjsua creates: the
          remote-video window (opened at a guess, corrected when the
          first frame decodes: `656x656 -> 720x480` in the log) and the
          hidden capture preview (opened at the requested 720x480 while
          the camera actually opened as YUY2 640x480). The failure is
          not cosmetic: vid_port reverts to the old format and then
          rejects every frame with "Unexpected frame size 645504,
          expected 518400" (= I420 656x656 vs I420 720x480), publishing
          a `PJMEDIA_EVENT_VID_DEV_ERROR` per frame. Fix: advertise
          `PJMEDIA_VID_DEV_CAP_FORMAT` and have get/set_cap store and
          return `strm->param.fmt` - a discarding sink can accept any
          format, but it must *remember* it, since vid_port reads it
          back via `get_param()` on the next change. Verified: the
          error storm is gone (0 occurrences in a full call),
          `test_basic.py` 49/49 and the dockerized live suite 6/6 still
          pass. This is the "works on their device" difference: a real
          SDL/render device implements CAP_FORMAT, so nobody with a
          display ever hit it.
        - **Silence detection (VAD) silently stopped audio RTP**
          (fixed: `no_vad` now defaults to true, `PJSOCKY_VAD=1` turns
          the detector back on). pjsua's default
          `pjsua_media_config.no_vad = PJ_FALSE` is inherited as-is in
          `main.c`. On this hardware both microphones are quiet
          (recorded room noise: max9860 rms 449, USB CAM rms 236 out of
          32768), which puts them right at pjmedia's silence threshold.
          Observed: a call on the USB CAM mic transmitted 33 audio
          packets (~0.6s) and then nothing, while video kept flowing -
          the log says "VAD re-enabled" / "Starting silence". Proven
          causal both ways: a bench-only `PJSOCKY_NO_VAD` build
          restored a continuous 50 pkt/s, and dropping the CAM mixer
          gain to 5% froze TX at 70 packets deterministically. It is
          intermittent at stock gain - the same setup streamed fine on
          a later run - which is exactly the shape of "it works for
          them, not for me". Resolved by inverting the library default
          in `main.c` - which is also what pjsua's own demo app does
          with `--no-vad` - and keeping the switch as an env var, per
          the env-vars-only config decision. Continuous RTP is also
          what makes `call.get_info`'s packet counters usable as a
          health signal: with VAD on, "tx stopped" means nothing.
        - **SDP media address on a multi-homed device** (investigated,
          nothing to fix - bench artifact). The target has two
          interfaces (`enu1i4` 192.168.10.2,
          the default route, and `enu1u3` 192.168.7.2). SIP itself is
          fine - pjsip picks the source address per destination, so the
          Via/Contact correctly said 192.168.7.2 - but pjmedia's RTP
          address comes from `pj_gethostip()` once at init, so the SDP
          carried `c=IN IP4 192.168.10.2` for a peer reachable only via
          192.168.7.x. Result: signalling and TX look perfect,
          `audio_rx_packets` stays 0 forever. It only recovers if the
          far end does symmetric RTP (adding `rtp_symmetric = yes` to
          the test PBX endpoint fixed it immediately, and is what a
          NAT-facing production PBX does). `main.c` sets no
          `pjsua_media_config.bound_addr`/`public_addr` and pjsocky
          exposes no way to, so on a multi-homed box the media address
          is simply whichever interface owns the default route.
          Confirmed with the hardware owner afterwards: the 192.168.7.x
          link is the development PC's SSH link only and is never a
          route to anything, so the default-route address pjmedia picks
          is the correct one in production and only the bench PBX (which
          lived on the PC) was reachable by a path the SDP did not name.
          Recorded rather than fixed - but worth remembering, because
          the symptom (registration fine, calls connect, TX counts up,
          RX flat at zero forever) looks exactly like a broken far end,
          and any future bench PBX on the PC link needs
          `rtp_symmetric = yes` to work at all.
        - **Video ran at ~8-10 fps of the negotiated 15** (fixed as
          far as it can be, with `PJSOCKY_VIDEO_SIZE`/
          `PJSOCKY_VIDEO_FPS`). pjsua asks the camera
          for I420 720x480; the UVC camera has no I420 and no usable
          MJPEG for pjmedia, so v4l2 opens YUY2 640x480 and every frame
          is colour-converted *and* rescaled before H.264 encode. With
          decode of the echoed 720x480 stream on top, pjsocky uses
          ~0.8 of one Cortex-A7 core and the log shows steady
          `capdbuf Underflow` and `Skipped N frames to reduce delay`.
          `main.c` now applies `PJSOCKY_VIDEO_SIZE`/`PJSOCKY_VIDEO_FPS`
          to every registered video codec after `pjsua_init()` (codecs
          register during init, so it cannot happen earlier), which lets
          the encoder be set to a size the camera produces natively.
          Measured on this target with `PJSOCKY_VIDEO_SIZE=640x480`:
          video RTP went from 8.5 to 13 packets/sec (of a 15 fps
          target), at ~0.93 of one Cortex-A7 core. The remaining gap is
          the YUY2->I420 colour conversion, which cannot be avoided -
          the camera has no I420 mode and its MJPEG is not a format
          pjmedia's v4l2 driver accepts. Deliberately not automatic:
          picking a size out of the device's own format list means
          picking, and getting it wrong (this camera also offers
          2592x1944) is far worse on an A7 than leaving the codec
          default alone. **The target's launcher has to set this** -
          pjsocky is started by `tp4-app`, so the variable belongs
          wherever that spawns it.

        Both required configurations were re-verified after the fixes,
        against the bench PBX with the RTP counters as the evidence:
        max9860 audio-only (50 pkt/s each way, sustained) and USB CAM
        mic + USB CAM video (50 pkt/s audio each way, 13 pkt/s video
        each way, no format errors in the log). `test_basic.py` 52/52
        (three new tests cover the new env vars, including that a
        malformed one is ignored rather than fatal) and the dockerized
        live suite 6/6.

        Also worth knowing about the audio devices themselves: use
        `default:CARD=CAM` (the ALSA plug device) as the USB CAM
        capture id. `front:CARD=CAM,DEV=0` is enumerated too, but it is
        stereo-only and `call.dial` fails on it with
        `PJMEDIA_EAUD_SYSERR` ("Unable to set a channel count of 1").
        max9860 (`default:CARD=max9860hifi`) opens cleanly for both
        directions at 16 kHz mono.

19. [x] Video faults no longer scream, and no longer cost the call.
        Reported after step 18: "if configured to use video but no video
        device is present, pjsocky screams with errors - one warning
        would be enough, otherwise continue with the call, but without
        video". Reproduced on the target by unbinding the USB camera
        (`echo 1-1.1.1.2:1.0 > /sys/bus/usb/drivers/uvcvideo/unbind`),
        which turns out to expose three distinct cases, not one:

        - **Camera gone before the call.** pjsua fails the video channel
          update once, tears the video stream down and the call runs as
          an audio call - a handful of log lines, already fine.
        - **Camera gone after startup but before enumeration matters.**
          pjmedia's v4l2 factory enumerates once at startup, so the
          camera simply is not in `device.list_video`, and the account's
          fallback pick lands on pjmedia's **colorbar generator** - a
          synthetic test source that produces frames happily. Video
          "works" and the far end sees a test pattern. Nothing said so
          before; `account.c` now warns once when the fallback resolves
          to a Colorbar device, and once when there is no capture device
          at all.
        - **Camera pulled during a call - this is the scream.**
          `vid_conf.c` retries the dead capture port once per frame
          interval and logs `Failed to get frame from port N [...]: No
          such device` every time - 312 lines in 20 seconds and
          unbounded, while the call stays up and audio is unaffected.
          It is not a `PJMEDIA_EVENT_VID_DEV_ERROR`; nothing in
          pjsua-lib times it out, and no event ever tells the
          controlling application that the video it negotiated is dead.

        Fixed in `call.c` with one shared path, `give_up_on_video()`:
        warn once, `pjsua_call_set_vid_strm(PJSUA_CALL_VID_STRM_REMOVE)`
        (a re-INVITE without the m=video line, which also destroys the
        capture port - that is what actually stops the retry loop), and
        mark the call so every later report of the same fault is
        silent. It is reached two ways:
        - `on_call_media_event` (newly registered in `main.c`) for
          `PJMEDIA_EVENT_VID_DEV_ERROR`. Safe to call
          `pjsua_call_set_vid_strm()` from there: pjsua-lib dispatches
          that callback from a timer on its worker thread
          (`call_med_event_cb` in pjsua_media.c), not from the media
          thread that published the event.
        - A watchdog polling the video TX packet counter every 3s, for
          the silent kind. A sending stream (ACTIVE, encoding direction,
          so a far-end hold is excluded by construction) that sends
          nothing across a window is dead. A stream that has never sent
          anything gets two windows instead of one - a first keyframe on
          a slow encoder is not the same thing as a stopped camera.

        Result on hardware: `Failed to get frame` went from unbounded to
        50 lines (~3s), followed by one `call.c` warning, `has_video`
        flipping to false, a `call_media_state` event, and the audio
        call carrying on untouched. A healthy 30s video call is
        unaffected (310 video packets, watchdog never fires).
        `docs/PROTOCOL.md`'s `call_media_state` now documents that a
        call can lose video and keep going, since that is a wire-visible
        behaviour a client has to expect. test_basic.py 52/52, live
        suite 6/6.

        Testing note for whoever repeats this: `run_live_tests.py` fails
        its two incoming-call tests with a bare "timed out" if any other
        Asterisk is already bound to host port 5080 (e.g. a bench PBX
        left running from step 18) - registration silently goes to the
        wrong one, so the outgoing tests still pass. Check for a stray
        container before believing that failure.

## Non-goals (explicit, revisit only with a real requirement)

- Multiple simultaneous accounts
- Multiple simultaneous calls / call transfer / call hold beyond whatever
  is trivially free from pjsua-lib defaults
- Any proprietary-shaped extension. If a downstream consumer needs
  something pjsocky doesn't do, the default answer is "extend the
  generic protocol in a way that would make sense to any consumer," not
  "add a private command."
