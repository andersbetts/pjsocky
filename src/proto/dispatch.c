#include "dispatch.h"
#include "jsonutil.h"

#include "../account.h"
#include "../call.h"
#include "../device.h"
#include "../im.h"

#include <pj/errno.h>
#include <pj/string.h>

/*
 * pj_json_parse() does not copy string values out of the source buffer -
 * parsed pj_str_t values point directly into `line`. Everything in this
 * file must finish reading the request (including echoing its "id" into
 * the response) before the caller's framing buffer can be reused for the
 * next pjsocky_framing_read_line() call. The server loop in server.c
 * respects this by fully serializing the response (pj_json_write(), which
 * does copy) before reading again.
 */

typedef struct cmd_entry
{
    const char      *name;
    pjsocky_cmd_fn   fn;
} cmd_entry;

/* Required string param. Returns PJ_EINVAL if `params` is NULL, isn't an
 * object, or has no such member, or the member isn't a string. */
static pj_status_t get_string_member(const pj_json_elem *params,
                                      const char *name, pj_str_t *out)
{
    pj_json_elem *el = pjsocky_json_find(params, name);

    if (!el || el->type != PJ_JSON_VAL_STRING)
        return PJ_EINVAL;

    *out = el->value.str;
    return PJ_SUCCESS;
}

/* Required boolean param. */
static pj_status_t get_bool_member(const pj_json_elem *params,
                                    const char *name, pj_bool_t *out)
{
    pj_json_elem *el = pjsocky_json_find(params, name);

    if (!el || el->type != PJ_JSON_VAL_BOOL)
        return PJ_EINVAL;

    *out = el->value.is_true;
    return PJ_SUCCESS;
}

/* Required integer param. */
static pj_status_t get_int_member(const pj_json_elem *params,
                                   const char *name, int *out)
{
    pj_json_elem *el = pjsocky_json_find(params, name);

    if (!el || el->type != PJ_JSON_VAL_NUMBER)
        return PJ_EINVAL;

    *out = (int)el->value.num;
    return PJ_SUCCESS;
}

static pj_status_t cmd_ping(pj_pool_t *pool,
                             const pj_json_elem *params,
                             pj_json_elem *result)
{
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(params);
    PJ_UNUSED_ARG(result);
    return PJ_SUCCESS;
}

/*
 * "state" is a v1 approximation: there's no distinct "registration in
 * progress" bucket (docs/PROTOCOL.md only defines idle/registering/
 * registered/in_call), so a configured-but-never-registered account, an
 * explicitly-unregistered one, and a failed registration attempt all
 * report as "registering" here - only a confirmed successful REGISTER
 * (pjsocky_account_is_registered()) counts as "registered".
 */
static pj_status_t cmd_status_get(pj_pool_t *pool,
                                   const pj_json_elem *params,
                                   pj_json_elem *result)
{
    pjsua_acc_id acc_id = pjsocky_account_get_id();
    pjsua_call_id call_id = pjsocky_call_get_id();

    PJ_UNUSED_ARG(params);

    if (acc_id == PJSUA_INVALID_ID) {
        pjsocky_json_add_string(pool, result, "state", "idle");
        pjsocky_json_add_number(pool, result, "acc_id", -1);
        pjsocky_json_add_number(pool, result, "reg_status", 0);
    } else {
        pjsua_acc_info info;
        pj_status_t status = pjsua_acc_get_info(acc_id, &info);

        if (status != PJ_SUCCESS)
            return status;

        pjsocky_json_add_string(pool, result, "state",
            call_id != PJSUA_INVALID_ID ? "in_call" :
            (pjsocky_account_is_registered() ? "registered" : "registering"));
        pjsocky_json_add_number(pool, result, "acc_id", (float)acc_id);
        pjsocky_json_add_number(pool, result, "reg_status", (float)info.status);
    }

    pjsocky_json_add_number(pool, result, "call_id", (float)call_id);

    return PJ_SUCCESS;
}

static pj_status_t cmd_account_configure(pj_pool_t *pool,
                                          const pj_json_elem *params,
                                          pj_json_elem *result)
{
    pj_str_t sip_uri, registrar_uri, username, password, realm;
    pjsua_acc_id acc_id;
    pj_status_t status;

    if (get_string_member(params, "sip_uri", &sip_uri) != PJ_SUCCESS ||
        get_string_member(params, "registrar_uri", &registrar_uri) != PJ_SUCCESS ||
        get_string_member(params, "username", &username) != PJ_SUCCESS ||
        get_string_member(params, "password", &password) != PJ_SUCCESS)
    {
        return PJ_EINVAL;
    }

    if (get_string_member(params, "realm", &realm) != PJ_SUCCESS)
        realm = pj_str("*");

    status = pjsocky_account_configure(&sip_uri, &registrar_uri, &username,
                                        &password, &realm, &acc_id);
    if (status != PJ_SUCCESS)
        return status;

    pjsocky_json_add_number(pool, result, "acc_id", (float)acc_id);
    return PJ_SUCCESS;
}

static pj_status_t cmd_account_register(pj_pool_t *pool,
                                         const pj_json_elem *params,
                                         pj_json_elem *result)
{
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(params);
    PJ_UNUSED_ARG(result);
    return pjsocky_account_register();
}

static pj_status_t cmd_account_unregister(pj_pool_t *pool,
                                           const pj_json_elem *params,
                                           pj_json_elem *result)
{
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(params);
    PJ_UNUSED_ARG(result);
    return pjsocky_account_unregister();
}

static pj_status_t cmd_account_remove(pj_pool_t *pool,
                                       const pj_json_elem *params,
                                       pj_json_elem *result)
{
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(params);
    PJ_UNUSED_ARG(result);
    return pjsocky_account_remove();
}

static const char *vid_dir_str(pjmedia_dir dir)
{
    switch (dir) {
    case PJMEDIA_DIR_CAPTURE:          return "capture";
    case PJMEDIA_DIR_RENDER:           return "render";
    case PJMEDIA_DIR_CAPTURE_PLAYBACK: return "capture_render";
    default:                           return "none";
    }
}

static pj_status_t cmd_device_list_audio(pj_pool_t *pool,
                                          const pj_json_elem *params,
                                          pj_json_elem *result)
{
    pjmedia_aud_dev_info *devices;
    unsigned count = PJSOCKY_MAX_DEVICES;
    pj_status_t status;
    pj_json_elem *devices_el;
    pj_str_t name;
    unsigned i;

    PJ_UNUSED_ARG(params);

    /* Pool-allocated, not stack: devices[i].name gets referenced (not
     * copied) by pjsocky_json_add_string() below, and must outlive this
     * function - see proto/jsonutil.h's lifetime warning. */
    devices = (pjmedia_aud_dev_info*)pj_pool_alloc(
        pool, PJSOCKY_MAX_DEVICES * sizeof(pjmedia_aud_dev_info));

    status = pjsocky_device_list_audio(devices, &count);
    if (status != PJ_SUCCESS)
        return status;

    devices_el = PJ_POOL_ALLOC_T(pool, pj_json_elem);
    name = pj_str("devices");
    pj_json_elem_array(devices_el, &name);

    for (i = 0; i < count; i++) {
        pj_json_elem *dev = PJ_POOL_ALLOC_T(pool, pj_json_elem);

        pj_json_elem_obj(dev, NULL);
        pjsocky_json_add_number(pool, dev, "id", (float)devices[i].id);
        pjsocky_json_add_string(pool, dev, "name", devices[i].name);
        pjsocky_json_add_number(pool, dev, "input_channels",
                                 (float)devices[i].input_count);
        pjsocky_json_add_number(pool, dev, "output_channels",
                                 (float)devices[i].output_count);
        pj_json_elem_add(devices_el, dev);
    }

    pj_json_elem_add(result, devices_el);
    return PJ_SUCCESS;
}

static pj_status_t cmd_device_list_video(pj_pool_t *pool,
                                          const pj_json_elem *params,
                                          pj_json_elem *result)
{
    pjmedia_vid_dev_info *devices;
    unsigned count = PJSOCKY_MAX_DEVICES;
    pj_status_t status;
    pj_json_elem *devices_el;
    pj_str_t name;
    unsigned i;

    PJ_UNUSED_ARG(params);

    devices = (pjmedia_vid_dev_info*)pj_pool_alloc(
        pool, PJSOCKY_MAX_DEVICES * sizeof(pjmedia_vid_dev_info));

    status = pjsocky_device_list_video(devices, &count);
    if (status != PJ_SUCCESS)
        return status;

    devices_el = PJ_POOL_ALLOC_T(pool, pj_json_elem);
    name = pj_str("devices");
    pj_json_elem_array(devices_el, &name);

    for (i = 0; i < count; i++) {
        pj_json_elem *dev = PJ_POOL_ALLOC_T(pool, pj_json_elem);

        pj_json_elem_obj(dev, NULL);
        pjsocky_json_add_number(pool, dev, "id", (float)devices[i].id);
        pjsocky_json_add_string(pool, dev, "name", devices[i].name);
        pjsocky_json_add_string(pool, dev, "driver", devices[i].driver);
        pjsocky_json_add_string(pool, dev, "dir", vid_dir_str(devices[i].dir));
        pj_json_elem_add(devices_el, dev);
    }

    pj_json_elem_add(result, devices_el);
    return PJ_SUCCESS;
}

static pj_status_t cmd_device_set_audio(pj_pool_t *pool,
                                         const pj_json_elem *params,
                                         pj_json_elem *result)
{
    int capture_id, playback_id;

    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(result);

    if (get_int_member(params, "capture_id", &capture_id) != PJ_SUCCESS ||
        get_int_member(params, "playback_id", &playback_id) != PJ_SUCCESS)
    {
        return PJ_EINVAL;
    }

    return pjsocky_device_set_audio(capture_id, playback_id);
}

static pj_status_t cmd_device_set_video(pj_pool_t *pool,
                                         const pj_json_elem *params,
                                         pj_json_elem *result)
{
    int capture_id;

    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(result);

    if (get_int_member(params, "capture_id", &capture_id) != PJ_SUCCESS)
        return PJ_EINVAL;

    pjsocky_device_set_video_capture(capture_id);
    return PJ_SUCCESS;
}

static pj_status_t cmd_call_dial(pj_pool_t *pool,
                                  const pj_json_elem *params,
                                  pj_json_elem *result)
{
    pj_str_t uri;
    pj_json_elem *video_el;
    pj_bool_t video = PJ_FALSE;
    pjsua_call_id call_id;
    pj_status_t status;

    if (get_string_member(params, "uri", &uri) != PJ_SUCCESS)
        return PJ_EINVAL;

    video_el = pjsocky_json_find(params, "video");
    if (video_el && video_el->type == PJ_JSON_VAL_BOOL)
        video = video_el->value.is_true;

    status = pjsocky_call_dial(&uri, video, &call_id);
    if (status != PJ_SUCCESS)
        return status;

    pjsocky_json_add_number(pool, result, "call_id", (float)call_id);
    return PJ_SUCCESS;
}

static pj_status_t cmd_call_hangup(pj_pool_t *pool,
                                    const pj_json_elem *params,
                                    pj_json_elem *result)
{
    int call_id;
    int code = 486; /* docs/PROTOCOL.md default: Busy Here */

    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(result);

    if (get_int_member(params, "call_id", &call_id) != PJ_SUCCESS)
        return PJ_EINVAL;

    /* "code" is optional - ignore failure, keep the default above. */
    get_int_member(params, "code", &code);

    return pjsocky_call_hangup(call_id, (unsigned)code);
}

static pj_status_t cmd_call_hangup_all(pj_pool_t *pool,
                                        const pj_json_elem *params,
                                        pj_json_elem *result)
{
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(params);
    PJ_UNUSED_ARG(result);
    return pjsocky_call_hangup_all();
}

static pj_status_t cmd_call_get_info(pj_pool_t *pool,
                                      const pj_json_elem *params,
                                      pj_json_elem *result)
{
    int call_id;
    pjsua_call_info *info;
    pj_status_t status;
    pj_bool_t has_audio = PJ_FALSE, has_video = PJ_FALSE;
    unsigned i;

    if (get_int_member(params, "call_id", &call_id) != PJ_SUCCESS)
        return PJ_EINVAL;

    /* Pool-allocated: remote_info/last_status_text get referenced (not
     * copied) by the JSON tree below - see jsonutil.h. */
    info = PJ_POOL_ALLOC_T(pool, pjsua_call_info);

    status = pjsocky_call_get_info(call_id, info);
    if (status != PJ_SUCCESS)
        return status;

    for (i = 0; i < info->media_cnt; i++) {
        if (info->media[i].status != PJSUA_CALL_MEDIA_ACTIVE)
            continue;
        if (info->media[i].type == PJMEDIA_TYPE_AUDIO)
            has_audio = PJ_TRUE;
        else if (info->media[i].type == PJMEDIA_TYPE_VIDEO)
            has_video = PJ_TRUE;
    }

    pjsocky_json_add_number(pool, result, "call_id", (float)info->id);
    pjsocky_json_add_string(pool, result, "state",
                             pjsocky_call_state_str(info->state));
    pjsocky_json_add_number(pool, result, "last_status", (float)info->last_status);
    pjsocky_json_add_str(pool, result, "last_status_text", &info->last_status_text);
    pjsocky_json_add_str(pool, result, "remote_info", &info->remote_info);
    pjsocky_json_add_bool(pool, result, "has_audio", has_audio);
    pjsocky_json_add_bool(pool, result, "has_video", has_video);
    pjsocky_json_add_number(pool, result, "connect_duration_sec",
                             (float)info->connect_duration.sec);

    return PJ_SUCCESS;
}

static pj_status_t cmd_call_answer(pj_pool_t *pool,
                                    const pj_json_elem *params,
                                    pj_json_elem *result)
{
    int call_id;
    int code = 200; /* docs/PROTOCOL.md default */
    pj_json_elem *video_el;
    pj_bool_t video;

    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(result);

    if (get_int_member(params, "call_id", &call_id) != PJ_SUCCESS)
        return PJ_EINVAL;

    /* "code" is optional - ignore failure, keep the default above. */
    get_int_member(params, "code", &code);

    video_el = pjsocky_json_find(params, "video");
    if (video_el && video_el->type == PJ_JSON_VAL_BOOL) {
        video = video_el->value.is_true;
    } else {
        /* Not specified: mirror whatever the incoming offer had - see
         * docs/PROTOCOL.md "call.answer" (this was an open question
         * there, now resolved). */
        pj_status_t status = pjsocky_call_remote_offered_video(call_id, &video);

        if (status != PJ_SUCCESS)
            return status;
    }

    return pjsocky_call_answer(call_id, (unsigned)code, video);
}

static pj_status_t cmd_config_set_ring_timeout(pj_pool_t *pool,
                                                const pj_json_elem *params,
                                                pj_json_elem *result)
{
    int seconds;

    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(result);

    if (get_int_member(params, "seconds", &seconds) != PJ_SUCCESS || seconds < 0)
        return PJ_EINVAL;

    pjsocky_call_set_ring_timeout((unsigned)seconds);
    return PJ_SUCCESS;
}

static pj_status_t cmd_config_get_ring_timeout(pj_pool_t *pool,
                                                const pj_json_elem *params,
                                                pj_json_elem *result)
{
    PJ_UNUSED_ARG(params);

    pjsocky_json_add_number(pool, result, "seconds",
                             (float)pjsocky_call_get_ring_timeout());
    return PJ_SUCCESS;
}

static pj_status_t cmd_im_send(pj_pool_t *pool,
                                const pj_json_elem *params,
                                pj_json_elem *result)
{
    pj_str_t to, content, mime_type;

    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(result);

    if (get_string_member(params, "to", &to) != PJ_SUCCESS ||
        get_string_member(params, "content", &content) != PJ_SUCCESS)
    {
        return PJ_EINVAL;
    }

    if (get_string_member(params, "mime_type", &mime_type) != PJ_SUCCESS)
        mime_type = pj_str("text/plain");

    return pjsocky_im_send(&to, &content, &mime_type);
}

static pj_status_t cmd_im_typing(pj_pool_t *pool,
                                  const pj_json_elem *params,
                                  pj_json_elem *result)
{
    pj_str_t to;
    pj_bool_t is_typing;

    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(result);

    if (get_string_member(params, "to", &to) != PJ_SUCCESS ||
        get_bool_member(params, "is_typing", &is_typing) != PJ_SUCCESS)
    {
        return PJ_EINVAL;
    }

    return pjsocky_im_typing(&to, is_typing);
}

static const cmd_entry CMD_TABLE[] = {
    { "ping", &cmd_ping },
    { "status.get", &cmd_status_get },
    { "account.configure", &cmd_account_configure },
    { "account.register", &cmd_account_register },
    { "account.unregister", &cmd_account_unregister },
    { "account.remove", &cmd_account_remove },
    { "device.list_audio", &cmd_device_list_audio },
    { "device.list_video", &cmd_device_list_video },
    { "device.set_audio", &cmd_device_set_audio },
    { "device.set_video", &cmd_device_set_video },
    { "call.dial", &cmd_call_dial },
    { "call.hangup", &cmd_call_hangup },
    { "call.hangup_all", &cmd_call_hangup_all },
    { "call.get_info", &cmd_call_get_info },
    { "call.answer", &cmd_call_answer },
    { "config.set_ring_timeout", &cmd_config_set_ring_timeout },
    { "config.get_ring_timeout", &cmd_config_get_ring_timeout },
    { "im.send", &cmd_im_send },
    { "im.typing", &cmd_im_typing },
};

static pjsocky_cmd_fn find_cmd(const pj_str_t *name)
{
    unsigned i;

    for (i = 0; i < PJ_ARRAY_SIZE(CMD_TABLE); i++) {
        if (pj_strcmp2(name, CMD_TABLE[i].name) == 0)
            return CMD_TABLE[i].fn;
    }
    return NULL;
}

/* Fill `response` as an {"id":..,"ok":false,"error":{"code":..,"message":..}}
 * error response. `id_val` is the already-extracted request id string. */
static void fail(pj_pool_t *pool, pj_json_elem *response,
                  const pj_str_t *id_val,
                  const char *code, const char *message)
{
    pj_json_elem *error;
    pj_str_t name;

    name = pj_str("");
    pj_json_elem_obj(response, &name);

    {
        pj_json_elem *id_out = PJ_POOL_ALLOC_T(pool, pj_json_elem);
        name = pj_str("id");
        pj_json_elem_string(id_out, &name, (pj_str_t*)id_val);
        pj_json_elem_add(response, id_out);
    }

    pjsocky_json_add_bool(pool, response, "ok", PJ_FALSE);

    error = PJ_POOL_ALLOC_T(pool, pj_json_elem);
    name = pj_str("error");
    pj_json_elem_obj(error, &name);
    pjsocky_json_add_string(pool, error, "code", code);
    pjsocky_json_add_string(pool, error, "message", message);
    pj_json_elem_add(response, error);
}

/* Map a pjsua_* failure into a docs/PROTOCOL.md error code. Handlers
 * return pj_status_t; this is the one place that decides what error
 * "code" string that becomes. */
static const char *error_code_for(pj_status_t status)
{
    switch (status) {
    case PJ_EEXISTS:
    case PJ_EINVALIDOP:
        return "invalid_state";
    case PJ_EINVAL:
        return "invalid_params";
    default:
        return "pjsua_error";
    }
}

pj_status_t pjsocky_dispatch_line(pj_pool_t *pool,
                                   char *line,
                                   pj_size_t line_len,
                                   pj_json_elem *response)
{
    pj_json_err_info err_info;
    unsigned size = (unsigned)line_len;
    pj_json_elem *request;
    pj_json_elem *id_el;
    pj_json_elem *cmd_el;
    pj_json_elem *params_el;
    pj_json_elem *result;
    pjsocky_cmd_fn fn;
    pj_status_t fn_status;
    pj_str_t name;

    request = pj_json_parse(pool, line, &size, &err_info);
    if (!request || request->type != PJ_JSON_VAL_OBJ)
        return PJ_EINVAL;

    id_el = pjsocky_json_find(request, "id");
    if (!id_el || id_el->type != PJ_JSON_VAL_STRING)
        return PJ_EINVAL;

    cmd_el = pjsocky_json_find(request, "cmd");
    if (!cmd_el || cmd_el->type != PJ_JSON_VAL_STRING) {
        fail(pool, response, &id_el->value.str,
             "bad_request", "missing or invalid \"cmd\"");
        return PJ_SUCCESS;
    }

    fn = find_cmd(&cmd_el->value.str);
    if (!fn) {
        fail(pool, response, &id_el->value.str,
             "unknown_command", "no such command");
        return PJ_SUCCESS;
    }

    params_el = pjsocky_json_find(request, "params");

    result = PJ_POOL_ALLOC_T(pool, pj_json_elem);
    name = pj_str("result");
    pj_json_elem_obj(result, &name);

    fn_status = (*fn)(pool, params_el, result);
    if (fn_status != PJ_SUCCESS) {
        /* Must come from `pool`, not a stack buffer: pjsocky_json_add_string()
         * (used by fail()) only copies the pj_str_t (pointer+length), not
         * the underlying bytes, so the string has to outlive this
         * function - it gets read again when the response is serialized
         * in server.c, after this call has returned. */
        char *err_msg = (char*)pj_pool_alloc(pool, PJ_ERR_MSG_SIZE);

        pj_strerror(fn_status, err_msg, PJ_ERR_MSG_SIZE);
        fail(pool, response, &id_el->value.str,
             error_code_for(fn_status), err_msg);
        return PJ_SUCCESS;
    }

    name = pj_str("");
    pj_json_elem_obj(response, &name);

    {
        pj_json_elem *id_out = PJ_POOL_ALLOC_T(pool, pj_json_elem);
        name = pj_str("id");
        pj_json_elem_string(id_out, &name, &id_el->value.str);
        pj_json_elem_add(response, id_out);
    }

    pjsocky_json_add_bool(pool, response, "ok", PJ_TRUE);
    pj_json_elem_add(response, result);

    return PJ_SUCCESS;
}
