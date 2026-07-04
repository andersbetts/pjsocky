#include "server.h"
#include "dispatch.h"
#include "events.h"
#include "framing.h"
#include "jsonutil.h"

#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/log.h>
#include <pj/pool.h>
#include <pj/string.h>
#include <pjlib-util/json.h>

#include <errno.h>
#include <sys/select.h>
#include <sys/un.h>
#include <unistd.h>

#define THIS_FILE "server.c"

struct pjsocky_server
{
    pj_pool_factory     *pool_factory;
    pj_sock_t            listen_sock;
    struct sockaddr_un   addr;
    /* Written from a signal handler (pjsocky_server_stop()) and read
     * from pjsocky_server_run()'s loop; volatile so the compiler doesn't
     * cache the read across the accept() call. */
    volatile pj_bool_t   stop_requested;
};

pj_status_t pjsocky_server_create(pj_pool_t *pool,
                                   const char *path,
                                   pjsocky_server_t **p_srv)
{
    pjsocky_server_t *srv;
    pj_status_t status;
    pj_size_t path_len;

    PJ_ASSERT_RETURN(pool && path && p_srv, PJ_EINVAL);

    path_len = pj_ansi_strlen(path);

    srv = PJ_POOL_ZALLOC_T(pool, pjsocky_server_t);
    srv->pool_factory = pool->factory;
    srv->listen_sock = PJ_INVALID_SOCKET;

    if (path_len >= sizeof(srv->addr.sun_path))
        return PJ_ENAMETOOLONG;

    srv->addr.sun_family = (sa_family_t)pj_AF_UNIX();
    pj_memcpy(srv->addr.sun_path, path, path_len);
    srv->addr.sun_path[path_len] = '\0';

    status = pj_sock_socket(pj_AF_UNIX(), pj_SOCK_STREAM(), 0,
                             &srv->listen_sock);
    if (status != PJ_SUCCESS)
        return status;

    /* Remove a stale socket file left behind by a previous run. It is
     * fine if this fails because the path didn't exist. */
    unlink(srv->addr.sun_path);

    status = pj_sock_bind(srv->listen_sock, (pj_sockaddr_t*)&srv->addr,
                           sizeof(srv->addr));
    if (status != PJ_SUCCESS) {
        pj_sock_close(srv->listen_sock);
        return status;
    }

    status = pj_sock_listen(srv->listen_sock, 4);
    if (status != PJ_SUCCESS) {
        pj_sock_close(srv->listen_sock);
        unlink(srv->addr.sun_path);
        return status;
    }

    PJ_LOG(3, (THIS_FILE, "Listening on %s", srv->addr.sun_path));

    *p_srv = srv;
    return PJ_SUCCESS;
}

/*
 * docs/PROTOCOL.md "Versioning": follows semver against the protocol
 * document itself, not PJSOCKY_VERSION (the daemon's own release
 * version, reported separately below). Still "-draft" - see the note at
 * the top of PROTOCOL.md: bump this (and that note) together once the
 * v1 command surface is considered stable enough to tag.
 */
#define PJSOCKY_PROTOCOL_VERSION "1.0.0-draft"

static void build_hello_data(pj_pool_t *pool, pj_json_elem *data, void *user_data)
{
    PJ_UNUSED_ARG(user_data);

    pjsocky_json_add_string(pool, data, "protocol_version", PJSOCKY_PROTOCOL_VERSION);
    pjsocky_json_add_string(pool, data, "daemon_version", PJSOCKY_VERSION);
}

/*
 * Serve one accepted connection to completion (peer disconnect, a
 * framing/protocol-level error we can't recover from, or a socket
 * error). Uses its own pool, drawn from the server's pool factory, for
 * everything the connection allocates - released when this returns.
 *
 * Response writes go through proto/events.c (pjsocky_events_write_response)
 * rather than calling pjsocky_framing_write_json() directly here, so
 * that pjsua callbacks pushing async events from pjsua's own worker
 * thread (see account.c's on_reg_state2) can never interleave their
 * bytes with a response write on the same socket - both paths share one
 * lock. See proto/events.h.
 */
static void serve_conn(pj_pool_factory *pool_factory, pj_sock_t conn_sock)
{
    pj_pool_t *pool;
    pjsocky_framing_t framing;
    pjsocky_events_t *events = pjsocky_events_instance();

    pool = pj_pool_create(pool_factory, "pjsocky-conn", 2000, 2000, NULL);
    if (!pool) {
        PJ_LOG(1, (THIS_FILE, "Failed to allocate connection pool"));
        pj_sock_close(conn_sock);
        return;
    }

    pjsocky_framing_init(&framing, conn_sock);
    pjsocky_events_set_conn(events, &framing);

    /* docs/PROTOCOL.md "Versioning": sent once, immediately on connect,
     * before any response. pjsocky_events_push() writes synchronously
     * and only returns once this is on the wire, so nothing else can
     * race ahead of it (a response can't be sent before this loop even
     * starts). */
    pjsocky_events_push(events, "hello", &build_hello_data, NULL);

    for (;;) {
        char *line;
        pj_size_t line_len;
        pj_status_t status;
        pj_json_elem response;
        char scratch[PJSOCKY_MAX_LINE];

        status = pjsocky_framing_read_line(&framing, &line, &line_len);
        if (status == PJ_EEOF) {
            PJ_LOG(4, (THIS_FILE, "Client disconnected"));
            break;
        }
        if (status == PJ_ETOOBIG) {
            PJ_LOG(2, (THIS_FILE, "Request line too long, closing connection"));
            break;
        }
        if (status != PJ_SUCCESS) {
            PJ_PERROR(2, (THIS_FILE, status, "Socket read error"));
            break;
        }

        status = pjsocky_dispatch_line(pool, line, line_len, &response);
        if (status != PJ_SUCCESS) {
            PJ_LOG(2, (THIS_FILE,
                       "Request could not be parsed well enough to reply, "
                       "closing connection"));
            break;
        }

        status = pjsocky_events_write_response(events, &response,
                                                scratch, sizeof(scratch));
        if (status != PJ_SUCCESS) {
            PJ_PERROR(4, (THIS_FILE, status, "Socket write error"));
            break;
        }

        /* Bound per-connection memory growth across many requests on one
         * long-lived connection; everything allocated above is only
         * needed for the request/response we just finished. */
        pj_pool_reset(pool);
    }

    /* Blocks until any event push currently in flight finishes, so the
     * socket below is never closed out from under it. */
    pjsocky_events_set_conn(events, NULL);

    pj_pool_release(pool);
    pj_sock_close(conn_sock);
}

/*
 * Poll interval for noticing stop_requested while no client is
 * connecting. pjsua_start() spawns its own worker thread(s), and on
 * Linux a process-directed signal (plain `kill`) can be delivered to
 * *any* thread that doesn't block it - not necessarily this one, even
 * though this is the thread blocked in accept(). Closing listen_sock
 * from a different thread than the one blocked in accept() on it does
 * not reliably wake that thread on Linux (unlike BSD), so relying on
 * the signal handler to unblock a plain blocking accept() is flaky in
 * practice (observed hanging on shutdown roughly 1 in 5 tries). Polling
 * with select() and a bounded timeout sidesteps the problem entirely:
 * shutdown latency is bounded by this interval regardless of which
 * thread the signal lands on.
 */
#define PJSOCKY_ACCEPT_POLL_MSEC   500

pj_status_t pjsocky_server_run(pjsocky_server_t *srv)
{
    PJ_ASSERT_RETURN(srv, PJ_EINVAL);

    while (!srv->stop_requested) {
        fd_set rfds;
        struct timeval tv;
        int n;

        FD_ZERO(&rfds);
        FD_SET(srv->listen_sock, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = PJSOCKY_ACCEPT_POLL_MSEC * 1000;

        n = select((int)srv->listen_sock + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            pj_status_t status = pj_get_os_error();

            if (status == PJ_STATUS_FROM_OS(EINTR))
                continue; /* interrupted by our own signal handler */
            PJ_PERROR(1, (THIS_FILE, status, "select() failed"));
            return status;
        }
        if (n == 0)
            continue; /* timed out - go recheck stop_requested */

        {
            pj_sock_t conn_sock;
            pj_status_t status = pj_sock_accept(srv->listen_sock, &conn_sock,
                                                 NULL, NULL);
            if (status != PJ_SUCCESS) {
                PJ_PERROR(1, (THIS_FILE, status, "accept() failed"));
                return status;
            }

            PJ_LOG(4, (THIS_FILE, "Client connected"));
            serve_conn(srv->pool_factory, conn_sock);
        }
    }

    return PJ_SUCCESS;
}

void pjsocky_server_stop(pjsocky_server_t *srv)
{
    if (srv)
        srv->stop_requested = PJ_TRUE;
}

void pjsocky_server_destroy(pjsocky_server_t *srv)
{
    if (!srv)
        return;

    /* listen_sock may already be closed by pjsocky_server_stop(); only
     * close it here if that didn't happen. */
    if (srv->listen_sock != PJ_INVALID_SOCKET)
        pj_sock_close(srv->listen_sock);

    unlink(srv->addr.sun_path);
}
