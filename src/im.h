/*
 * Wraps pjsua-lib's instant-messaging API (out-of-dialog SIP MESSAGE and
 * composing/typing indications) - see docs/PROTOCOL.md ("im.send",
 * "im.typing", "incoming_message", "message_status", "typing"). No
 * buddy list involved - see the conversation that settled this: SIP
 * MESSAGE send/receive only needs a URI, not a buddy handle.
 *
 * Deliberately has no dependency on JSON or the wire protocol - the
 * proto/dispatch.c command handlers translate between the two.
 */
#ifndef PJSOCKY_IM_H
#define PJSOCKY_IM_H

#include <pjsua-lib/pjsua.h>

PJ_BEGIN_DECL

/*
 * Wraps pjsua_im_send() against the current (v1 single) account.
 * Out-of-dialog SIP MESSAGE, not tied to any call. Returns
 * PJ_EINVALIDOP if the account isn't registered - see
 * docs/PROTOCOL.md "im.send". Asynchronous like account.register(): a
 * PJ_SUCCESS return only means the request was accepted for sending,
 * not that it was delivered - see pjsocky_im_on_pager_status2().
 */
pj_status_t pjsocky_im_send(const pj_str_t *to, const pj_str_t *content,
                             const pj_str_t *mime_type);

/*
 * Wraps pjsua_im_typing() against the current (v1 single) account.
 * Out-of-dialog composing indication (RFC 3994) - see docs/PROTOCOL.md
 * "im.typing". Returns PJ_EINVALIDOP if the account isn't registered.
 * Fire-and-forget: pjsua's own send path for this has no delivery-status
 * callback, unlike pjsua_im_send/on_pager_status2.
 */
pj_status_t pjsocky_im_typing(const pj_str_t *to, pj_bool_t is_typing);

/*
 * Callbacks to assign to pjsua_config.cb before pjsua_init(). Push
 * incoming_message/message_status/typing events (docs/PROTOCOL.md) via
 * proto/events.c.
 */
void pjsocky_im_on_pager2(pjsua_call_id call_id, const pj_str_t *from,
                          const pj_str_t *to, const pj_str_t *contact,
                          const pj_str_t *mime_type, const pj_str_t *body,
                          pjsip_rx_data *rdata, pjsua_acc_id acc_id);

void pjsocky_im_on_pager_status2(pjsua_call_id call_id, const pj_str_t *to,
                                 const pj_str_t *body, void *user_data,
                                 pjsip_status_code status,
                                 const pj_str_t *reason,
                                 pjsip_tx_data *tdata, pjsip_rx_data *rdata,
                                 pjsua_acc_id acc_id);

void pjsocky_im_on_typing2(pjsua_call_id call_id, const pj_str_t *from,
                           const pj_str_t *to, const pj_str_t *contact,
                           pj_bool_t is_typing, pjsip_rx_data *rdata,
                           pjsua_acc_id acc_id);

PJ_END_DECL

#endif /* PJSOCKY_IM_H */
