# Contributing to pjsocky

## Ground rules

- **The protocol spec leads, the code follows.** docs/PROTOCOL.md is
  the source of truth for everything on the wire. Behavior lands there
  first, then in C — never the other way around. Protocol changes are
  public API changes: within a major protocol version they must be
  strictly additive (new optional fields, new commands, new events),
  and they belong in docs/CHANGELOG.md.
- **Keep it general-purpose.** pjsocky must make sense to anyone who
  wants a scriptable, headless SIP calling agent. Features shaped
  around one specific downstream consumer don't belong here — extend
  the generic protocol in a way any consumer would recognize, or keep
  the feature downstream.
- **No pjproject modifications.** pjsocky builds against an unmodified
  sibling pjproject checkout. If something there is missing or broken,
  work around it here (documented — see the OpenH264/CMake notes in
  CMakeLists.txt for the established pattern) or take it upstream.
- **v1 scope is deliberate.** One account, one call, one control
  connection. See "Non-goals" in docs/PROTOCOL.md and CONTEXT.md before
  proposing multi-anything.

## Building and testing

Build as described in README.md, then:

```sh
python3 tests/protocol/test_basic.py        # must pass, no infrastructure needed
python3 tests/asterisk/run_live_tests.py    # must pass if you touched call/media paths
```

Tests are plain python3 stdlib — no pytest, no packages. New commands
and events come **with tests**: the protocol suite grows alongside the
surface (this has caught real bugs that manual testing missed; see
CONTEXT.md's build-order notes for the receipts).

The build must stay warning-clean under `-Wall -Wextra`.

## Code shape

- C, matching the style already in src/ (pjproject-ish: 4-space indent,
  `pjsocky_` prefix, pj types and pools).
- Wrappers around pjsua-lib (account.c, call.c, device.c, im.c) stay
  JSON-free; JSON lives in src/proto/. Mind the pj_str_t lifetime
  warning in src/proto/jsonutil.h.
- Anything that takes a caller-supplied id must bounds-check it before
  calling into pjsua — some pjsua validation aborts the process instead
  of returning an error (see CONTEXT.md, step 8).

## License

GPLv2 (COPYING). By contributing you agree your changes are licensed
the same way.
