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
 * pjsua's own defaults), PJSOCKY_WRITE_TIMEOUT_MSEC (control-socket
 * write deadline, see below), PJSOCKY_VAD (silence detection, off by
 * default here) and PJSOCKY_VIDEO_SIZE/PJSOCKY_VIDEO_FPS (encoder
 * capture format - see the notes on each below).
 */
#include "account.h"
#include "call.h"
#include "device.h"
#include "im.h"
#include "proto/events.h"
#include "proto/server.h"
#include "proto/version.h"

#include <pjsua-lib/pjsua.h>

#include <signal.h>
#include <stdio.h>   /* setvbuf() - see main() */
#include <stdlib.h>
#include <string.h>  /* strtok_r() - configure_nameservers() */

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
 * Silence detection (VAD). Set to 1/on/yes/true to enable pjmedia's
 * silence detector; anything else, or unset, leaves it off.
 *
 * pjsocky inverts pjsua-lib's default (VAD on) deliberately, matching
 * pjsua's own demo app, which offers --no-vad for the same reason. With
 * VAD on, a microphone whose level sits near the detector's threshold
 * makes the daemon stop sending RTP mid-call and start again later:
 * on real hardware (a quiet electret on an emergency call box) that is
 * indistinguishable from a broken media path, it depends on the room
 * rather than on anything the operator controls, and pjsocky - being
 * headless - has nobody watching to notice audio came back. Continuous
 * RTP is also what makes the packet counters in call.get_info a usable
 * health signal. The bandwidth VAD saves is not worth any of that on a
 * single-call device, but the switch is here for whoever disagrees.
 */
#define PJSOCKY_VAD_ENV             "PJSOCKY_VAD"

/*
 * Video encoder capture format: PJSOCKY_VIDEO_SIZE as "WxH" (e.g.
 * "640x480") and PJSOCKY_VIDEO_FPS as a whole number of frames per
 * second. Unset leaves pjmedia's own per-codec defaults alone.
 *
 * These exist because the codec default and the camera are chosen
 * independently: pjmedia's H.264 default asks for 720x480, and a camera
 * that cannot produce it (or cannot produce it in a pixel format
 * pjmedia accepts) is opened at whatever it does support, after which
 * every single frame is colour-converted and rescaled before it reaches
 * the encoder. On a small ARM target that conversion is what stands
 * between the negotiated frame rate and the delivered one. Setting the
 * encoder to a size the camera produces natively removes the rescale
 * entirely - see device.list_video and the daemon log line from
 * vid_port.c that reports the format a device actually opened with.
 */
#define PJSOCKY_VIDEO_SIZE_ENV      "PJSOCKY_VIDEO_SIZE"
#define PJSOCKY_VIDEO_FPS_ENV       "PJSOCKY_VIDEO_FPS"

/*
 * STUN server ("host" or "host:port"), for a device behind NAT. Unset means no
 * STUN, which is the old behaviour and correct whenever the device's own
 * address is the one peers can reach.
 *
 * Without it, pjmedia builds the SDP's media address from pj_gethostip() - the
 * device's own interface address. SIP survives that, because the registrar
 * rewrites Via/Contact from `rport`/`received`, but nothing rewrites the `c=`
 * line inside the SDP body. So a NAT'd device sends a perfectly good INVITE
 * that says "send my media to 192.168.10.2", and the far end does exactly that,
 * into a private address it cannot route to.
 *
 * The failure is one-directional and silent, which is what makes it expensive:
 * signalling completes, the call is established, our transmit counter climbs
 * normally, and only the receive counter stays at zero forever. It looks
 * exactly like a far end that is not sending. Measured on TP4 against the
 * production PBX: `audio tx=497 rx=0`, with the answer naming a perfectly
 * reachable `c=IN IP4 217.174.89.69`.
 *
 * A far end doing symmetric RTP (Asterisk's `rtp_symmetric`, "comedia") hides
 * this by replying to wherever our packets actually came from, which is why the
 * same firmware works against one PBX and not another. This is the fix on our
 * side of that difference: with STUN, pjsua discovers the public address and
 * port and puts them in the SDP, so the far end is told the truth and does not
 * have to guess.
 */
#define PJSOCKY_STUN_SRV_ENV        "PJSOCKY_STUN_SRV"

/*
 * ICE, on top of STUN. 1/on/yes/true enables it; unset leaves it off.
 *
 * Kept separate from the STUN setting, and off by default, because the two fail
 * differently. STUN only changes what we advertise, so a STUN server that is
 * unreachable costs a timeout at startup and then behaves as if it were unset.
 * ICE changes the negotiation itself, and a far end that does not support it
 * has to be detected and fallen back from - which is worth having on a hostile
 * network and is not worth having by default on a device that a working STUN
 * address already fixes.
 */
#define PJSOCKY_ICE_ENV             "PJSOCKY_ICE"

/*
 * Nameservers for pjsip's own asynchronous DNS resolver, space- or
 * comma-separated. Unset means the list is read from PJSOCKY_RESOLV_CONF.
 *
 * Without a resolver of its own, pjsip resolves a SIP target with a blocking
 * getaddrinfo() from inside pjsua_call_make_call() - on the thread that also
 * serves the control socket. The dial reply, and every command queued behind
 * it, then waits for the system resolver, and that wait is unbounded from the
 * client's point of view: measured at 5 s on TP4, where the first nameserver
 * on the list never answers, and the client gave the dial up as failed while
 * the INVITE was still on its way out. Handing pjsip the nameservers makes
 * every resolution asynchronous, so call.dial returns at once and a target
 * that cannot be resolved ends the call with a 503 like any other failure.
 *
 * It also turns on SRV resolution for the SIP domain, which is what a
 * nameserver in pjsua_config means; a domain with no SRV record falls back
 * to its A record, so nothing changes for a PBX addressed by hostname.
 */
#define PJSOCKY_NAMESERVER_ENV      "PJSOCKY_NAMESERVER"
#define PJSOCKY_RESOLV_CONF         "/etc/resolv.conf"

/* Sanity bounds for the two above - not hardware limits, just far enough
 * out that anything beyond them is a typo rather than an intention. */
#define PJSOCKY_VIDEO_MAX_DIM       8192
#define PJSOCKY_VIDEO_MAX_FPS       240

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
 * Reads a boolean-ish environment variable: unset or unrecognised is
 * PJ_FALSE, "1"/"on"/"yes"/"true" (any case) is PJ_TRUE.
 */
static pj_bool_t env_flag_is_set(const char *name)
{
    const char *val = getenv(name);

    if (!val)
        return PJ_FALSE;

    return (pj_ansi_strcmp(val, "1") == 0 ||
            pj_ansi_stricmp(val, "on") == 0 ||
            pj_ansi_stricmp(val, "yes") == 0 ||
            pj_ansi_stricmp(val, "true") == 0);
}

/*
 * Fills ua_cfg->nameserver from PJSOCKY_NAMESERVER, or failing that from
 * the nameserver lines of PJSOCKY_RESOLV_CONF, up to pjsua's limit of four
 * in list order. The strings live in `pool`, which pjsua_init() then copies
 * from. Leaves the count at zero, with pjsip on the blocking system
 * resolver as before, if neither source names one - and says so, since a
 * device in that state dials slowly and there is nothing in the SIP log to
 * show why.
 */
static void configure_nameservers(pj_pool_t *pool, pjsua_config *ua_cfg)
{
    const char *env = getenv(PJSOCKY_NAMESERVER_ENV);
    const char *source = PJSOCKY_NAMESERVER_ENV;
    char buf[512];
    char *save = NULL;
    char *tok;
    unsigned i;

    ua_cfg->nameserver_count = 0;

    if (env && env[0]) {
        pj_ansi_snprintf(buf, sizeof(buf), "%s", env);
        for (tok = strtok_r(buf, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
            if (ua_cfg->nameserver_count >= PJ_ARRAY_SIZE(ua_cfg->nameserver))
                break;
            pj_strdup2_with_null(pool, &ua_cfg->nameserver[ua_cfg->nameserver_count++], tok);
        }
    } else {
        FILE *f = fopen(PJSOCKY_RESOLV_CONF, "r");

        source = PJSOCKY_RESOLV_CONF;
        if (f) {
            while (fgets(buf, sizeof(buf), f) &&
                   ua_cfg->nameserver_count < PJ_ARRAY_SIZE(ua_cfg->nameserver)) {
                tok = strtok_r(buf, " \t\r\n", &save);
                if (!tok || pj_ansi_strcmp(tok, "nameserver") != 0)
                    continue;
                tok = strtok_r(NULL, " \t\r\n", &save);
                if (tok)
                    pj_strdup2_with_null(pool, &ua_cfg->nameserver[ua_cfg->nameserver_count++], tok);
            }
            fclose(f);
        }
    }

    if (ua_cfg->nameserver_count == 0) {
        PJ_LOG(2, (THIS_FILE, "no nameserver from %s: SIP targets resolve "
                   "synchronously, so dialling blocks on the system resolver",
                   source));
        return;
    }

    for (i = 0; i < ua_cfg->nameserver_count; i++)
        PJ_LOG(3, (THIS_FILE, "nameserver %.*s (%s)",
                   (int)ua_cfg->nameserver[i].slen, ua_cfg->nameserver[i].ptr,
                   source));
}

/*
 * Applies PJSOCKY_VIDEO_SIZE/PJSOCKY_VIDEO_FPS to every registered video
 * codec. Must run after pjsua_init() (that is where codecs register) and
 * before any call is set up.
 *
 * A malformed or out-of-range value is logged and ignored rather than
 * being fatal: a daemon that refuses to start because of one bad
 * environment variable is a worse failure mode than one that runs with
 * the codec default and says so in the log.
 */
static void apply_video_format_override(void)
{
    const char *size_str = getenv(PJSOCKY_VIDEO_SIZE_ENV);
    const char *fps_str = getenv(PJSOCKY_VIDEO_FPS_ENV);
    pjsua_codec_info codecs[PJMEDIA_CODEC_MGR_MAX_CODECS];
    unsigned codec_cnt = PJ_ARRAY_SIZE(codecs);
    unsigned width = 0, height = 0, fps = 0;
    unsigned i;
    pj_status_t status;

    if (size_str) {
        /* Seeded with the input, not NULL: the *end checks below run even
         * when nothing was parsed at all. */
        char *end = (char *)size_str;
        unsigned long w, h;

        /* strtoul() happily accepts "-3" and wraps it into a huge unsigned,
         * so the sign has to be rejected before parsing, not after. */
        w = pj_isdigit(*size_str) ? strtoul(size_str, &end, 10) : 0;
        h = (w && (*end == 'x' || *end == 'X') && pj_isdigit(end[1]))
            ? strtoul(end + 1, &end, 10) : 0;

        if (w == 0 || h == 0 || *end != '\0' ||
            w > PJSOCKY_VIDEO_MAX_DIM || h > PJSOCKY_VIDEO_MAX_DIM)
        {
            PJ_LOG(1, (THIS_FILE, "Ignoring %s=\"%s\": expected WxH with "
                       "each side 1-%d, e.g. 640x480",
                       PJSOCKY_VIDEO_SIZE_ENV, size_str,
                       PJSOCKY_VIDEO_MAX_DIM));
        } else {
            width = (unsigned)w;
            height = (unsigned)h;
        }
    }

    if (fps_str) {
        char *end = (char *)fps_str;   /* see the size parse above */
        unsigned long f = pj_isdigit(*fps_str) ? strtoul(fps_str, &end, 10) : 0;

        if (f == 0 || *end != '\0' || f > PJSOCKY_VIDEO_MAX_FPS) {
            PJ_LOG(1, (THIS_FILE, "Ignoring %s=\"%s\": expected a whole "
                       "number of frames per second, 1-%d",
                       PJSOCKY_VIDEO_FPS_ENV, fps_str,
                       PJSOCKY_VIDEO_MAX_FPS));
        } else {
            fps = (unsigned)f;
        }
    }

    if (!width && !fps)
        return;

    status = pjsua_vid_enum_codecs(codecs, &codec_cnt);
    if (status != PJ_SUCCESS) {
        pjsua_perror(THIS_FILE, "Error enumerating video codecs", status);
        return;
    }

    for (i = 0; i < codec_cnt; i++) {
        pjmedia_vid_codec_param param;

        status = pjsua_vid_codec_get_param(&codecs[i].codec_id, &param);
        if (status != PJ_SUCCESS) {
            pjsua_perror(THIS_FILE, "Error reading video codec param", status);
            continue;
        }

        if (width) {
            param.enc_fmt.det.vid.size.w = width;
            param.enc_fmt.det.vid.size.h = height;
        }
        if (fps) {
            param.enc_fmt.det.vid.fps.num = fps;
            param.enc_fmt.det.vid.fps.denum = 1;
        }

        status = pjsua_vid_codec_set_param(&codecs[i].codec_id, &param);
        if (status != PJ_SUCCESS) {
            pjsua_perror(THIS_FILE, "Error setting video codec param", status);
            continue;
        }

        PJ_LOG(3, (THIS_FILE, "Video codec %.*s encodes at %dx%d @%d fps",
                   (int)codecs[i].codec_id.slen, codecs[i].codec_id.ptr,
                   (int)param.enc_fmt.det.vid.size.w,
                   (int)param.enc_fmt.det.vid.size.h,
                   (int)(param.enc_fmt.det.vid.fps.num /
                         param.enc_fmt.det.vid.fps.denum)));
    }
}

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

    /*
     * Line-buffer the log before anything writes to it.
     *
     * pjsocky's whole log is stdout, and on a device it is a pipe to the
     * launching application rather than a terminal - which means glibc picks
     * full buffering, not line buffering, and nothing reaches the reader until
     * 4KB has accumulated. On a quiet daemon that is not a small delay: an
     * observed run had lines arriving in the system journal one hour and
     * forty-four minutes after the events they described, in a burst, with
     * their own timestamps intact and the journal's wrong.
     *
     * That is worse than losing the lines. A log whose ordering against the
     * rest of the system is silently false invites conclusions drawn from a
     * sequence that never happened - and this is a daemon whose faults are
     * diagnosed almost entirely by reading its log next to somebody else's.
     *
     * _IOLBF costs a write() per line, which on a log this size is nothing
     * next to being able to trust it.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

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
    configure_nameservers(pool, &ua_cfg);
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

    /* pjsua_config_default() bzeroes user_agent, and pjsua_core.c only adds the
     * User-Agent header when it is non-empty - so taking the library default means
     * sending INVITEs and REGISTERs with no User-Agent at all. The pjsua demo app
     * always sets one (pjsua_app_config.c), and RFC 3261 makes the header optional,
     * but plenty of endpoints assume it is present when they build a call record or
     * a UI string, which is a poor thing to find out from someone else's crash.
     * Same shape as the txt_cnt default in call.c: a pjsua-lib default is not
     * pjsua-app behaviour.
     *
     * pj_get_sys_info() can contain slashes, which RFC 3261 does not allow here, so
     * they are replaced - mirroring the demo app.
     */
    {
        static char ua[128];
        unsigned i;

        pj_ansi_snprintf(ua, sizeof(ua), "pjsocky/%s (PJSUA/v%s %s)",
                         PJSOCKY_VERSION, pj_get_version(),
                         pj_get_sys_info()->info.ptr);
        for (i = 0; ua[i]; i++) {
            if (ua[i] == '/' && i > sizeof("pjsocky/") + 1)
                ua[i] = ' ';
        }
        ua_cfg.user_agent = pj_str(ua);
    }

    /* See PJSOCKY_VAD_ENV: the library default (VAD on) is inverted here,
     * and the env var is how to get it back. */
    media_cfg.no_vad = !env_flag_is_set(PJSOCKY_VAD_ENV);
    if (!media_cfg.no_vad)
        PJ_LOG(3, (THIS_FILE, "%s set: silence detection enabled",
                   PJSOCKY_VAD_ENV));

    /* Before pjsua_init(): pjsua resolves the STUN server and performs the
       binding request during init, and the address it learns is what every
       later media transport is created with. */
    {
        const char *stun = getenv(PJSOCKY_STUN_SRV_ENV);

        if (stun != NULL && stun[0] != '\0') {
            ua_cfg.stun_srv_cnt = 1;
            ua_cfg.stun_srv[0] = pj_str((char *)stun);

            /* Not fatal if it cannot be reached: a device that refuses to start
               because a STUN server is down is a worse failure than one that
               starts and advertises its local address, which is exactly what it
               would have done without this setting at all. */
            ua_cfg.stun_ignore_failure = PJ_TRUE;

            PJ_LOG(3, (THIS_FILE, "STUN server %s (%s)", stun,
                       PJSOCKY_STUN_SRV_ENV));
        }

        if (env_flag_is_set(PJSOCKY_ICE_ENV)) {
            media_cfg.enable_ice = PJ_TRUE;
            PJ_LOG(3, (THIS_FILE, "%s set: ICE enabled", PJSOCKY_ICE_ENV));
        }
    }

    ua_cfg.cb.on_reg_state2 = &pjsocky_account_on_reg_state2;
    ua_cfg.cb.on_call_state = &pjsocky_call_on_call_state;
    ua_cfg.cb.on_call_media_state = &pjsocky_call_on_call_media_state;
    ua_cfg.cb.on_call_media_event = &pjsocky_call_on_call_media_event;
    ua_cfg.cb.on_call_rx_offer = &pjsocky_call_on_call_rx_offer;
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

    /* See device.h/null_video_dev.c: this hardware has no real video
     * display, so pjsua's own video pipeline (incoming-video decode, and
     * even local capture preview for a send-only stream) would otherwise
     * fail with PJMEDIA_EVID_NODEFDEV the moment any call's video comes up. */
    status = pjmedia_vid_register_factory(&pjsocky_null_vid_factory, NULL);
    if (status != PJ_SUCCESS) {
        pjsua_perror(THIS_FILE, "Error registering null video render device", status);
        pjsua_destroy();
        return 1;
    }

    /* Codecs register during pjsua_init(), so any encoder-side override
     * has to land after it and before the first call - see
     * PJSOCKY_VIDEO_SIZE_ENV. */
    apply_video_format_override();

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

    /*
     * Headless fallback: on a box with zero audio devices (a container,
     * or a stripped-down target without ALSA devices) route call audio
     * through pjsua's built-in null sound device up front, instead of
     * failing to open a real device when the first call's media comes
     * up. When real devices exist they stay the default; device.list_audio
     * truthfully reports an empty list either way (docs/PROTOCOL.md
     * doesn't promise a non-empty device list).
     */
    if (pjmedia_aud_dev_count() == 0) {
        PJ_LOG(3, (THIS_FILE,
                   "No audio devices found - using the null sound device"));
        status = pjsua_set_null_snd_dev();
        if (status != PJ_SUCCESS)
            pjsua_perror(THIS_FILE, "Error setting null sound device", status);
    }

    /* Version first, and on its own line: the fourth component is the build
     * number (configure-time epoch - see CMakeLists.txt), so this is what says
     * whether the daemon running on a device is the one that was just built.
     * The controlling application sees the same string in the hello event, but
     * this line is here whether or not anything ever connects. */
    PJ_LOG(3, (THIS_FILE, "pjsocky %s (protocol %s, pjsip %s) starting",
               PJSOCKY_VERSION, PJSOCKY_PROTOCOL_VERSION, pj_get_version()));
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
