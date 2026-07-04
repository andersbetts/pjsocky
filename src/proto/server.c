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
#include <fcntl.h>
#include <sys/select.h>
#include <sys/un.h>
#include <unistd.h>

#define THIS_FILE "server.c"

struct pjsocky_server
{
    pj_pool_factory     *pool_factory;
    pj_sock_t            listen_sock;
    struct sockaddr_un   addr;
    unsigned             write_timeout_msec;
    /* Written from a signal handler (pjsocky_server_stop()) and read
     * from pjsocky_server_run()'s loop; volatile so the compiler doesn't
     * cache the read across loop iterations. */
    volatile pj_bool_t   stop_requested;
};

/* The (single, per docs/PROTOCOL.md "Transport") active control
 * connection. Owned entirely by pjsocky_server_run()'s loop. */
typedef struct conn_state
{
    pj_bool_t            active;
    pj_sock_t            sock;
    pj_pool_t           *pool;
    pjsocky_framing_t    framing;
} conn_state;

pj_status_t pjsocky_server_create(pj_pool_t *pool,
                                   const char *path,
                                   unsigned write_timeout_msec,
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
    srv->write_timeout_msec = write_timeout_msec;

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

static pj_status_t set_nonblocking(pj_sock_t sock)
{
    int flags = fcntl((int)sock, F_GETFL, 0);

    if (flags < 0 || fcntl((int)sock, F_SETFL, flags | O_NONBLOCK) < 0)
        return pj_get_os_error();
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

/* Builder for the "error" event (docs/PROTOCOL.md "Events" - `error`):
 * daemon-side problems with no request id to correlate a response to.
 * `code`/`message` must outlive the push (string literals only). */
struct error_event_ctx
{
    const char *code;
    const char *message;
};

static void build_error_event_data(pj_pool_t *pool, pj_json_elem *data,
                                    void *user_data)
{
    const struct error_event_ctx *ctx =
        (const struct error_event_ctx*)user_data;

    pjsocky_json_add_string(pool, data, "code", ctx->code);
    pjsocky_json_add_string(pool, data, "message", ctx->message);
}

static void push_error_event(const char *code, const char *message)
{
    struct error_event_ctx ctx;

    ctx.code = code;
    ctx.message = message;
    pjsocky_events_push(pjsocky_events_instance(), "error",
                        &build_error_event_data, &ctx);
}

/*
 * docs/PROTOCOL.md "Transport": a second connection arriving while one
 * is active gets a single `error` event with code "connection_refused"
 * (deliberately no `hello` - its absence tells the client this
 * connection was never usable) and is closed. Written directly rather
 * than through proto/events.c, which is bound to the *active*
 * connection's socket.
 */
static void refuse_conn(pjsocky_server_t *srv, pj_sock_t sock)
{
    pj_pool_t *pool;

    PJ_LOG(3, (THIS_FILE,
               "Refusing concurrent control connection (one already active)"));

    pool = pj_pool_create(srv->pool_factory, "pjsocky-refuse", 1000, 1000,
                          NULL);
    if (pool) {
        pjsocky_framing_t framing;
        pj_json_elem event, data;
        pj_str_t name;
        struct error_event_ctx ctx;
        char scratch[512];

        set_nonblocking(sock);
        pjsocky_framing_init(&framing, sock, srv->write_timeout_msec);

        name = pj_str("");
        pj_json_elem_obj(&event, &name);
        pjsocky_json_add_string(pool, &event, "event", "error");

        name = pj_str("data");
        pj_json_elem_obj(&data, &name);
        ctx.code = "connection_refused";
        ctx.message = "another control connection is active";
        build_error_event_data(pool, &data, &ctx);
        pj_json_elem_add(&event, &data);

        /* Best effort - the refused peer may already be gone. */
        pjsocky_framing_write_json(&framing, &event, scratch, sizeof(scratch));
        pj_pool_release(pool);
    }

    pj_sock_close(sock);
}

static void open_conn(pjsocky_server_t *srv, conn_state *conn,
                      pj_sock_t sock)
{
    pj_status_t status;

    status = set_nonblocking(sock);
    if (status != PJ_SUCCESS) {
        PJ_PERROR(1, (THIS_FILE, status,
                      "Failed to make connection socket non-blocking"));
        pj_sock_close(sock);
        return;
    }

    conn->pool = pj_pool_create(srv->pool_factory, "pjsocky-conn", 2000,
                                2000, NULL);
    if (!conn->pool) {
        PJ_LOG(1, (THIS_FILE, "Failed to allocate connection pool"));
        pj_sock_close(sock);
        return;
    }

    conn->sock = sock;
    pjsocky_framing_init(&conn->framing, sock, srv->write_timeout_msec);
    pjsocky_events_set_conn(pjsocky_events_instance(), &conn->framing);

    /* docs/PROTOCOL.md "Versioning": sent once, immediately on connect,
     * before any response. pjsocky_events_push() writes synchronously
     * and only returns once this is on the wire, so nothing else can
     * race ahead of it. If the client vanished already, the next read
     * notices - no need to special-case a failed hello. */
    pjsocky_events_push(pjsocky_events_instance(), "hello",
                        &build_hello_data, NULL);

    conn->active = PJ_TRUE;
    PJ_LOG(4, (THIS_FILE, "Client connected"));
}

/*
 * docs/PROTOCOL.md "Robustness" ("Client disconnect"): only the control
 * connection dies here. All SIP state (registration, an active call)
 * survives untouched; events fired while no client is connected are
 * discarded by proto/events.c.
 */
static void close_conn(conn_state *conn)
{
    /* Blocks until any event push currently in flight finishes, so the
     * socket below is never closed out from under it. */
    pjsocky_events_set_conn(pjsocky_events_instance(), NULL);

    pj_pool_release(conn->pool);
    pj_sock_close(conn->sock);
    conn->active = PJ_FALSE;
}

/*
 * Dispatch one complete request line and write its response. Returns
 * PJ_SUCCESS to keep the connection, anything else to drop it.
 *
 * An unparseable line (or one with no usable "id") is NOT a
 * connection-fatal condition: docs/PROTOCOL.md "Robustness" - the
 * framing is line-based, so the next line is independent. Report it
 * with an `error` event and carry on.
 */
static pj_status_t handle_line(conn_state *conn, char *line,
                                pj_size_t line_len)
{
    pj_json_elem response;
    char scratch[PJSOCKY_MAX_LINE];
    pj_status_t status;

    status = pjsocky_dispatch_line(conn->pool, line, line_len, &response);
    if (status != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE,
                   "Unparseable request line - reporting via error event"));
        push_error_event("bad_request",
                         "not a JSON object with a string \"id\"");
        pj_pool_reset(conn->pool);
        return PJ_SUCCESS;
    }

    status = pjsocky_events_write_response(pjsocky_events_instance(),
                                            &response, scratch,
                                            sizeof(scratch));

    /* Bound per-connection memory growth across many requests on one
     * long-lived connection; everything allocated above is only needed
     * for the request/response we just finished. */
    pj_pool_reset(conn->pool);

    if (status != PJ_SUCCESS)
        PJ_PERROR(4, (THIS_FILE, status, "Socket write error"));
    return status;
}

/*
 * The connection socket selected readable: pull whatever is available
 * into the framing buffer and process every complete line in it.
 * Returns PJ_SUCCESS to keep the connection, anything else to drop it.
 */
static pj_status_t handle_conn_input(conn_state *conn)
{
    pj_status_t status = pjsocky_framing_fill(&conn->framing);

    if (status == PJ_EPENDING)
        return PJ_SUCCESS; /* spurious wakeup, nothing to read after all */
    if (status == PJ_EEOF) {
        PJ_LOG(4, (THIS_FILE, "Client disconnected"));
        return status;
    }
    if (status != PJ_SUCCESS) {
        PJ_PERROR(2, (THIS_FILE, status, "Socket read error"));
        return status;
    }

    for (;;) {
        char *line;
        pj_size_t line_len;

        status = pjsocky_framing_extract_line(&conn->framing, &line,
                                               &line_len);
        if (status == PJ_EPENDING)
            return PJ_SUCCESS; /* partial line - wait for more data */
        if (status == PJ_ETOOBIG) {
            /* docs/PROTOCOL.md "Robustness": framing can't be trusted
             * past an oversized line - error event, then drop. */
            PJ_LOG(2, (THIS_FILE,
                       "Request line too long, closing connection"));
            push_error_event("bad_request",
                             "line exceeds maximum length");
            return status;
        }

        status = handle_line(conn, line, line_len);
        if (status != PJ_SUCCESS)
            return status;
    }
}

/*
 * Poll interval for noticing stop_requested. pjsua_start() spawns its
 * own worker thread(s), and on Linux a process-directed signal (plain
 * `kill`) can be delivered to *any* thread that doesn't block it - not
 * necessarily this one, even though this is the thread blocked in
 * select(). Closing sockets from the signal handler to force a wakeup
 * is flaky on Linux in a multithreaded process (observed hanging on
 * shutdown roughly 1 in 5 tries in an earlier revision); polling with
 * select() and a bounded timeout sidesteps the problem entirely:
 * shutdown latency is bounded by this interval regardless of which
 * thread the signal lands on.
 */
#define PJSOCKY_ACCEPT_POLL_MSEC   500

pj_status_t pjsocky_server_run(pjsocky_server_t *srv)
{
    conn_state conn;

    PJ_ASSERT_RETURN(srv, PJ_EINVAL);

    pj_bzero(&conn, sizeof(conn));

    while (!srv->stop_requested) {
        fd_set rfds;
        struct timeval tv;
        int maxfd;
        int n;

        FD_ZERO(&rfds);
        FD_SET(srv->listen_sock, &rfds);
        maxfd = (int)srv->listen_sock;
        if (conn.active) {
            FD_SET(conn.sock, &rfds);
            if ((int)conn.sock > maxfd)
                maxfd = (int)conn.sock;
        }
        tv.tv_sec = 0;
        tv.tv_usec = PJSOCKY_ACCEPT_POLL_MSEC * 1000;

        n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            pj_status_t status = pj_get_os_error();

            if (status == PJ_STATUS_FROM_OS(EINTR))
                continue; /* interrupted by our own signal handler */
            PJ_PERROR(1, (THIS_FILE, status, "select() failed"));
            if (conn.active)
                close_conn(&conn);
            return status;
        }
        if (n == 0)
            continue; /* timed out - go recheck stop_requested */

        /* Existing connection first: if it closes in this same
         * iteration, a simultaneously arriving connection below gets
         * served instead of refused. */
        if (conn.active && FD_ISSET(conn.sock, &rfds)) {
            if (handle_conn_input(&conn) != PJ_SUCCESS)
                close_conn(&conn);
        }

        if (FD_ISSET(srv->listen_sock, &rfds)) {
            pj_sock_t new_sock;
            pj_status_t status = pj_sock_accept(srv->listen_sock, &new_sock,
                                                 NULL, NULL);
            if (status != PJ_SUCCESS) {
                PJ_PERROR(1, (THIS_FILE, status, "accept() failed"));
                if (conn.active)
                    close_conn(&conn);
                return status;
            }

            if (conn.active)
                refuse_conn(srv, new_sock);
            else
                open_conn(srv, &conn, new_sock);
        }
    }

    if (conn.active)
        close_conn(&conn);

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

    if (srv->listen_sock != PJ_INVALID_SOCKET)
        pj_sock_close(srv->listen_sock);

    unlink(srv->addr.sun_path);
}
