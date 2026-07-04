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

/* Default for `write_timeout_msec` below - docs/PROTOCOL.md
 * "Backpressure". Overridable at daemon startup via the
 * PJSOCKY_WRITE_TIMEOUT_MSEC env var (see main.c). */
#define PJSOCKY_WRITE_TIMEOUT_DEFAULT_MSEC  5000

/*
 * Create and bind the listening socket at `path`. `pool` is used only to
 * allocate the long-lived server object itself; per-connection state is
 * allocated from separate pools drawn from the same pool factory
 * (pool->factory), so the caller's pool does not grow with traffic.
 *
 * `write_timeout_msec` bounds every write to the control connection: a
 * client that stops reading past this deadline is declared dead and its
 * connection dropped (docs/PROTOCOL.md "Backpressure").
 */
pj_status_t pjsocky_server_create(pj_pool_t *pool,
                                   const char *path,
                                   unsigned write_timeout_msec,
                                   pjsocky_server_t **p_srv);

/*
 * Blocking select() loop: serves the single active control connection
 * (docs/PROTOCOL.md "Transport") and the listening socket together, so
 * a second connection attempt is refused immediately with a
 * "connection_refused" error event rather than waiting in the listen
 * backlog, and pjsocky_server_stop() is noticed within one poll
 * interval (PJSOCKY_ACCEPT_POLL_MSEC in server.c) even while a client
 * is connected. Runs until stopped or a fatal listening-socket error
 * occurs. Returns PJ_SUCCESS on a clean stop.
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
