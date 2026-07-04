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

GPLv2, matching pjproject. `COPYING` will be a copy of pjproject's GPLv2
text. This repository is public. Do not add code here that only makes
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
  COPYING                  # GPLv2 (copied from pjproject)
  README.md
  CONTEXT.md                # this file
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
  packaging/
    pjsocky.service           # systemd unit for tp4/tp7 targets
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
  deliberate v1 simplification, revisit once we know if tp4/tp7 need
  concurrent controllers.
- **Idle by default**: the daemon starts with `pjsua_init`/`pjsua_start`
  and zero accounts. No SIP traffic happens until a client sends
  `account.configure` followed by `account.register`.
- **One account at a time in v1.** Multi-account is a later extension, not
  a v1 requirement — keep the protocol namespaced (`acc_id` present in
  messages) so it's not a breaking change to add later.
- **No GUI/video rendering window.** Video capture/render are headless;
  frames go through whatever pjmedia video device/port is wired to tp4/tp7
  hardware. Exact hardware video path is a TODO/open question (see below).
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
  build lightweight and dependency-light on tp4/tp7, and matches the
  actual requirement rather than pjproject's kitchen-sink default. See the
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

- [ ] Confirm exact tp4/tp7 audio device backend (ALSA? custom pjmedia
      audiodev driver?) and how it enumerates through `pjsua_enum_aud_devs`.
- [ ] Confirm tp4/tp7 video hardware path — is there already a pjmedia
      video device driver, or does pjsocky need one written?
- [ ] Decide daemon supervision: systemd unit vs. whatever init tp4/tp7
      firmware uses. - **Decided: systemd.** Not yet implemented -
      `packaging/pjsocky.service` is still a step 14 (packaging) task.
- [x] Decide config file format for the socket path, log level, etc.
      (separate from the runtime protocol). - **Decided: env vars, no
      config file.** Implemented: `PJSOCKY_SOCK_PATH` (since step 6) and
      `PJSOCKY_LOG_LEVEL` (0-6, overrides both `pjsua_logging_config.level`
      and `.console_level`; unset uses pjsua's own defaults 5/4) - see
      `main.c`.
- [x] Decide protocol version negotiation: a `hello`/handshake message
      exchanged on connect, or just documented compatibility per release
      tag? - **Decided: keep `hello`.** Implemented in `server.c`
      (`build_hello_data`, sent via `pjsocky_events_push()` right after
      `pjsocky_events_set_conn()`, before the request loop starts) -
      `docs/PROTOCOL.md` already specified the wire format, it just
      wasn't wired up in code until now. `protocol_version` is still
      reported as `"1.0.0-draft"` (see `PJSOCKY_PROTOCOL_VERSION` in
      `server.c`) - bump that once the v1 surface is considered stable
      enough to tag, per `PROTOCOL.md`'s own versioning note.
      `daemon_version` comes from `PJSOCKY_VERSION`, a compile definition
      derived from `CMakeLists.txt`'s `project(... VERSION ...)` rather
      than a second hardcoded string.
- [x] Typing indicators (`on_typing2`/`SIP MESSAGE` composing state) are
      not in v1's `im.send`/`incoming_message` pair — decide if tp4/tp7
      actually needs them before adding. - **Decided: add them.**
      Implemented as `im.typing` (wraps `pjsua_im_typing`, out-of-dialog,
      same model as `im.send`) and a `typing` event (from `on_typing2`).
      Out-of-dialog only for v1 - in-dialog typing indications during an
      active call (`pjsua_call_send_typing_ind` and its receive-side
      counterpart) are a possible future addition, not implemented now -
      see `docs/PROTOCOL.md`'s `typing` event section and Non-goals.
- [x] OpenH264: confirm the tp4/tp7 build environment can provide the
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
        intent is that whoever has real Asterisk access (e.g. against
        lift-emg-tel's actual PBX) runs this file as the actual
        verification, rather than starting test-writing from scratch at
        that point. Per direct instruction, these were written but not
        required to pass in this environment.
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
12. [ ] Robustness pass: malformed JSON input, client disconnect
        mid-call, command timeouts, daemon behavior on pjsua callback
        errors — write down expected behavior in PROTOCOL.md, then
        implement.
13. [~] `tests/protocol`: started early (during step 6) rather than
        deferred to the end, after ad-hoc manual verification missed two
        real bugs that an assert-based test caught immediately - see
        step 6's notes. `client.py` (harness: launches an isolated
        daemon via `PJSOCKY_SOCK_PATH`, NDJSON request/response + event
        client) and `test_basic.py` (27 tests and growing - ping,
        status.get, account.*, device.*, call.* incl. call.answer
        error/gating paths, and im.send error/gating paths, plus the
        async reg_state event path and the call_id bounds-check crash
        fix from step 8/9) exist now; grows alongside each future
        command/event instead of being written from scratch at the end.
        A real end-to-end call (dial-or-receive, answer, confirmed,
        hangup, incl. video) and a real im.send/message_status round
        trip are written but not runnable here
        - `test_live_call.py`, gated on `PJSOCKY_TEST_ASTERISK_*` env
        vars, skips cleanly without them (see step 10's notes; step 9's
        notes cover why a fake local peer via pjproject's own `pjsua`
        CLI didn't pan out either). Uses
        `PJSOCKY_TEST_FAST_TIMERS=1` (main.c) to shrink pjsip's
        transaction timers via `pjsip_tsx_set_timers()` for tests that
        need a registration attempt to fail - see main.c's comment on
        that env var for why (RFC 3261 Timer F is 32s by default, and
        getting the override right took two tries - `pjsip_cfg()->tsx.t1`
        alone does nothing, and the `td` param controls the actual
        give-up deadline despite being documented as INVITE-specific).
14. [ ] Packaging: systemd unit + sample config for tp4/tp7.
15. [ ] Public release prep: README with real usage examples, CHANGELOG,
        CONTRIBUTING, confirm COPYING is verbatim GPLv2, CI (GitHub
        Actions build against a pinned pjproject sibling checkout).
16. [ ] Automatic setup of asterisk server to test against ? Is it feasible?

## Non-goals (explicit, revisit only with a real requirement)

- Multiple simultaneous accounts
- Multiple simultaneous calls / call transfer / call hold beyond whatever
  is trivially free from pjsua-lib defaults
- Any proprietary-shaped extension. If `lift-emg-tel` needs something
  pjsocky doesn't do, the default answer is "extend the generic protocol
  in a way that would make sense to any consumer," not "add a private
  command."
