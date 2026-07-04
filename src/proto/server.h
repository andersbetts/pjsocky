/*
 * Unix domain socket server for the pjsocky control protocol
 * (docs/PROTOCOL.md - "Transport").
 */
#ifndef PJSOCKY_SERVER_H
#define PJSOCKY_SERVER_H

#include <pj/pool.h>
#include <pj/types.h>

PJ_BEGIN_DECL

typedef struct pjsocky_server pjsocky_server_t;

/*
 * Create and bind the listening socket at `path`. `pool` is used only to
 * allocate the long-lived server object itself; per-connection state is
 * allocated from separate pools drawn from the same pool factory
 * (pool->factory), so the caller's pool does not grow with traffic.
 */
pj_status_t pjsocky_server_create(pj_pool_t *pool,
                                   const char *path,
                                   pjsocky_server_t **p_srv);

/*
 * Blocking accept loop: serve one connection at a time to completion
 * (peer disconnect or a fatal per-connection error), then accept the
 * next, until pjsocky_server_stop() is called or a fatal listening-socket
 * error occurs. Returns PJ_SUCCESS on a clean stop.
 *
 * v1 scope limits:
 *  - A second connection attempt arriving while one is already being
 *    served simply waits in the kernel's listen() backlog until the
 *    first finishes - it is not actively refused with a
 *    "connection_refused" error yet (docs/PROTOCOL.md describes the
 *    target behavior; implementing it needs real concurrency - a second
 *    thread or a select()/poll() loop - out of scope for this skeleton).
 *  - pjsocky_server_stop() only affects a pending accept() wait (no
 *    client connected yet); it is noticed within one poll interval (see
 *    PJSOCKY_ACCEPT_POLL_MSEC in server.c). If a connection is currently
 *    being served, this loop won't return until that connection ends on
 *    its own (client disconnects, or a read/write error). See
 *    CONTEXT.md's "Robustness pass" TODO.
 */
pj_status_t pjsocky_server_run(pjsocky_server_t *srv);

/*
 * Ask the accept loop in pjsocky_server_run() to stop; noticed within
 * one poll interval. Safe to call from a POSIX signal handler
 * (SIGINT/SIGTERM) - a single flag write, no logging or allocation.
 * (An earlier version tried to unblock a plain blocking accept() by
 * closing the listening socket from the signal handler - that's flaky
 * on Linux in a multithreaded process, since a process-directed signal
 * isn't guaranteed to land on the thread actually blocked in accept();
 * see the comment above PJSOCKY_ACCEPT_POLL_MSEC in server.c.)
 */
void pjsocky_server_stop(pjsocky_server_t *srv);

void pjsocky_server_destroy(pjsocky_server_t *srv);

PJ_END_DECL

#endif /* PJSOCKY_SERVER_H */
