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

/*
 * The call whose video has already been given up on, so the warning and
 * the teardown happen exactly once - see give_up_on_video(). Cleared
 * when a call disconnects, so the next call gets to try video again
 * (the camera may be back).
 */
static pjsua_call_id g_video_failed_call_id = PJSUA_INVALID_ID;

/*
 * Watchdog for a video stream that is negotiated and "active" but not
 * actually producing anything - see video_watchdog_cb(). Armed while a
 * call has a sending video stream, disarmed with the call.
 */
#define PJSOCKY_VIDEO_WATCHDOG_MSEC     3000

static pj_timer_entry g_video_watchdog;
static pj_bool_t g_video_watchdog_scheduled = PJ_FALSE;
static unsigned g_video_tx_packets_seen;
static unsigned g_video_stall_ticks;

static void cancel_video_watchdog(void);

/*
 * Periodic media report, for the length of every established call.
 *
 * "No media throughput" is the single most common thing reported about this
 * daemon, and until now answering it meant either attaching to the control
 * socket - which the launching application already holds, so it cannot be done
 * while the product is running - or reading a full pjmedia trace afterwards
 * and inferring. Neither is available to the person actually on the call.
 *
 * So the counters are simply logged, once every few seconds, for both media
 * types and both directions. That turns "no media" into a specific question
 * with the answer already in the log: nothing sent, or nothing received, and
 * for which stream. Those have completely different causes - nothing sent is a
 * capture or encoder fault on this device, nothing received is the far end or
 * the path to it - and telling them apart is most of the diagnosis.
 *
 * Cheap enough to leave on: two pjsua_call_get_stream_stat() calls and one log
 * line per interval, against a device that is streaming RTP at 50 packets a
 * second while it happens.
 */
#define PJSOCKY_MEDIA_REPORT_MSEC       5000

static pj_timer_entry g_media_report;
static pj_bool_t g_media_report_scheduled = PJ_FALSE;

static void cancel_media_report(void);

/* pjsua's own names for pjsua_call_media_status, for the log. A stream that is
   present but not ACTIVE is the interesting case - "no video" and "video the
   far end put on hold" and "video whose transport failed" all look identical
   from a packet counter, and they have nothing in common. */
static const char *media_status_str(pjsua_call_media_status st)
{
    switch (st) {
    case PJSUA_CALL_MEDIA_NONE:         return "none";
    case PJSUA_CALL_MEDIA_ACTIVE:       return "active";
    case PJSUA_CALL_MEDIA_LOCAL_HOLD:   return "local-hold";
    case PJSUA_CALL_MEDIA_REMOTE_HOLD:  return "remote-hold";
    case PJSUA_CALL_MEDIA_ERROR:        return "error";
    default:                            return "?";
    }
}

static const char *dir_str(pjmedia_dir dir)
{
    switch (dir) {
    case PJMEDIA_DIR_NONE:           return "inactive";
    case PJMEDIA_DIR_ENCODING:       return "sendonly";
    case PJMEDIA_DIR_DECODING:       return "recvonly";
    case PJMEDIA_DIR_ENCODING_DECODING: return "sendrecv";
    default:                         return "?";
    }
}

/*
 * What the last offer/answer actually produced, one line per m-line.
 *
 * Logged from on_call_media_state, which is where a negotiation result lands -
 * including the one from a mid-call re-INVITE, which is how a far end that
 * answered with its camera off later turns it on. That case was guesswork
 * before: the only evidence was has_video going false, which says a video
 * stream is not active without saying whether one was offered, what was agreed,
 * or which side declined.
 *
 * The payload types are printed separately for each direction on purpose.
 * Dynamic payload types (96-127) are negotiated per-direction - RFC 3264 6.1,
 * each side's numbers say what *it* expects to receive - so tx_pt != rx_pt is
 * normal and legal, not a fault. Seeing both spelled out is what stops that
 * being re-investigated as a mismatch every time it appears in a trace.
 */
static void log_negotiated_media(pjsua_call_id call_id,
                                  const pjsua_call_info *info)
{
    unsigned i;

    for (i = 0; i < info->media_cnt; i++) {
        const char *type = (info->media[i].type == PJMEDIA_TYPE_AUDIO) ? "audio" :
                           (info->media[i].type == PJMEDIA_TYPE_VIDEO) ? "video" : "other";
        pjsua_stream_info si;

        if (info->media[i].status != PJSUA_CALL_MEDIA_ACTIVE ||
            pjsua_call_get_stream_info(call_id, i, &si) != PJ_SUCCESS)
        {
            PJ_LOG(3, (THIS_FILE, "call %d media[%u]: %s %s dir=%s",
                       call_id, i, type,
                       media_status_str(info->media[i].status),
                       dir_str(info->media[i].dir)));
            continue;
        }

        if (si.type == PJMEDIA_TYPE_VIDEO) {
            PJ_LOG(3, (THIS_FILE,
                       "call %d media[%u]: video active dir=%s codec=%.*s "
                       "tx_pt=%u rx_pt=%u",
                       call_id, i, dir_str(info->media[i].dir),
                       (int)si.info.vid.codec_info.encoding_name.slen,
                       si.info.vid.codec_info.encoding_name.ptr,
                       si.info.vid.tx_pt, si.info.vid.rx_pt));
        } else {
            PJ_LOG(3, (THIS_FILE,
                       "call %d media[%u]: audio active dir=%s codec=%.*s "
                       "tx_pt=%u rx_pt=%u",
                       call_id, i, dir_str(info->media[i].dir),
                       (int)si.info.aud.fmt.encoding_name.slen,
                       si.info.aud.fmt.encoding_name.ptr,
                       si.info.aud.tx_pt, si.info.aud.rx_pt));
        }
    }

    /* Said explicitly rather than left to be inferred from an absent line: a
       remote that offered no video at all and a remote whose video we failed to
       bring up are different problems with the same silence. */
    PJ_LOG(3, (THIS_FILE, "call %d negotiated: %u m-line(s), remote offered "
               "%u audio / %u video",
               call_id, info->media_cnt, info->rem_aud_cnt, info->rem_vid_cnt));
}

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

    /* pjsua_call_setting_default() sets txt_cnt = 1, so the library default puts a T.140
     * real-time text stream in every offer. The pjsua demo app never ships that: it keeps
     * its own app_config.txt_cnt (zero unless --text is passed) and overwrites the library
     * default on every call and answer. pjsocky, being a from-scratch pjsua-lib
     * application, took the default and offered
     *     m=text 4004 RTP/AVP 100 98
     *     a=rtpmap:100 red/1000
     *     a=rtpmap:98 t140/1000
     * on every call. Nothing in this system wants it - the far end answers it with port 0 -
     * and an unexpected third m-line with a 1000Hz clock rate is exactly the kind of thing a
     * fragile endpoint mishandles. Match pjsua and leave it off; set this to 1 (and add a
     * config knob, like pjsua's --text) if real-time text is ever actually wanted.
     */
    setting.txt_cnt = 0;
    if (!video) {
        setting.vid_cnt = 0;
    }

    /* Video is offered sendrecv, exactly as pjsua does. This used to be forced to
     * send-only (PJSUA_CALL_SET_MEDIA_DIR + media_dir[1] = PJMEDIA_DIR_ENCODING)
     * because this hardware has no video render device and pjsua would fail an
     * incoming video stream with PJMEDIA_EVID_NODEFDEV. That is already solved,
     * and better, by the null render device registered in main.c
     * (null_video_dev.c provides exactly one PJMEDIA_DIR_RENDER device), so the
     * direction override was doing the same job twice.
     *
     * It was not harmless: "a=sendonly" on the video m-line was the only thing
     * left distinguishing pjsocky's INVITE from pjsua's - 24 of 25 SDP lines were
     * byte-identical - and a remote endpoint that cannot cope with a receive-only
     * video offer sees it while ringing, before anyone answers.
     */

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

pj_status_t pjsocky_call_get_rtp_counters(pjsua_call_id call_id,
                                           pjmedia_type type,
                                           unsigned *p_tx_packets,
                                           unsigned *p_rx_packets)
{
    pjsua_call_info info;
    pjsua_stream_stat stat;
    pj_status_t status;
    unsigned i;

    status = pjsocky_call_get_info(call_id, &info);
    if (status != PJ_SUCCESS)
        return status;

    for (i = 0; i < info.media_cnt; i++) {
        if (info.media[i].type != type ||
            info.media[i].status != PJSUA_CALL_MEDIA_ACTIVE)
            continue;

        status = pjsua_call_get_stream_stat(call_id, i, &stat);
        if (status != PJ_SUCCESS)
            return status;

        *p_tx_packets = stat.rtcp.tx.pkt;
        *p_rx_packets = stat.rtcp.rx.pkt;
        return PJ_SUCCESS;
    }

    return PJ_ENOTFOUND;
}

pj_status_t pjsocky_call_answer(pjsua_call_id call_id, unsigned code,
                                 pj_bool_t video)
{
    pjsua_call_setting setting;

    if (!is_valid_call_id(call_id))
        return PJ_EINVAL;

    pjsua_call_setting_default(&setting);

    /* No text stream on answers either - see pjsocky_call_dial(). */
    setting.txt_cnt = 0;

    if (!video) {
        setting.vid_cnt = 0;
    }

    /* Answers are sendrecv too - see pjsocky_call_dial(). */

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
    if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
        if (call_id == g_call_id)
            g_call_id = PJSUA_INVALID_ID;
        if (call_id == g_video_failed_call_id)
            g_video_failed_call_id = PJSUA_INVALID_ID;
        cancel_video_watchdog();
        cancel_media_report();
    }

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
/*
 * True if this call currently has a video stream that is supposed to be
 * sending: active (not held) and with the encoding direction on.
 * *p_med_idx receives its media index.
 */
static pj_bool_t find_sending_video(const pjsua_call_info *info,
                                     unsigned *p_med_idx)
{
    unsigned i;

    for (i = 0; i < info->media_cnt; i++) {
        if (info->media[i].type == PJMEDIA_TYPE_VIDEO &&
            info->media[i].status == PJSUA_CALL_MEDIA_ACTIVE &&
            (info->media[i].dir & PJMEDIA_DIR_ENCODING))
        {
            *p_med_idx = i;
            return PJ_TRUE;
        }
    }

    return PJ_FALSE;
}

static void cancel_video_watchdog(void)
{
    if (!g_video_watchdog_scheduled)
        return;
    pjsua_cancel_timer(&g_video_watchdog);
    g_video_watchdog_scheduled = PJ_FALSE;
}

/*
 * Drop this call's video, once, and let the call continue as an audio
 * call. `reason` is what to tell the log; it is logged exactly once per
 * call, no matter how many times the underlying fault reports itself.
 *
 * Removing the stream (a re-INVITE without the m=video line) rather than
 * just muting it does two things worth having: the far end stops waiting
 * for a picture that is not coming, and the local capture port is torn
 * down - which is what actually stops pjmedia retrying, and logging, at
 * frame rate. pjsua's on_call_media_state fires again afterwards, so the
 * controlling application hears about it the normal way: a
 * call_media_state event with has_video false.
 */
static void give_up_on_video(pjsua_call_id call_id, unsigned med_idx,
                              pj_status_t reason_status, const char *reason)
{
    pjsua_call_vid_strm_op_param param;
    pj_status_t status;

    if (call_id == g_video_failed_call_id)
        return; /* already done for this call - stay quiet */

    g_video_failed_call_id = call_id;
    cancel_video_watchdog();

    if (reason_status != PJ_SUCCESS) {
        PJ_PERROR(2, (THIS_FILE, reason_status,
                      "call %d: %s - continuing without video",
                      call_id, reason));
    } else {
        PJ_LOG(2, (THIS_FILE, "call %d: %s - continuing without video",
                   call_id, reason));
    }

    pjsua_call_vid_strm_op_param_default(&param);
    param.med_idx = (int)med_idx;

    status = pjsua_call_set_vid_strm(call_id, PJSUA_CALL_VID_STRM_REMOVE,
                                      &param);
    if (status != PJ_SUCCESS) {
        /* Nothing further to try; the call itself is unaffected. */
        PJ_PERROR(2, (THIS_FILE, status,
                      "call %d: could not remove the failed video stream",
                      call_id));
    }
}

/*
 * Not every video failure announces itself as an event. A camera that
 * disappears mid-call (USB unplugged, or a driver that stops delivering)
 * leaves the stream negotiated and PJSUA_CALL_MEDIA_ACTIVE, with pjmedia
 * retrying the capture port once per frame interval and logging a line
 * every time - "Failed to get frame from port N", forever, while the call
 * stays up and audio keeps working. Nothing in pjsua-lib times that out.
 *
 * So the transmitted packet count is polled instead, every three
 * seconds: a sending video stream that puts no RTP packet at all on the
 * wire for that long is not slow, it is dead - a live H.264 stream at any
 * usable frame rate sends something in three seconds, and a stream held
 * by the far end reports REMOTE_HOLD rather than ACTIVE, so it is not
 * counted here at all.
 *
 * A stream that has never sent anything gets two windows rather than one
 * before being written off. Those two cases are not the same: a stream
 * that was sending and stopped is unambiguous, while a stream still
 * coming up may genuinely need a moment for its first keyframe on a slow
 * encoder, and giving up on video that was about to work would be worse
 * than the delay.
 */
static void video_watchdog_cb(pj_timer_heap_t *th, pj_timer_entry *entry)
{
    pjsua_call_id call_id = (pjsua_call_id)(pj_ssize_t)entry->user_data;
    pjsua_call_info info;
    pj_time_val delay = { PJSOCKY_VIDEO_WATCHDOG_MSEC / 1000,
                          PJSOCKY_VIDEO_WATCHDOG_MSEC % 1000 };
    unsigned med_idx = 0;
    unsigned tx = 0, rx = 0;

    PJ_UNUSED_ARG(th);
    g_video_watchdog_scheduled = PJ_FALSE;

    if (pjsua_call_get_info(call_id, &info) != PJ_SUCCESS)
        return;
    if (info.state != PJSIP_INV_STATE_CONFIRMED)
        return;
    if (!find_sending_video(&info, &med_idx))
        return; /* no video to watch - nothing to re-arm for either */

    if (pjsocky_call_get_rtp_counters(call_id, PJMEDIA_TYPE_VIDEO,
                                       &tx, &rx) == PJ_SUCCESS &&
        tx > g_video_tx_packets_seen)
    {
        g_video_tx_packets_seen = tx;
        g_video_stall_ticks = 0;
    } else if (++g_video_stall_ticks >= (g_video_tx_packets_seen ? 1u : 2u)) {
        give_up_on_video(call_id, med_idx, PJ_SUCCESS,
                         "the video capture device stopped producing "
                         "frames");
        return;
    }

    if (pjsua_schedule_timer(&g_video_watchdog, &delay) == PJ_SUCCESS)
        g_video_watchdog_scheduled = PJ_TRUE;
}

static void cancel_media_report(void)
{
    if (!g_media_report_scheduled)
        return;
    pjsua_cancel_timer(&g_media_report);
    g_media_report_scheduled = PJ_FALSE;
}

/*
 * One line per interval, naming both streams and both directions. A media type
 * the call does not have is reported as "-" rather than as zeroes, because a
 * stream that is absent and a stream that is present and silent are different
 * faults and must not look alike in the log.
 */
static void media_report_cb(pj_timer_heap_t *th, pj_timer_entry *entry)
{
    pjsua_call_id call_id = (pjsua_call_id)(pj_ssize_t)entry->user_data;
    pjsua_call_info info;
    pj_time_val delay = { PJSOCKY_MEDIA_REPORT_MSEC / 1000,
                          PJSOCKY_MEDIA_REPORT_MSEC % 1000 };
    unsigned atx = 0, arx = 0, vtx = 0, vrx = 0;
    pj_bool_t have_audio, have_video;

    PJ_UNUSED_ARG(th);
    g_media_report_scheduled = PJ_FALSE;

    if (pjsua_call_get_info(call_id, &info) != PJ_SUCCESS)
        return;
    if (info.state != PJSIP_INV_STATE_CONFIRMED)
        return;

    have_audio = (pjsocky_call_get_rtp_counters(call_id, PJMEDIA_TYPE_AUDIO,
                                                &atx, &arx) == PJ_SUCCESS);
    have_video = (pjsocky_call_get_rtp_counters(call_id, PJMEDIA_TYPE_VIDEO,
                                                &vtx, &vrx) == PJ_SUCCESS);

    if (have_audio && have_video) {
        PJ_LOG(3, (THIS_FILE, "call %d media: audio tx=%u rx=%u  video tx=%u rx=%u",
                   call_id, atx, arx, vtx, vrx));
    } else if (have_audio) {
        PJ_LOG(3, (THIS_FILE, "call %d media: audio tx=%u rx=%u  video -",
                   call_id, atx, arx));
    } else if (have_video) {
        PJ_LOG(3, (THIS_FILE, "call %d media: audio -  video tx=%u rx=%u",
                   call_id, vtx, vrx));
    } else {
        PJ_LOG(3, (THIS_FILE, "call %d media: no active stream of either type",
                   call_id));
    }

    if (pjsua_schedule_timer(&g_media_report, &delay) == PJ_SUCCESS)
        g_media_report_scheduled = PJ_TRUE;
}

/* Arms the report above for a confirmed call. Re-armed rather than left running
 * across a media renegotiation, so that the timer always carries the current
 * call_id - a re-INVITE that adds video (a far end enabling its camera
 * mid-call) arrives here as another on_call_media_state. */
static void arm_media_report(pjsua_call_id call_id)
{
    pj_time_val delay = { PJSOCKY_MEDIA_REPORT_MSEC / 1000,
                          PJSOCKY_MEDIA_REPORT_MSEC % 1000 };

    cancel_media_report();

    pj_timer_entry_init(&g_media_report, 0, (void*)(pj_ssize_t)call_id,
                        &media_report_cb);
    if (pjsua_schedule_timer(&g_media_report, &delay) == PJ_SUCCESS)
        g_media_report_scheduled = PJ_TRUE;
}

/* Arms the watchdog above for a call that has a sending video stream, and
 * disarms it for one that no longer does. Called from on_call_media_state,
 * which is also where a removed video stream shows up. */
static void arm_video_watchdog(pjsua_call_id call_id,
                                const pjsua_call_info *info)
{
    unsigned med_idx = 0;
    pj_time_val delay = { PJSOCKY_VIDEO_WATCHDOG_MSEC / 1000,
                          PJSOCKY_VIDEO_WATCHDOG_MSEC % 1000 };

    cancel_video_watchdog();

    if (!find_sending_video(info, &med_idx) ||
        call_id == g_video_failed_call_id)
    {
        return;
    }

    g_video_tx_packets_seen = 0;
    g_video_stall_ticks = 0;

    pj_timer_entry_init(&g_video_watchdog, 0, (void*)(pj_ssize_t)call_id,
                        &video_watchdog_cb);
    if (pjsua_schedule_timer(&g_video_watchdog, &delay) == PJ_SUCCESS)
        g_video_watchdog_scheduled = PJ_TRUE;
}

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
    arm_video_watchdog(call_id, &info);
    arm_media_report(call_id);
    log_negotiated_media(call_id, &info);

    pjsocky_events_push(pjsocky_events_instance(), "call_media_state",
                         &build_call_media_state_data, &info);
}

/*
 * Accept video that the far end adds mid-call.
 *
 * pjsua answers an incoming offer according to call->opt, and vid_cnt in that
 * setting is fixed when the call is set up. A far end that answers our INVITE
 * with its camera off leaves the call with no video, and every later re-INVITE
 * offering video is then answered inactive - the m-line comes back, but as
 * status "none", direction "inactive", and nothing is ever sent or decoded.
 *
 * That is not a corner case, it is the normal way a video call starts here: the
 * operator answers first and turns the camera on a moment later. Measured on
 * the device before this existed:
 *
 *   call 0 media[1]: video none dir=inactive
 *   call 0 negotiated: 2 m-line(s), remote offered 1 audio / 1 video
 *
 * - the offer was there, and we declined it. on_call_rx_offer is the callback
 * pjsua provides for exactly this: `opt` is the setting the answer will be
 * built from, and raising vid_cnt here is what lets the answer carry video.
 *
 * Capped at one stream deliberately. This hardware has one camera and slightly
 * less than one Cortex-A7 to spare for encoding; a peer offering two video
 * m-lines gets one answered and the other declined, which is a better outcome
 * than trying to honour both.
 */
void pjsocky_call_on_call_rx_offer(pjsua_call_id call_id,
                                    const pjmedia_sdp_session *offer,
                                    void *reserved,
                                    pjsip_status_code *code,
                                    pjsua_call_setting *opt)
{
    unsigned i, offered_video = 0;

    PJ_UNUSED_ARG(reserved);
    PJ_UNUSED_ARG(code);

    if (!offer || !opt)
        return;

    for (i = 0; i < offer->media_count; i++) {
        const pjmedia_sdp_media *m = offer->media[i];

        /* Port 0 is how an offer withdraws a stream; counting it would put
         * vid_cnt back up on precisely the re-INVITE that is removing video. */
        if (pj_stricmp2(&m->desc.media, "video") == 0 && m->desc.port != 0)
            offered_video++;
    }

    /* No text stream, for the same reason dial() and answer() set txt_cnt = 0 -
     * see pjsocky_call_dial(). The setting handed to us here is the call's
     * current one, so without this a re-INVITE would be the one place a T.140
     * stream could still appear. */
    opt->txt_cnt = 0;

    if (offered_video > 0 && opt->vid_cnt == 0) {
        opt->vid_cnt = 1;
        PJ_LOG(3, (THIS_FILE, "call %d: far end added video mid-call, "
                   "accepting it", call_id));
    }
}

void pjsocky_call_on_call_media_event(pjsua_call_id call_id, unsigned med_idx,
                                       pjmedia_event *event)
{
    if (!event || event->type != PJMEDIA_EVENT_VID_DEV_ERROR)
        return;

    /* pjmedia reports a device failure per attempt, and an attempt happens
     * per frame, so one fault arrives as an unbounded stream of identical
     * events - each one also logged by pjsua-lib itself. give_up_on_video()
     * is a one-shot, so the second and later ones cost nothing.
     *
     * Safe to call pjsua_call_set_vid_strm() from here: pjsua-lib dispatches
     * on_call_media_event through a timer on its worker thread
     * (pjsua_media.c's call_med_event_cb), not from the media thread that
     * published the event. */
    give_up_on_video(call_id, med_idx, event->data.vid_dev_err.status,
                     "video device failed");
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
