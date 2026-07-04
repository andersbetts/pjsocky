/*
 * Small helpers for building/reading pj_json_elem object trees. Shared
 * between proto/dispatch.c (request/response envelopes) and the pjsua
 * wrapper modules (account.c, and later call.c/im.c) that build event
 * payloads for proto/events.c to push.
 *
 * LIFETIME WARNING: none of the add_* helpers below copy string bytes.
 * pj_json_elem_string() (which they wrap) only copies the pj_str_t
 * struct - the pointer and length - not the characters it points to.
 * Whatever `val`/`name` point at must stay valid until the tree is
 * serialized (pj_json_write()), which usually happens later, in a
 * different function (e.g. proto/server.c writing a proto/dispatch.c
 * response after dispatch_line() has already returned). String literals
 * are always fine (static storage). A stack buffer built inside the
 * same function that calls one of these helpers is NOT fine unless the
 * tree is fully serialized before that function returns - allocate it
 * from `pool` instead. (This exact mistake happened once already: an
 * error message built in a local `char err_msg[N]` and passed through
 * pjsocky_json_add_string() produced garbage bytes on the wire once the
 * stack frame was reused - see dispatch.c's pjsocky_dispatch_line().)
 */
#ifndef PJSOCKY_JSONUTIL_H
#define PJSOCKY_JSONUTIL_H

#include <pj/pool.h>
#include <pjlib-util/json.h>

PJ_BEGIN_DECL

/* Find a direct child of a JSON object by name, or NULL if `obj` isn't
 * an object or has no such member. */
pj_json_elem *pjsocky_json_find(const pj_json_elem *obj, const char *name);

void pjsocky_json_add_bool(pj_pool_t *pool, pj_json_elem *obj,
                            const char *name, pj_bool_t val);

/* For null-terminated C string values (literals like "ok", "idle"). */
void pjsocky_json_add_string(pj_pool_t *pool, pj_json_elem *obj,
                              const char *name, const char *val);

/* For pj_str_t values that are not necessarily NUL-terminated (e.g.
 * anything read out of a pjsua_* struct) - does not call strlen() on
 * them. */
void pjsocky_json_add_str(pj_pool_t *pool, pj_json_elem *obj,
                           const char *name, const pj_str_t *val);

void pjsocky_json_add_number(pj_pool_t *pool, pj_json_elem *obj,
                              const char *name, float val);

PJ_END_DECL

#endif /* PJSOCKY_JSONUTIL_H */
