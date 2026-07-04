#include "account.h"

#include "proto/events.h"
#include "proto/jsonutil.h"

#include <pj/errno.h>
#include <pj/log.h>

#define THIS_FILE "account.c"

/* v1 supports exactly one account - see CONTEXT.md. */
static pjsua_acc_id g_acc_id = PJSUA_INVALID_ID;

/*
 * Tracked explicitly rather than derived from pjsua_acc_info at query
 * time: pjsua_acc_info.has_registration only means "has a reg_uri
 * configured" (true as soon as account.configure runs, static
 * thereafter) - it is NOT "is currently registered", despite the
 * tempting name. The SIP status code alone is also ambiguous (both a
 * successful REGISTER and a successful un-REGISTER get a 2xx), so this
 * is set from on_reg_state2 using pjsua_reg_info.renew (register vs.
 * unregister direction) together with the resulting status.
 */
static pj_bool_t g_registered = PJ_FALSE;

pjsua_acc_id pjsocky_account_get_id(void)
{
    return g_acc_id;
}

pj_bool_t pjsocky_account_is_registered(void)
{
    return g_registered;
}

pj_status_t pjsocky_account_configure(const pj_str_t *sip_uri,
                                       const pj_str_t *registrar_uri,
                                       const pj_str_t *username,
                                       const pj_str_t *password,
                                       const pj_str_t *realm,
                                       pjsua_acc_id *p_acc_id)
{
    pjsua_acc_config acc_cfg;
    pj_status_t status;

    if (g_acc_id != PJSUA_INVALID_ID)
        return PJ_EEXISTS;

    g_registered = PJ_FALSE;

    pjsua_acc_config_default(&acc_cfg);
    acc_cfg.id = *sip_uri;
    acc_cfg.reg_uri = *registrar_uri;
    acc_cfg.register_on_acc_add = PJ_FALSE;
    acc_cfg.cred_count = 1;
    acc_cfg.cred_info[0].realm = *realm;
    acc_cfg.cred_info[0].scheme = pj_str("digest");
    acc_cfg.cred_info[0].username = *username;
    acc_cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc_cfg.cred_info[0].data = *password;

    /* pjsua_acc_add() duplicates all of acc_cfg's string fields into the
     * account's own pool before returning (pjsua_acc_config_dup()), so
     * sip_uri/registrar_uri/username/password/realm don't need to
     * outlive this call. */
    status = pjsua_acc_add(&acc_cfg, PJ_TRUE, &g_acc_id);
    if (status != PJ_SUCCESS)
        return status;

    *p_acc_id = g_acc_id;
    return PJ_SUCCESS;
}

pj_status_t pjsocky_account_register(void)
{
    if (g_acc_id == PJSUA_INVALID_ID)
        return PJ_EINVALIDOP;

    return pjsua_acc_set_registration(g_acc_id, PJ_TRUE);
}

pj_status_t pjsocky_account_unregister(void)
{
    if (g_acc_id == PJSUA_INVALID_ID)
        return PJ_EINVALIDOP;

    return pjsua_acc_set_registration(g_acc_id, PJ_FALSE);
}

pj_status_t pjsocky_account_remove(void)
{
    pj_status_t status;

    if (g_acc_id == PJSUA_INVALID_ID)
        return PJ_EINVALIDOP;

    /* docs/PROTOCOL.md "account.remove": refuse while registered rather
     * than silently un-REGISTERing on the client's behalf - the client
     * must account.unregister first, so registration teardown is always
     * an explicit, observable step (reg_state event). g_registered only
     * covers a *successful* REGISTER; an attempt still in flight is
     * fine to delete - pjsua_acc_del cancels it. */
    if (g_registered)
        return PJ_EINVALIDOP;

    status = pjsua_acc_del(g_acc_id);
    if (status != PJ_SUCCESS)
        return status;

    g_acc_id = PJSUA_INVALID_ID;
    g_registered = PJ_FALSE;
    return PJ_SUCCESS;
}

static void build_reg_state_data(pj_pool_t *pool, pj_json_elem *data,
                                  void *user_data)
{
    const pjsua_acc_info *info = (const pjsua_acc_info*)user_data;

    pjsocky_json_add_number(pool, data, "acc_id", (float)info->id);
    pjsocky_json_add_number(pool, data, "status", (float)info->status);
    pjsocky_json_add_str(pool, data, "status_text", &info->status_text);
    /* g_registered, not info->has_registration - see the comment above
     * its declaration. Must already be up to date by the time this
     * runs: pjsocky_account_on_reg_state2() sets it before calling
     * pjsocky_events_push(), which invokes this builder synchronously. */
    pjsocky_json_add_bool(pool, data, "registered", pjsocky_account_is_registered());
}

void pjsocky_account_on_reg_state2(pjsua_acc_id acc_id, pjsua_reg_info *info)
{
    pjsua_acc_info acc_info;
    pj_status_t status;

    status = pjsua_acc_get_info(acc_id, &acc_info);
    if (status != PJ_SUCCESS) {
        PJ_PERROR(1, (THIS_FILE, status, "pjsua_acc_get_info() failed"));
        return;
    }

    g_registered = (pj_bool_t)(info->renew && acc_info.status / 100 == 2);

    pjsocky_events_push(pjsocky_events_instance(), "reg_state",
                         &build_reg_state_data, &acc_info);
}
