/*
 * Wraps pjsua-lib's call API for pjsocky's v1 single-call model - see
 * CONTEXT.md and docs/PROTOCOL.md ("call.dial", "call.answer" etc).
 *
 * Deliberately has no dependency on JSON or the wire protocol - the
 * proto/dispatch.c command handlers translate between the two.
 */
#ifndef PJSOCKY_CALL_H
#define PJSOCKY_CALL_H

#include <pjsua-lib/pjsua.h>

PJ_BEGIN_DECL

/* The current call's ID, or PJSUA_INVALID_ID if none is active. Cleared
 * automatically when on_call_state observes PJSIP_INV_STATE_DISCONNECTED. */
pjsua_call_id pjsocky_call_get_id(void);

/*
 * Wraps pjsua_call_make_call() against the current (v1 single) account.
 * Returns PJ_EINVALIDOP if the account isn't registered - see
 * docs/PROTOCOL.md "call.dial".
 */
pj_status_t pjsocky_call_dial(const pj_str_t *uri, pj_bool_t video,
                               pjsua_call_id *p_call_id);

/* Wraps pjsua_call_hangup(). */
pj_status_t pjsocky_call_hangup(pjsua_call_id call_id, unsigned code);

/* Hangs up the current call if any; PJ_SUCCESS no-op if none - see
 * docs/PROTOCOL.md "call.hangup_all". */
pj_status_t pjsocky_call_hangup_all(void);

/* Wraps pjsua_call_get_info(). */
pj_status_t pjsocky_call_get_info(pjsua_call_id call_id, pjsua_call_info *info);

/*
 * Wraps pjsua_call_answer2(). `code`/`video` are already resolved by
 * the caller (docs/PROTOCOL.md "call.answer": code defaults to 200,
 * video defaults to whatever pjsocky_call_remote_offered_video() says -
 * this function itself has no opinion on defaults, it just answers with
 * exactly what it's given).
 */
pj_status_t pjsocky_call_answer(pjsua_call_id call_id, unsigned code,
                                 pj_bool_t video);

/*
 * RTP packet counters for the first active stream of `type` on this call,
 * straight out of pjsua_call_get_stream_stat().
 *
 * The point is the difference between "negotiated" and "flowing", which
 * nothing else here can tell you: a call reports media active, and
 * call_media_state says has_video, as soon as the stream object exists and
 * SDP agreed - whether or not a single frame is ever captured, encoded and
 * sent. When the far end reports no picture, tx_packets is the question
 * that splits the problem in half: still zero means nothing is being
 * produced on this side (capture or encoder), climbing means the bytes left
 * the box and the far end or the network owns the rest.
 *
 * Returns PJ_ENOTFOUND when the call has no active stream of that type.
 */
pj_status_t pjsocky_call_get_rtp_counters(pjsua_call_id call_id,
                                           pjmedia_type type,
                                           unsigned *p_tx_packets,
                                           unsigned *p_rx_packets);

/*
 * Whether the remote offered video on this call
 * (pjsua_call_info.rem_vid_cnt > 0). Used to resolve call.answer's
 * "video" default when the client doesn't specify one - see
 * docs/PROTOCOL.md "call.answer".
 */
pj_status_t pjsocky_call_remote_offered_video(pjsua_call_id call_id,
                                               pj_bool_t *p_has_video);

/*
 * Configure the incoming-call ring timeout in seconds. 0 (the default)
 * disables it - an incoming call rings until the client answers/hangs
 * up or the remote cancels, matching v1's original behavior before this
 * was made configurable. A nonzero value auto-rejects an unanswered
 * incoming call once it's been ringing that long (486 Busy Here would
 * be misleading here - the daemon uses 480 Temporarily Unavailable).
 * Applies to future incoming calls; does not affect one already
 * ringing. See docs/PROTOCOL.md's "config.set_ring_timeout".
 */
void pjsocky_call_set_ring_timeout(unsigned seconds);
unsigned pjsocky_call_get_ring_timeout(void);

/* String name of a pjsip_inv_state value, e.g. "CONFIRMED" - shared
 * between the call_state event builder and call.get_info's "state"
 * field so the mapping only lives in one place. */
const char *pjsocky_call_state_str(pjsip_inv_state state);

/*
 * Callbacks to assign to pjsua_config.cb before pjsua_init(). Push
 * call_state/call_media_state/incoming_call events (docs/PROTOCOL.md)
 * via proto/events.c. on_incoming_call does NOT auto-answer or
 * auto-reject - the client must call call.answer or call.hangup.
 */
void pjsocky_call_on_call_state(pjsua_call_id call_id, pjsip_event *e);
void pjsocky_call_on_call_media_state(pjsua_call_id call_id);
void pjsocky_call_on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                                    pjsip_rx_data *rdata);

/*
 * Incoming media offers (a re-INVITE from the far end). Raises vid_cnt so that
 * video the peer turns on mid-call is accepted rather than answered inactive -
 * see the implementation comment in call.c.
 */
void pjsocky_call_on_call_rx_offer(pjsua_call_id call_id,
                                    const pjmedia_sdp_session *offer,
                                    void *reserved,
                                    pjsip_status_code *code,
                                    pjsua_call_setting *opt);

/*
 * Video device failures during a call. Gives up on video once, quietly
 * and for good, instead of letting the failure repeat per frame - see
 * the implementation comment in call.c.
 */
void pjsocky_call_on_call_media_event(pjsua_call_id call_id, unsigned med_idx,
                                       pjmedia_event *event);

PJ_END_DECL

#endif /* PJSOCKY_CALL_H */
