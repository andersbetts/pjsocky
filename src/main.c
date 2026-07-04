/*
 * pjsocky - headless SIP audio/video call daemon.
 *
 * Build-order step 12 from CONTEXT.md: robustness pass (malformed input
 * handling, second-connection refusal, write deadline, account.remove).
 * Accounts/devices/calls/IM (steps 6-11) came earlier.
 *
 * Configuration is via environment variables, not a config file or CLI
 * flags - a deliberate choice (see CONTEXT.md's "Decide config file
 * format"), not a stopgap: PJSOCKY_SOCK_PATH (control socket path,
 * default /tmp/pjsocky.sock), PJSOCKY_LOG_LEVEL (0-6, default follows
 * pjsua's own defaults) and PJSOCKY_WRITE_TIMEOUT_MSEC (control-socket
 * write deadline, see below).
 */
#include "account.h"
#include "call.h"
#include "im.h"
#include "proto/events.h"
#include "proto/server.h"

#include <pjsua-lib/pjsua.h>

#include <signal.h>
#include <stdlib.h>

#define THIS_FILE                   "main.c"
#define PJSOCKY_SOCK_PATH_ENV       "PJSOCKY_SOCK_PATH"
#define PJSOCKY_SOCK_PATH_DEFAULT   "/tmp/pjsocky.sock"

/*
 * Overrides pjsua_logging_config's level and console_level (both, same
 * value - pjsocky has no separate log file by default, so the
 * distinction between "logged" and "shown on console" doesn't apply
 * here) with an integer 0 (none) to 6 (trace). Unset uses pjsua's own
 * defaults (level=5, console_level=4). Config-via-env-vars is a
 * deliberate choice, not a stopgap awaiting a config file - see
 * CONTEXT.md's "Decide config file format" open question.
 */
#define PJSOCKY_LOG_LEVEL_ENV       "PJSOCKY_LOG_LEVEL"

/*
 * Write deadline (milliseconds) for the control connection - see
 * docs/PROTOCOL.md "Backpressure" and PJSOCKY_WRITE_TIMEOUT_DEFAULT_MSEC
 * in proto/server.h. A client that stops reading past this deadline is
 * declared dead and its connection dropped. Values <= 0 are ignored.
 */
#define PJSOCKY_WRITE_TIMEOUT_ENV   "PJSOCKY_WRITE_TIMEOUT_MSEC"

/*
 * Test-only: pjsip's default non-INVITE transaction timeout (RFC 3261
 * Timer F = 64*T1, T1 defaults to 500ms => 32s) means a REGISTER to an
 * address that doesn't produce a prompt transport-level error (e.g. a
 * closed UDP port that the sandbox doesn't deliver a fast ICMP
 * unreachable for) takes a genuine 32 seconds to fail - not a pjsocky
 * bug, just SIP's own retransmission timing. tests/protocol sets this
 * env var to shrink T1 so that scenario doesn't make the suite slow.
 * Never set outside tests - this changes real SIP retransmission
 * timing, which matters for real registrations/calls.
 */
#define PJSOCKY_TEST_FAST_TIMERS_ENV   "PJSOCKY_TEST_FAST_TIMERS"

/*
 * Set once by main() before installing the signal handler below, and
 * read only from that handler - see on_shutdown_signal().
 */
static pjsocky_server_t *g_srv;

/*
 * SIGINT/SIGTERM handler. Must stay async-signal-safe: no PJ_LOG (may
 * lock/allocate), no pjsua_* calls. pjsocky_server_stop() itself is
 * documented safe to call from a signal handler (see server.h).
 */
static void on_shutdown_signal(int sig)
{
    PJ_UNUSED_ARG(sig);
    if (g_srv)
        pjsocky_server_stop(g_srv);
}

int main(void)
{
    pjsua_config ua_cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config media_cfg;
    pj_status_t status;
    pj_pool_t *pool;
    pjsocky_events_t *events;
    pjsocky_server_t *srv;

    status = pjsua_create();
    if (status != PJ_SUCCESS) {
        pjsua_perror(THIS_FILE, "Error creating pjsua", status);
        return 1;
    }

    pool = pjsua_pool_create("pjsocky", 1000, 1000);
    if (!pool) {
        PJ_LOG(1, (THIS_FILE, "Failed to allocate startup pool"));
        pjsua_destroy();
        return 1;
    }

    /*
     * Must exist before pjsua_start(): pjsua callbacks (on_reg_state2
     * etc.) reach proto/events.c through pjsocky_events_instance()
     * rather than a parameter, since pjsua's callback signatures have
     * no user_data slot to thread one through. See proto/events.h.
     */
    status = pjsocky_events_create(pool, &events);
    if (status != PJ_SUCCESS) {
        pjsua_perror(THIS_FILE, "Error creating event dispatcher", status);
        pjsua_destroy();
        return 1;
    }

    if (getenv(PJSOCKY_TEST_FAST_TIMERS_ENV)) {
        /*
         * Writing pjsip_cfg()->tsx.t1 directly does nothing on its own:
         * the transaction layer caches the actual timer values it uses
         * separately, at module init time, before this code even runs.
         * pjsip_tsx_set_timers() is the real, documented way to change
         * them at runtime.
         *
         * Getting this right took two tries:
         *  1. pjsip_cfg()->tsx.t1 = 100 directly - silently had no
         *     effect at all, registration still took the full default
         *     32s to time out.
         *  2. pjsip_tsx_set_timers(100, 0, 0, 0) - shrunk the
         *     retransmit *pacing* but not the overall give-up point:
         *     the 4th param (`td`, documented as "for INVITE") turns
         *     out to be what actually controls the cached
         *     timeout_timer_val used as the general transaction
         *     completion deadline (sip_transaction.c) - so with td=0
         *     that deadline stayed at the default 64*500ms=32s
         *     regardless of t1. Passing td explicitly fixes it.
         */
        pjsip_tsx_set_timers(100, 0, 0, 1000);
        PJ_LOG(2, (THIS_FILE, "%s set: SIP transaction timers shrunk for "
                   "testing, do not use in production",
                   PJSOCKY_TEST_FAST_TIMERS_ENV));
    }

    pjsua_config_default(&ua_cfg);
    pjsua_logging_config_default(&log_cfg);
    pjsua_media_config_default(&media_cfg);

    {
        const char *log_level_str = getenv(PJSOCKY_LOG_LEVEL_ENV);

        if (log_level_str) {
            unsigned long level = strtoul(log_level_str, NULL, 10);

            log_cfg.level = (unsigned)level;
            log_cfg.console_level = (unsigned)level;
        }
    }

    ua_cfg.cb.on_reg_state2 = &pjsocky_account_on_reg_state2;
    ua_cfg.cb.on_call_state = &pjsocky_call_on_call_state;
    ua_cfg.cb.on_call_media_state = &pjsocky_call_on_call_media_state;
    ua_cfg.cb.on_incoming_call = &pjsocky_call_on_incoming_call;
    ua_cfg.cb.on_pager2 = &pjsocky_im_on_pager2;
    ua_cfg.cb.on_pager_status2 = &pjsocky_im_on_pager_status2;
    ua_cfg.cb.on_typing2 = &pjsocky_im_on_typing2;

    status = pjsua_init(&ua_cfg, &log_cfg, &media_cfg);
    if (status != PJ_SUCCESS) {
        pjsua_perror(THIS_FILE, "Error initializing pjsua", status);
        pjsua_destroy();
        return 1;
    }

    /*
     * pjsua_acc_add() asserts on there being at least one SIP transport
     * (pjsua_var.tpdata[0]) - accounts are bound to a transport, not
     * just a URI. Port 0 = bind to any available port; which local
     * port/interface pjsocky should actually use is a TODO (see
     * CONTEXT.md - "Decide config file format").
     */
    {
        pjsua_transport_config tp_cfg;

        pjsua_transport_config_default(&tp_cfg);
        status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tp_cfg, NULL);
        if (status != PJ_SUCCESS) {
            pjsua_perror(THIS_FILE, "Error creating SIP UDP transport", status);
            pjsua_destroy();
            return 1;
        }
    }

    status = pjsua_start();
    if (status != PJ_SUCCESS) {
        pjsua_perror(THIS_FILE, "Error starting pjsua", status);
        pjsua_destroy();
        return 1;
    }

    PJ_LOG(3, (THIS_FILE, "pjsocky started idle, no accounts configured"));

    {
        const char *sock_path = getenv(PJSOCKY_SOCK_PATH_ENV);
        const char *timeout_str = getenv(PJSOCKY_WRITE_TIMEOUT_ENV);
        unsigned write_timeout_msec = PJSOCKY_WRITE_TIMEOUT_DEFAULT_MSEC;

        if (!sock_path)
            sock_path = PJSOCKY_SOCK_PATH_DEFAULT;

        if (timeout_str) {
            long parsed = atol(timeout_str);

            if (parsed > 0) {
                write_timeout_msec = (unsigned)parsed;
            } else {
                PJ_LOG(2, (THIS_FILE, "Ignoring invalid %s='%s'",
                           PJSOCKY_WRITE_TIMEOUT_ENV, timeout_str));
            }
        }

        status = pjsocky_server_create(pool, sock_path, write_timeout_msec,
                                        &srv);
    }
    if (status != PJ_SUCCESS) {
        pjsua_perror(THIS_FILE, "Error creating control socket", status);
        pjsua_destroy();
        return 1;
    }

    g_srv = srv;
    signal(SIGINT, &on_shutdown_signal);
    signal(SIGTERM, &on_shutdown_signal);

    status = pjsocky_server_run(srv);
    if (status != PJ_SUCCESS)
        pjsua_perror(THIS_FILE, "Control socket accept loop stopped", status);

    pjsocky_server_destroy(srv);
    pjsua_destroy();

    return 0;
}
