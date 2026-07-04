/*
 * Command name -> handler lookup for the request envelope described in
 * docs/PROTOCOL.md ("Message envelope", "Commands").
 */
#ifndef PJSOCKY_DISPATCH_H
#define PJSOCKY_DISPATCH_H

#include <pj/pool.h>
#include <pjlib-util/json.h>

PJ_BEGIN_DECL

/*
 * A command handler. `params` is the request's "params" object, or NULL
 * if the request had none. The handler fills `result` (already
 * initialized as an empty, unnamed JSON object by the caller) and
 * returns PJ_SUCCESS, or leaves `result` untouched and returns an error
 * status, which the caller maps to a docs/PROTOCOL.md error `code`.
 */
typedef pj_status_t (*pjsocky_cmd_fn)(pj_pool_t *pool,
                                       const pj_json_elem *params,
                                       pj_json_elem *result);

/*
 * Parse one NDJSON line as a request and build the corresponding
 * response object (see docs/PROTOCOL.md - "Response"), ready to be
 * written out with pj_json_write() and pjsocky_framing_write_line().
 *
 * Returns PJ_SUCCESS if `response` was filled in (this covers both
 * {"ok":true,...} and {"ok":false,...} responses - both are a
 * successfully handled request from the protocol's point of view).
 *
 * Returns a non-PJ_SUCCESS status only when the line could not be
 * correlated to any request `id` at all (e.g. it isn't valid JSON, or
 * isn't a JSON object) - the caller cannot send a meaningful response
 * and should close the connection. This is a deliberate v1 scope limit;
 * see CONTEXT.md's "Robustness pass" TODO.
 */
pj_status_t pjsocky_dispatch_line(pj_pool_t *pool,
                                   char *line,
                                   pj_size_t line_len,
                                   pj_json_elem *response);

PJ_END_DECL

#endif /* PJSOCKY_DISPATCH_H */
