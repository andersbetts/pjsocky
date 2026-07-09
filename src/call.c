#include "call.h"

#include "account.h"
#include "device.h"
#include "proto/events.h"
#include "proto/jsonutil.h"

#include <pj/errno.h>
#include <pj/log.h>

#define THIS_FILE "call.c"

/* v1 supports one active call at a time - see CONTEXT.md. */
static pjsua_call_id g_call_id = PJSUA_INVALID_ID;

/* 0 = disabled (default): unbounded ring, matching v1's original
 * behavior before this was made configurable - see
 * pjsocky_call_set_ring_timeout(). */
static unsigned g_ring_timeout_sec = 0;

/*
 * Armed only while an incoming call is ringing and g_ring_timeout_sec >
 * 0; disarmed as soon as that call leaves INCOMING/EARLY state
 * (answered or hung up) or the timer itself fires and auto-rejects it.
 * entry.user_data carries the call_id it was scheduled for, so
 * ring_timer_cb() can re-check the call is still the one it applies to -
 * pjsip's timer heap can already have dequeued a callback by the time a
 * cancel would run (e.g. the call gets answered in the same tick).
 */
static pj_timer_entry g_ring_timer;
static pj_bool_t g_ring_timer_scheduled = PJ_FALSE;

pjsua_call_id pjsocky_call_get_id(void)
{
    return g_call_id;
}

void pjsocky_call_set_ring_timeout(unsigned seconds)
{
    g_ring_timeout_sec = seconds;
}

unsigned pjsocky_call_get_ring_timeout(void)
{
    return g_ring_timeout_sec;
}

static void cancel_ring_timer(void)
{
    if (!g_ring_timer_scheduled)
        return;
    pjsua_cancel_timer(&g_ring_timer);
    g_ring_timer_scheduled = PJ_FALSE;
}

static void ring_timer_cb(pj_timer_heap_t *th, pj_timer_entry *entry)
{
    pjsua_call_id call_id = (pjsua_call_id)(pj_ssize_t)entry->user_data;
    pjsua_call_info info;

    PJ_UNUSED_ARG(th);
    g_ring_timer_scheduled = PJ_FALSE;

    if (pjsua_call_get_info(call_id, &info) != PJ_SUCCESS)
        return;
    if (info.state != PJSIP_INV_STATE_INCOMING &&
        info.state != PJSIP_INV_STATE_EARLY)
    {
        return;
    }

    PJ_LOG(3, (THIS_FILE, "call %d: ring timeout - auto-rejecting", call_id));
    pjsua_call_hangup(call_id, PJSIP_SC_TEMPORARILY_UNAVAILABLE, NULL, NULL);
}

/*
 * pjsua_call_hangup()/pjsua_call_get_info() range-check call_id with
 * PJ_ASSERT_RETURN(), which in this build actually aborts the process
 * on failure rather than just returning an error (assertions are
 * compiled in) - confirmed the hard way: a client sending an
 * out-of-range call_id (e.g. via call.hangup/call.get_info) crashed the
 * whole daemon. An in-range call_id that just isn't an active call is
 * handled by pjsua cleanly (a normal error return, not a crash) - it's
 * specifically the bounds check that's unsafe to delegate to pjsua, so
 * that's all this does.
 */
static pj_bool_t is_valid_call_id(pjsua_call_id call_id)
{
    return call_id >= 0 && call_id < (pjsua_call_id)pjsua_call_get_max_count();
}

pj_status_t pjsocky_call_dial(const pj_str_t *uri, pj_bool_t video,
                               pjsua_call_id *p_call_id)
{
    pjsua_acc_id acc_id = pjsocky_account_get_id();
    pjsua_call_setting setting;
    pj_status_t status;

    if (acc_id == PJSUA_INVALID_ID || !pjsocky_account_is_registered())
        return PJ_EINVALIDOP;

    pjsua_call_setting_default(&setting);
    if (!video) {
        setting.vid_cnt = 0;
    } else {
        /* This hardware has no video render device at all (see device.c /
         * account.c's vid_cap_dev comments -- device.list_video only ever
         * enumerates capture-direction devices). Negotiating the default
         * sendrecv leaves pjsua trying to create an RX render window when
         * the call's video stream comes up, which fails outright with
         * PJMEDIA_EVID_NODEFDEV and tears the whole video stream down --
         * because there is nothing to display incoming video *on* here,
         * unlike a phone/softphone UI. Restrict to send-only so pjsua never
         * needs a render device: media_dir[0] is audio, [1] is video, per
         * pjsua_call_setting_default()'s indexing. */
        setting.flag |= PJSUA_CALL_SET_MEDIA_DIR;
        setting.media_dir[1] = PJMEDIA_DIR_ENCODING;
    }

    /* pjsua_call_make_call() duplicates *uri into its own pool before
     * returning, same as pjsua_acc_add() - see account.c's comment on
     * pjsua_acc_config_dup(). */
    status = pjsua_call_make_call(acc_id, uri, &setting, NULL, NULL, &g_call_id);
    if (status != PJ_SUCCESS) {
        g_call_id = PJSUA_INVALID_ID;
        return status;
    }

    *p_call_id = g_call_id;
    return PJ_SUCCESS;
}

pj_status_t pjsocky_call_hangup(pjsua_call_id call_id, unsigned code)
{
    if (!is_valid_call_id(call_id))
        return PJ_EINVAL;

    return pjsua_call_hangup(call_id, code, NULL, NULL);
}

pj_status_t pjsocky_call_hangup_all(void)
{
    if (g_call_id == PJSUA_INVALID_ID)
        return PJ_SUCCESS;

    /* code=0: let pjsua pick BYE vs CANCEL vs a rejection response
     * based on the call's current state, rather than us guessing. */
    return pjsua_call_hangup(g_call_id, 0, NULL, NULL);
}

pj_status_t pjsocky_call_get_info(pjsua_call_id call_id, pjsua_call_info *info)
{
    if (!is_valid_call_id(call_id))
        return PJ_EINVAL;

    return pjsua_call_get_info(call_id, info);
}

pj_status_t pjsocky_call_remote_offered_video(pjsua_call_id call_id,
                                               pj_bool_t *p_has_video)
{
    pjsua_call_info info;
    pj_status_t status;

    status = pjsocky_call_get_info(call_id, &info);
    if (status != PJ_SUCCESS)
        return status;

    *p_has_video = (pj_bool_t)(info.rem_vid_cnt > 0);
    return PJ_SUCCESS;
}

pj_status_t pjsocky_call_answer(pjsua_call_id call_id, unsigned code,
                                 pj_bool_t video)
{
    pjsua_call_setting setting;

    if (!is_valid_call_id(call_id))
        return PJ_EINVAL;

    pjsua_call_setting_default(&setting);
    if (!video) {
        setting.vid_cnt = 0;
    } else {
        /* See the matching comment in pjsocky_call_dial(): no render device
         * exists on this hardware, so answer send-only too. */
        setting.flag |= PJSUA_CALL_SET_MEDIA_DIR;
        setting.media_dir[1] = PJMEDIA_DIR_ENCODING;
    }

    return pjsua_call_answer2(call_id, &setting, code, NULL, NULL);
}

const char *pjsocky_call_state_str(pjsip_inv_state state)
{
    switch (state) {
    case PJSIP_INV_STATE_NULL:         return "NULL";
    case PJSIP_INV_STATE_CALLING:      return "CALLING";
    case PJSIP_INV_STATE_INCOMING:     return "INCOMING";
    case PJSIP_INV_STATE_EARLY:        return "EARLY";
    case PJSIP_INV_STATE_CONNECTING:   return "CONNECTING";
    case PJSIP_INV_STATE_CONFIRMED:    return "CONFIRMED";
    case PJSIP_INV_STATE_DISCONNECTED: return "DISCONNECTED";
    default:                           return "UNKNOWN";
    }
}

static void build_call_state_data(pj_pool_t *pool, pj_json_elem *data,
                                   void *user_data)
{
    const pjsua_call_info *info = (const pjsua_call_info*)user_data;

    pjsocky_json_add_number(pool, data, "call_id", (float)info->id);
    pjsocky_json_add_string(pool, data, "state",
                             pjsocky_call_state_str(info->state));
    pjsocky_json_add_number(pool, data, "last_status", (float)info->last_status);
    pjsocky_json_add_str(pool, data, "last_status_text", &info->last_status_text);
}

void pjsocky_call_on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info info;
    pj_status_t status;

    PJ_UNUSED_ARG(e);

    status = pjsua_call_get_info(call_id, &info);
    if (status != PJ_SUCCESS) {
        PJ_PERROR(1, (THIS_FILE, status, "pjsua_call_get_info() failed"));
        return;
    }

    /* The ring timer only applies while its call is INCOMING/EARLY -
     * cancel it the moment that call moves on (answered or hung up),
     * regardless of what caused the transition. */
    if (g_ring_timer_scheduled &&
        call_id == (pjsua_call_id)(pj_ssize_t)g_ring_timer.user_data &&
        info.state != PJSIP_INV_STATE_INCOMING &&
        info.state != PJSIP_INV_STATE_EARLY)
    {
        cancel_ring_timer();
    }

    /* Terminal state for this call_id - see docs/PROTOCOL.md
     * "call_state": it may be reused by a later call after this. */
    if (info.state == PJSIP_INV_STATE_DISCONNECTED && call_id == g_call_id)
        g_call_id = PJSUA_INVALID_ID;

    pjsocky_events_push(pjsocky_events_instance(), "call_state",
                         &build_call_state_data, &info);
}

static void build_call_media_state_data(pj_pool_t *pool, pj_json_elem *data,
                                         void *user_data)
{
    const pjsua_call_info *info = (const pjsua_call_info*)user_data;
    pj_bool_t has_audio = PJ_FALSE, has_video = PJ_FALSE;
    unsigned i;

    for (i = 0; i < info->media_cnt; i++) {
        if (info->media[i].status != PJSUA_CALL_MEDIA_ACTIVE)
            continue;
        if (info->media[i].type == PJMEDIA_TYPE_AUDIO)
            has_audio = PJ_TRUE;
        else if (info->media[i].type == PJMEDIA_TYPE_VIDEO)
            has_video = PJ_TRUE;
    }

    pjsocky_json_add_number(pool, data, "call_id", (float)info->id);
    pjsocky_json_add_bool(pool, data, "has_audio", has_audio);
    pjsocky_json_add_bool(pool, data, "has_video", has_video);
}

/*
 * Apply the device.set_video capture device selection, if any, to this
 * call's video stream. Has to happen here (on_call_media_state), not at
 * dial()/answer() time: the video stream doesn't exist yet when the
 * call is set up (pjsua_call_setting.vid_cnt just requests that SDP
 * offer/answer negotiate one) - PJSUA_CALL_VID_STRM_CHANGE_CAP_DEV only
 * makes sense once the stream is actually active. See
 * docs/PROTOCOL.md's "device.set_video" and "Open questions" notes on
 * this having been deferred to call setup time.
 */
static void apply_video_capture_device(pjsua_call_id call_id,
                                        const pjsua_call_info *info)
{
    pjmedia_vid_dev_index cap_dev = pjsocky_device_get_video_capture();
    pj_bool_t has_active_video = PJ_FALSE;
    unsigned i;
    pjsua_call_vid_strm_op_param param;
    pj_status_t status;

    if (cap_dev == PJMEDIA_VID_INVALID_DEV)
        return; /* device.set_video was never called - nothing to apply */

    for (i = 0; i < info->media_cnt; i++) {
        if (info->media[i].type == PJMEDIA_TYPE_VIDEO &&
            info->media[i].status == PJSUA_CALL_MEDIA_ACTIVE)
        {
            has_active_video = PJ_TRUE;
            break;
        }
    }
    if (!has_active_video)
        return;

    pjsua_call_vid_strm_op_param_default(&param);
    param.cap_dev = cap_dev;

    status = pjsua_call_set_vid_strm(call_id, PJSUA_CALL_VID_STRM_CHANGE_CAP_DEV,
                                      &param);
    if (status != PJ_SUCCESS) {
        PJ_PERROR(1, (THIS_FILE, status,
                      "pjsua_call_set_vid_strm(CHANGE_CAP_DEV) failed"));
    }
}

/*
 * pjsua-lib never connects a call's conference-bridge audio port to the
 * sound device's port (slot 0) on its own -- that's application
 * responsibility. The pjsua CLI demo app does it in its own
 * on_call_media_state handling (pjsip-apps/src/pjsua/pjsua_app.c), but
 * pjsocky is a from-scratch pjsua-lib application, not derived from that
 * demo app, and never picked up this step. Without it, a call can reach
 * PJSUA_CALL_MEDIA_ACTIVE with real RTP flowing (has_audio:true in the
 * call_media_state event) while nothing actually reaches the speaker/mic:
 * the call's port and the sound device's port just sit unconnected in the
 * conference bridge. Connect them both ways, mirroring the demo app.
 * pjsua_conf_connect() is idempotent for an already-connected pair, so this
 * is safe to call again on media renegotiation.
 */
static void connect_call_audio_to_sound_dev(const pjsua_call_info *info)
{
    unsigned i;

    for (i = 0; i < info->media_cnt; i++) {
        pjsua_conf_port_id call_slot;
        pj_status_t status;

        if (info->media[i].type != PJMEDIA_TYPE_AUDIO ||
            info->media[i].status != PJSUA_CALL_MEDIA_ACTIVE)
            continue;

        call_slot = info->media[i].stream.aud.conf_slot;

        status = pjsua_conf_connect(call_slot, 0);
        if (status != PJ_SUCCESS)
            PJ_PERROR(1, (THIS_FILE, status,
                          "pjsua_conf_connect(call->sound_dev) failed"));

        status = pjsua_conf_connect(0, call_slot);
        if (status != PJ_SUCCESS)
            PJ_PERROR(1, (THIS_FILE, status,
                          "pjsua_conf_connect(sound_dev->call) failed"));
    }
}

void pjsocky_call_on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info info;
    pj_status_t status;

    status = pjsua_call_get_info(call_id, &info);
    if (status != PJ_SUCCESS) {
        PJ_PERROR(1, (THIS_FILE, status, "pjsua_call_get_info() failed"));
        return;
    }

    apply_video_capture_device(call_id, &info);
    connect_call_audio_to_sound_dev(&info);

    pjsocky_events_push(pjsocky_events_instance(), "call_media_state",
                         &build_call_media_state_data, &info);
}

static void build_incoming_call_data(pj_pool_t *pool, pj_json_elem *data,
                                      void *user_data)
{
    const pjsua_call_info *info = (const pjsua_call_info*)user_data;

    pjsocky_json_add_number(pool, data, "call_id", (float)info->id);
    pjsocky_json_add_number(pool, data, "acc_id", (float)info->acc_id);
    /* Same format as call.get_info's "remote_info" ("Display Name"
     * <sip:user@host>), not a bare URI - docs/PROTOCOL.md's original
     * example showed a bare URI for "from"; using remote_info directly
     * is simpler and consistent with get_info rather than writing
     * separate URI-extraction code for one field. */
    pjsocky_json_add_str(pool, data, "from", &info->remote_info);
    pjsocky_json_add_bool(pool, data, "has_video", info->rem_vid_cnt > 0);
}

void pjsocky_call_on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                                    pjsip_rx_data *rdata)
{
    pjsua_call_info info;
    pj_status_t status;

    PJ_UNUSED_ARG(acc_id);
    PJ_UNUSED_ARG(rdata);

    /*
     * v1 tracks one call at a time (CONTEXT.md), same as
     * pjsocky_call_dial() does for outgoing calls. A second incoming
     * call while one is already active is NOT rejected here - pjsua
     * itself allows up to PJSUA_MAX_CALLS concurrently, and this
     * callback still fires and still pushes an incoming_call event for
     * it. It's answerable/hangup-able by its own call_id regardless,
     * but it will clobber what g_call_id (and therefore
     * status.get/call.hangup_all) considers "the" current call. Not
     * defended against yet - see CONTEXT.md's robustness-pass TODO.
     */
    g_call_id = call_id;

    status = pjsua_call_get_info(call_id, &info);
    if (status != PJ_SUCCESS) {
        PJ_PERROR(1, (THIS_FILE, status, "pjsua_call_get_info() failed"));
        return;
    }

    if (g_ring_timeout_sec > 0) {
        pj_time_val delay;

        cancel_ring_timer(); /* defensive; v1's single-call model means
                                 there shouldn't be a stale one armed */
        pj_timer_entry_init(&g_ring_timer, 0, (void*)(pj_ssize_t)call_id,
                             &ring_timer_cb);
        delay.sec = (long)g_ring_timeout_sec;
        delay.msec = 0;
        if (pjsua_schedule_timer(&g_ring_timer, &delay) == PJ_SUCCESS)
            g_ring_timer_scheduled = PJ_TRUE;
        else
            PJ_LOG(1, (THIS_FILE, "failed to schedule ring timeout timer"));
    }

    pjsocky_events_push(pjsocky_events_instance(), "incoming_call",
                         &build_incoming_call_data, &info);
}
