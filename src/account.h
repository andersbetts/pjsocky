/*
 * Wraps pjsua-lib's account API (pjsua_acc_add/del/set_registration) for
 * pjsocky's v1 single-account model - see CONTEXT.md ("One account at a
 * time in v1") and docs/PROTOCOL.md ("account.configure" etc).
 *
 * Deliberately has no dependency on JSON or the wire protocol - the
 * proto/dispatch.c command handlers translate between the two.
 */
#ifndef PJSOCKY_ACCOUNT_H
#define PJSOCKY_ACCOUNT_H

#include <pjsua-lib/pjsua.h>

PJ_BEGIN_DECL

/* The current account's ID, or PJSUA_INVALID_ID if none is configured. */
pjsua_acc_id pjsocky_account_get_id(void);

/*
 * Whether the last completed registration action was a successful
 * REGISTER (as opposed to: no account, never attempted, an
 * un-REGISTER, or a failed attempt). NOT the same as
 * pjsua_acc_info.has_registration, which just means "a reg_uri is
 * configured" - see the comment on this function's implementation.
 */
pj_bool_t pjsocky_account_is_registered(void);

/*
 * Add the (only, v1) account. Returns PJ_EEXISTS if an account is
 * already configured - see docs/PROTOCOL.md's "account.configure".
 * Does not register it (matches pjsua_acc_add's register_on_acc_add=FALSE).
 */
pj_status_t pjsocky_account_configure(const pj_str_t *sip_uri,
                                       const pj_str_t *registrar_uri,
                                       const pj_str_t *username,
                                       const pj_str_t *password,
                                       const pj_str_t *realm,
                                       pjsua_acc_id *p_acc_id);

/* Returns PJ_EINVALIDOP if no account is configured. */
pj_status_t pjsocky_account_register(void);
pj_status_t pjsocky_account_unregister(void);

/*
 * Callback to assign to pjsua_config.cb.on_reg_state2 before
 * pjsua_init(). Pushes a "reg_state" event (docs/PROTOCOL.md) via
 * proto/events.c.
 */
void pjsocky_account_on_reg_state2(pjsua_acc_id acc_id, pjsua_reg_info *info);

PJ_END_DECL

#endif /* PJSOCKY_ACCOUNT_H */
