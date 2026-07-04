/*
 * Coordinates writes to the currently active control connection between
 * two threads: the main thread running proto/server.c's request/response
 * loop, and pjsua's own worker thread, which is where pjsua callbacks
 * (on_reg_state2 etc.) actually fire. Both must write to the same
 * socket, and concurrent unsynchronized writes from two threads could
 * interleave bytes on the wire and corrupt NDJSON framing - so both
 * paths are routed through this module's single lock instead of calling
 * proto/framing.c directly. See CONTEXT.md's "Event push thread-safety"
 * open question - this is that design.
 */
#ifndef PJSOCKY_EVENTS_H
#define PJSOCKY_EVENTS_H

#include <pj/pool.h>
#include <pjlib-util/json.h>

#include "framing.h"

PJ_BEGIN_DECL

typedef struct pjsocky_events pjsocky_events_t;

/*
 * Create the process-wide event/connection-write coordinator. Call once
 * during startup, before pjsua_start() - pjsua callbacks reach this
 * module via pjsocky_events_instance() rather than a parameter, since
 * pjsua's callback signatures have no user_data slot to thread one
 * through.
 */
pj_status_t pjsocky_events_create(pj_pool_t *pool, pjsocky_events_t **p_events);

/* The instance created by pjsocky_events_create(), or NULL if that
 * hasn't run yet. */
pjsocky_events_t *pjsocky_events_instance(void);

/*
 * Record the currently active control connection, or NULL when none is
 * connected. Called by proto/server.c right after accept() and again
 * (with NULL) right before it closes the connection socket. Blocks
 * until any write currently in flight (a response or a pushed event)
 * finishes, so the connection socket is never closed out from under an
 * in-progress write.
 */
void pjsocky_events_set_conn(pjsocky_events_t *events, pjsocky_framing_t *framing);

/*
 * Write a request/response envelope to the current connection.
 * proto/server.c uses this instead of calling
 * pjsocky_framing_write_json() directly, so response writes and async
 * event pushes always go through the same lock and never interleave on
 * the wire. Returns PJ_SUCCESS (nothing to do) if there is currently no
 * active connection.
 */
pj_status_t pjsocky_events_write_response(pjsocky_events_t *events,
                                           const pj_json_elem *response,
                                           char *scratch,
                                           pj_size_t scratch_size);

/*
 * Fills in the "data" object of an outgoing event. Runs synchronously
 * inside pjsocky_events_push(), on whatever thread called it - `pool` is
 * private to that one call.
 */
typedef void (*pjsocky_event_builder)(pj_pool_t *pool,
                                       pj_json_elem *data,
                                       void *user_data);

/*
 * Push an async {"event":name,"data":{...}} message to the currently
 * connected client, if any. Safe to call from any thread, including
 * from inside a pjsua callback running on pjsua's own worker thread. If
 * no client is connected, the event is silently dropped - see
 * docs/PROTOCOL.md ("Backpressure"): events are not guaranteed
 * delivery, a client resyncs via a *.get command instead.
 */
pj_status_t pjsocky_events_push(pjsocky_events_t *events,
                                 const char *event_name,
                                 pjsocky_event_builder builder,
                                 void *user_data);

PJ_END_DECL

#endif /* PJSOCKY_EVENTS_H */
