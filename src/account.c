#include "account.h"

#include "device.h"
#include "proto/events.h"
#include "proto/jsonutil.h"

#include <pj/errno.h>
#include <pj/log.h>

#define THIS_FILE "account.c"

/* pjsua-lib resolves PJMEDIA_VID_DEFAULT_CAPTURE_DEV (-1, pjsua_acc_config's
 * own default for vid_cap_dev) at call setup time -- but on hardware with no
 * OS-level "default" video device (as here: three named devices, none
 * flagged default), that resolution fails outright with
 * PJMEDIA_EVID_NODEFDEV and pjsua-lib tears the whole video stream down
 * before call.c's apply_video_capture_device() ever gets a chance to pick a
 * real one. Always resolve to a concrete device up front so an account is
 * never left pointing at that unresolvable sentinel; device.set_video
 * (pjsocky_device_set_video_capture()) overrides this both here (if it ran
 * before account.configure) and live on an existing account (see its own
 * comment in device.c). */
static pjmedia_vid_dev_index resolve_default_vid_cap_dev(void)
{
    pjmedia_vid_dev_index cap_dev = pjsocky_device_get_video_capture();
    pjmedia_vid_dev_info devices[PJSOCKY_MAX_DEVICES];
    unsigned count = PJSOCKY_MAX_DEVICES;

    if (cap_dev != PJMEDIA_VID_INVALID_DEV)
        return cap_dev;

    if (pjsocky_device_list_video(devices, &count) == PJ_SUCCESS && count > 0)
        return devices[0].id;

    return PJMEDIA_VID_INVALID_DEV; /* no video device on this system at all */
}

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

    /* Both default to PJ_FALSE in pjsua-lib: without vid_out_auto_transmit,
     * pjsua_vid.c never connects the capture device to the outgoing
     * stream's encoder (that wiring -- setup_vid_capture()'s
     * pjsua_vid_conf_connect() -- is gated on this flag), so a call can
     * negotiate video and reach PJSUA_CALL_MEDIA_ACTIVE with nothing ever
     * actually transmitted, the same class of bug the audio side had
     * before pjsocky_call_on_call_media_state() started connecting the
     * audio conference ports itself. vid_in_auto_show is the RX-side
     * equivalent (show incoming video without the client having to ask). */
    acc_cfg.vid_in_auto_show = PJ_TRUE;
    acc_cfg.vid_out_auto_transmit = PJ_TRUE;
    acc_cfg.vid_cap_dev = resolve_default_vid_cap_dev();

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
