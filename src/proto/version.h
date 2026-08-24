/*
 * What this daemon is, in the three numbers a client can act on
 * (docs/PROTOCOL.md - "Versioning", the `version.get` command and the
 * `hello` event).
 *
 * One place, because there are two ways to ask: the `hello` event
 * pushed on connect, and the `version.get` command. Both answer with the
 * same object, and they have to keep answering with the same object - a
 * client that learned the daemon version from `hello` and re-read it
 * later after a respawn must not get a differently-shaped answer. They
 * were one #define and one builder in server.c; the command made that a
 * second copy waiting to drift.
 */
#ifndef PJSOCKY_VERSION_H
#define PJSOCKY_VERSION_H

#include <pj/pool.h>
#include <pjlib-util/json.h>

PJ_BEGIN_DECL

/*
 * Semver against docs/PROTOCOL.md itself, not against PJSOCKY_VERSION
 * (the daemon's own release version, reported alongside it). Bumped from
 * "1.0.0-draft" once the v1 command surface was considered stable enough
 * to tag - see the note at the top of PROTOCOL.md. 1.1.0 added the
 * `version.get` command and the `pjsip_version` field, both additive.
 * Any future breaking change bumps to 2.x per the same note.
 */
#define PJSOCKY_PROTOCOL_VERSION "1.1.0"

/*
 * Add `protocol_version`, `daemon_version` and `pjsip_version` to `obj`.
 *
 * All three values are static storage (two compile-time strings and
 * pjlib's own version constant), which is what the jsonutil.h lifetime
 * warning requires: nothing here is copied, so nothing here may be a
 * stack buffer.
 */
void pjsocky_version_fill(pj_pool_t *pool, pj_json_elem *obj);

PJ_END_DECL

#endif /* PJSOCKY_VERSION_H */
