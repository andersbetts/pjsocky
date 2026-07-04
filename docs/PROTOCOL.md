# pjsocky control protocol

Version: **1.0.0-draft** (no wire bytes shipped yet — draft until v1.0.0 is
tagged, then this document is append-only within the 1.x line: fields may
be added, never removed or repurposed. Breaking changes bump to 2.x and
must be negotiable, see [Versioning](#versioning)).

This document is the single source of truth for the wire protocol. Do not
let the C implementation grow behavior that isn't described here first.

## Design goals

- **Generic.** Nothing in this protocol is shaped around any one
  downstream consumer. It should read as sensible to anyone controlling a
  headless SIP audio/video endpoint.
- **Transparent and debuggable.** A human must be able to drive it with
  `socat - UNIX-CONNECT:/run/pjsocky.sock` and type JSON by hand.
- **Small.** One connection, one account, one call at a time in v1. See
  [Non-goals](#non-goals).

## Transport

- Default: Unix domain stream socket. Path is a daemon startup option
  (e.g. `/run/pjsocky.sock`), not part of this protocol.
- Optional (development builds only, off by default): TCP on loopback.
- Exactly **one control connection** is accepted at a time. If a second
  client connects while one is active, the daemon sends it a single
  `error` message with `code: "connection_refused"` and closes the
  connection. It does not evict the existing client.
- The daemon does not require authentication at the transport layer in
  v1 — the socket's filesystem permissions (or loopback-only binding) are
  the trust boundary. This is a deliberate v1 scope limit, see
  [Open questions](#open-questions).

## Framing

Newline-delimited JSON (NDJSON): each message is exactly one JSON object
(RFC 8259) followed by a single `\n` (0x0A). No embedded newlines inside a
message — the JSON body must be serialized without pretty-printing.

- Max line length: 8192 bytes. A line exceeding this is a framing error
  (see [Errors](#errors)); the daemon closes the connection.
- Encoding: UTF-8.
- Implementation note: use `pjlib-util`'s `pj_json_parse`/`pj_json_write`
  (`pjlib-util/include/pjlib-util/json.h`) for both directions.

## Message envelope

There are exactly three message shapes on the wire. The presence/absence
of `id` is what distinguishes them — there is no separate `"type"` field.

### 1. Request (client -> daemon)

```json
{"id": "1", "cmd": "call.dial", "params": {"uri": "sip:100@example.com"}}
```

- `id` (string, required): client-assigned correlation token, echoed back
  in the response. Must be unique among the client's currently
  outstanding requests. Any string is legal (`"1"`, a UUID, etc.) — the
  daemon treats it as an opaque token, never interprets it.
- `cmd` (string, required): dotted command name, see [Commands](#commands).
- `params` (object, optional): command-specific arguments. Omit entirely
  for commands that take none (`{"id":"2","cmd":"status.get"}` is valid).

### 2. Response (daemon -> client, one per request)

Success:

```json
{"id": "1", "ok": true, "result": {"call_id": 0}}
```

Failure:

```json
{"id": "1", "ok": false, "error": {"code": "invalid_state", "message": "no account registered"}}
```

- `id` echoes the request's `id` exactly.
- `ok` (bool, required).
- `result` (object, present iff `ok:true`). `{}` if the command has no
  meaningful result.
- `error` (object, present iff `ok:false`): `code` (string, stable
  machine-readable token from the [Errors](#errors) table) and `message`
  (string, human-readable, not stable, do not pattern-match on it).

Responses are sent in the order the corresponding requests were fully
received. The daemon does not pipeline out of order.

### 3. Event (daemon -> client, unsolicited)

```json
{"event": "call_state", "data": {"call_id": 0, "state": "CONFIRMED", "last_status": 200}}
```

- No `id` field — this is exactly how a client distinguishes an event
  from a response. A message with `id` is a response; a message without
  `id` is an event. (`cmd`/`event` being mutually exclusive keys reinforces
  this but `id`'s presence is the authoritative discriminator.)
- `event` (string, required): dotted or snake event name, see
  [Events](#events).
- `data` (object, required, may be `{}`).

Events are not correlated to any request and are not acknowledged. A
client that isn't reading fast enough is responsible for its own socket
buffer; the daemon does not block indefinitely on a slow reader (see
[Backpressure](#backpressure)).

## Versioning

On connect, the daemon proactively sends a `hello` event before anything
else, with no request required:

```json
{"event": "hello", "data": {"protocol_version": "1.0.0", "daemon_version": "0.1.0"}}
```

- `protocol_version` follows semver against *this document*. Same major
  version = wire-compatible; a client should refuse to operate (or warn)
  against a daemon whose major version it doesn't recognize.
- Within a major version, only additive changes are made (new optional
  fields, new commands, new events). A client must ignore unknown fields
  and unknown event names rather than erroring.
- `daemon_version` is informational (pjsocky's own release version), not
  meaningful for compatibility decisions.

## Backpressure

If the daemon's outgoing socket buffer for events is full because the
client isn't reading, the daemon drops the *oldest* queued event first
(events are not guaranteed delivery) rather than blocking the pjsua
callback thread. A client that needs a reliable snapshot after a gap
should call `status.get` / `call.get_info`, which always reflect current
state regardless of any events missed.

## Errors

| `code`                 | Meaning                                                        |
|------------------------|------------------------------------------------------------------|
| `bad_request`          | Malformed JSON, missing `cmd`, wrong param types                |
| `unknown_command`      | `cmd` not recognized                                             |
| `invalid_params`       | `cmd` recognized, `params` fail validation for that command      |
| `invalid_state`        | Command not valid in current daemon/account/call state           |
| `not_found`            | Referenced `acc_id`/`call_id`/device id does not exist            |
| `pjsua_error`          | Underlying `pjsua_*` call returned non-`PJ_SUCCESS`; `message` carries `pj_strerror()` text |
| `connection_refused`   | Sent to a second concurrent connection attempt, then socket closed |
| `internal_error`       | Unexpected daemon-side failure                                    |

## Commands

Command params/results below list only the fields relevant to pjsocky's
v1 scope; underlying `pjsua_*` structs have more fields pjsocky does not
yet expose (kept deliberately minimal per `CONTEXT.md`).

### `ping`

Params: none. Result: `{}`. Trivial liveness check.

### `status.get`

Params: none.

Result:
```json
{
  "state": "idle",
  "acc_id": -1,
  "reg_status": 0,
  "call_id": -1
}
```
`state` is one of `idle | registering | registered | in_call`. `acc_id`/
`call_id` are `-1` (mirrors `pjsua`'s own `PJSUA_INVALID_ID`) when absent.
`reg_status` is the last SIP status code from `pjsua_acc_info.status`, `0`
if never registered.

### `device.list_audio`

Params: none.

Result:
```json
{
  "devices": [
    {"id": 0, "name": "default", "input_channels": 1, "output_channels": 2}
  ]
}
```
Wraps `pjsua_enum_aud_devs()` / `pjmedia_aud_dev_info` (`id`, `name`,
`input_count`, `output_count`).

### `device.list_video`

Params: none.

Result:
```json
{
  "devices": [
    {"id": 0, "name": "tp7-cam0", "driver": "v4l2", "dir": "capture"}
  ]
}
```
Wraps `pjsua_vid_enum_devs()` / `pjmedia_vid_dev_info` (`id`, `name`,
`driver`, `dir` one of `capture|render|capture_render`).

### `device.set_audio`

Params: `{"capture_id": 0, "playback_id": 0}` (both required, integers as
returned by `device.list_audio`). Wraps `pjsua_set_snd_dev`. Result: `{}`.
Valid in any daemon state; takes effect on the currently open sound
device / next call.

### `device.set_video`

Params: `{"capture_id": 0}` (required). Selects the capture device used by
subsequent `call.dial`/`call.answer` calls with `video:true`. Does not
itself open the device (rendering/capture opens at call media setup, per
`on_call_media_state`). Result: `{}`.

### `account.configure`

Params:
```json
{
  "sip_uri": "sip:1000@example.com",
  "registrar_uri": "sip:example.com",
  "username": "1000",
  "password": "secret",
  "realm": "*"
}
```
All fields required except `realm` (defaults to `"*"`, matching
`pjsip_cred_info` wildcard convention). Wraps `pjsua_acc_add` with
`register_on_acc_add = PJ_FALSE` — configuring an account never implicitly
registers it. Calling this while an account already exists returns
`invalid_state` (`account.remove` first, see below) — v1 is single
account only.

Result: `{"acc_id": 0}`.

### `account.remove`

Params: none (operates on the single v1 account). Wraps
`pjsua_acc_del`. Valid only when not registered (`account.unregister`
first) — returns `invalid_state` otherwise. Result: `{}`.

### `account.register`

Params: none. Wraps `pjsua_acc_set_registration(acc_id, PJ_TRUE)`.
Returns immediately (`{}`); actual outcome arrives via the `reg_state`
event, not the response — registration is asynchronous SIP signalling
and the response only confirms the request was accepted for processing.
Returns `invalid_state` if no account configured.

### `account.unregister`

Params: none. Wraps `pjsua_acc_set_registration(acc_id, PJ_FALSE)`. Same
async-via-event caveat as `account.register`.

### `call.dial`

Params: `{"uri": "sip:100@example.com", "video": false}` (`video`
optional, default `false`). Wraps `pjsua_call_make_call`. Returns
`invalid_state` if not registered. Result: `{"call_id": 0}`. Call
progress arrives via `call_state`/`call_media_state` events.

### `call.answer`

Params: `{"call_id": 0, "code": 200, "video": false}` (`code` optional,
default `200`; `video` optional, default matches whether the incoming
offer had video — see [Open questions](#open-questions)). Wraps
`pjsua_call_answer2`. Result: `{}`.

### `call.hangup`

Params: `{"call_id": 0, "code": 486}` (`code` optional, default `486`
Busy Here for an active/incoming call the local side is rejecting; the
daemon does not attempt to infer a "more correct" code — caller decides).
Wraps `pjsua_call_hangup`. Result: `{}`.

### `call.hangup_all`

Params: none. Hangs up the current call if any (v1 is single-call); a
no-op returning `{}` if idle, not an error.

### `im.send`

Params: `{"to": "sip:100@example.com", "content": "hello", "mime_type": "text/plain"}`
(`mime_type` optional, default `"text/plain"`). Wraps `pjsua_im_send`. This
is an out-of-dialog SIP MESSAGE — it is not tied to `call_id` and does not
require an active call. Returns `invalid_state` if no account is
registered.

Result: `{}`. As with `account.register`, the response only confirms the
request was accepted for processing — actual delivery outcome (SIP
success/failure response to the MESSAGE) arrives via the `message_status`
event, not the response. This mirrors `pjsua_im_send`'s own async design
(delivery status reported through `on_pager_status2`, not the call return
value).

### `im.typing`

Params: `{"to": "sip:100@example.com", "is_typing": true}` (both
required). Wraps `pjsua_im_typing` — an out-of-dialog composing
indication (RFC 3994), same "no dialog/call needed" model as `im.send`.
Returns `invalid_state` if no account is registered.

Result: `{}`. Fire-and-forget: there is no delivery-outcome event for
this (unlike `im.send`/`message_status`) — a composing indication is
inherently best-effort and pjsua does not report one back.

### `call.get_info`

Params: `{"call_id": 0}`. Wraps `pjsua_call_get_info`.

Result:
```json
{
  "call_id": 0,
  "state": "CONFIRMED",
  "last_status": 200,
  "last_status_text": "OK",
  "remote_info": "\"Alice\" <sip:100@example.com>",
  "has_audio": true,
  "has_video": false,
  "connect_duration_sec": 42
}
```
`state` is the string name of the `pjsip_inv_state` enum value (`NULL |
CALLING | INCOMING | EARLY | CONNECTING | CONFIRMED | DISCONNECTED`).

## Events

### `hello`
Sent once, immediately on connect, before any response. See
[Versioning](#versioning).

### `reg_state`
```json
{"event": "reg_state", "data": {"acc_id": 0, "status": 200, "status_text": "OK", "registered": true}}
```
Fired from `on_reg_state2`. `status` is the SIP status code (0 if the
attempt never reached the network, e.g. DNS failure). `registered` is
`true` only if this attempt was a REGISTER (not an un-REGISTER) that got
a 2xx response - **not** the same thing as `pjsua_acc_info.has_registration`,
which just means "an account has a `reg_uri` configured" and is true
immediately after `account.configure`, before any registration attempt
happens. (An earlier draft of this doc conflated the two - the daemon
tracks the real state explicitly instead of deriving it from that flag.)

### `incoming_call`
```json
{"event": "incoming_call", "data": {"call_id": 0, "acc_id": 0, "from": "sip:200@example.com", "has_video": false}}
```
Fired from `on_incoming_call`. The daemon does **not** auto-answer or
auto-reject — the client must call `call.answer` or `call.hangup`. There
is no default timeout in v1 (see [Open questions](#open-questions)); the
call rings until the client acts or the remote cancels.

### `call_state`
```json
{"event": "call_state", "data": {"call_id": 0, "state": "CONNECTING", "last_status": 180, "last_status_text": "Ringing"}}
```
Fired from `on_call_state` on every transition. When `state` is
`DISCONNECTED`, this is the terminal event for that `call_id` — no
further events reference it, and `call_id` may be reused by a later call.

### `call_media_state`
```json
{"event": "call_media_state", "data": {"call_id": 0, "has_audio": true, "has_video": false}}
```
Fired from `on_call_media_state`. May fire more than once per call (e.g.
media renegotiation) — always reflects current state, not a delta.

### `incoming_message`
```json
{"event": "incoming_message", "data": {"from": "sip:200@example.com", "to": "sip:1000@example.com", "mime_type": "text/plain", "body": "hi"}}
```
Fired from `on_pager2` for an incoming out-of-dialog SIP MESSAGE. Not
correlated to any call — `call_id` is intentionally absent here (unlike
`pjsua`'s raw callback signature, which carries a `call_id` that is
`PJSUA_INVALID_ID` for out-of-dialog IM anyway; the protocol just omits
the field rather than exposing an always-invalid value).

### `message_status`
```json
{"event": "message_status", "data": {"to": "sip:100@example.com", "body": "hello", "status": 200, "reason": "OK"}}
```
Fired from `on_pager_status2`, reporting delivery outcome for a prior
`im.send`. `status` is the SIP status code for the MESSAGE transaction (0
if it never reached the network).

### `typing`
```json
{"event": "typing", "data": {"from": "sip:200@example.com", "to": "sip:1000@example.com", "is_typing": true}}
```
Fired from `on_typing2` for an incoming composing indication (RFC 3994).
Same shape reasoning as `incoming_message` — no `call_id` field, since
this is the out-of-dialog case (`on_typing2`'s `call_id` argument is
`PJSUA_INVALID_ID` here; in-dialog typing indications received during an
active call are a possible future addition, not handled in v1 - see
`pjsua_call_send_typing_ind`'s counterpart for *sending* one in-dialog,
also not exposed yet).

## Codecs

Audio: G.711 ulaw/alaw only. Video: H.264 only, via OpenH264. These are
build-time choices (`CMakeLists.txt` pins them explicitly, see
`CONTEXT.md`), not something a client selects over the protocol in v1 —
there is exactly one audio codec and one video codec compiled in, so
there is nothing to negotiate from the control protocol's point of view.
If a future requirement needs runtime codec selection (e.g. multiple
codecs compiled in, client picks per call), that's a protocol version
bump, not a v1 concern.

## Example session

```
-> (connect)
<- {"event":"hello","data":{"protocol_version":"1.0.0","daemon_version":"0.1.0"}}
-> {"id":"1","cmd":"device.list_audio"}
<- {"id":"1","ok":true,"result":{"devices":[{"id":0,"name":"default","input_channels":1,"output_channels":2}]}}
-> {"id":"2","cmd":"account.configure","params":{"sip_uri":"sip:1000@example.com","registrar_uri":"sip:example.com","username":"1000","password":"secret"}}
<- {"id":"2","ok":true,"result":{"acc_id":0}}
-> {"id":"3","cmd":"account.register"}
<- {"id":"3","ok":true,"result":{}}
<- {"event":"reg_state","data":{"acc_id":0,"status":200,"status_text":"OK","registered":true}}
-> {"id":"4","cmd":"call.dial","params":{"uri":"sip:100@example.com"}}
<- {"id":"4","ok":true,"result":{"call_id":0}}
<- {"event":"call_state","data":{"call_id":0,"state":"CALLING","last_status":0,"last_status_text":""}}
<- {"event":"call_state","data":{"call_id":0,"state":"CONNECTING","last_status":200,"last_status_text":"OK"}}
<- {"event":"call_media_state","data":{"call_id":0,"has_audio":true,"has_video":false}}
<- {"event":"call_state","data":{"call_id":0,"state":"CONFIRMED","last_status":200,"last_status_text":"OK"}}
-> {"id":"5","cmd":"call.hangup","params":{"call_id":0,"code":200}}
<- {"id":"5","ok":true,"result":{}}
<- {"event":"call_state","data":{"call_id":0,"state":"DISCONNECTED","last_status":200,"last_status_text":"Normal call clearing"}}
```

## Non-goals

Deliberately out of scope for v1 (mirrors `CONTEXT.md`) — do not add
commands/events for these without updating the design decision there
first:

- Multiple accounts, multiple concurrent calls, call hold/transfer
- Presence (buddy list, subscribe/notify) — instant messaging (SIP
  MESSAGE) is in scope, presence is not
- In-dialog typing indications (during an active call) — out-of-dialog
  `im.typing`/`typing` are in scope, see `pjsua_call_send_typing_ind`'s
  note under the `typing` event
- Multiple concurrent control connections
- Transport-layer authentication/encryption on the control socket itself
  (rely on filesystem/loopback trust boundary)

## Open questions

- [ ] Incoming-call ring timeout: should the daemon apply a default
      (e.g. auto-reject after N seconds) or is an unbounded ring
      acceptable given a single always-connected controller on tp4/tp7?
- [x] `call.answer` video default when the client doesn't specify
      `video`: resolved — mirrors the incoming offer (see the
      `call.answer` section above), not a fixed audio-only default.
- [ ] Does the control socket need any authentication beyond filesystem
      permissions, given tp4/tp7's threat model? Revisit before exposing
      the TCP transport option outside development builds.
- [ ] Reconnect behavior: if the controlling client drops mid-call, does
      the call continue and simply become uncontrollable until a client
      reconnects (current assumption), or should the daemon hang up
      calls when the control connection is lost?
