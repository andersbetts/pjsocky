#include "im.h"

#include "account.h"
#include "proto/events.h"
#include "proto/jsonutil.h"

pj_status_t pjsocky_im_send(const pj_str_t *to, const pj_str_t *content,
                             const pj_str_t *mime_type)
{
    pjsua_acc_id acc_id = pjsocky_account_get_id();

    if (acc_id == PJSUA_INVALID_ID || !pjsocky_account_is_registered())
        return PJ_EINVALIDOP;

    /* pjsua_im_send() duplicates its string arguments into its own pool
     * before returning, same as pjsua_acc_add()/pjsua_call_make_call() -
     * see account.c's comment on pjsua_acc_config_dup(). */
    return pjsua_im_send(acc_id, to, mime_type, content, NULL, NULL);
}

pj_status_t pjsocky_im_typing(const pj_str_t *to, pj_bool_t is_typing)
{
    pjsua_acc_id acc_id = pjsocky_account_get_id();

    if (acc_id == PJSUA_INVALID_ID || !pjsocky_account_is_registered())
        return PJ_EINVALIDOP;

    return pjsua_im_typing(acc_id, to, is_typing, NULL);
}

struct incoming_message_ctx
{
    const pj_str_t *from;
    const pj_str_t *to;
    const pj_str_t *mime_type;
    const pj_str_t *body;
};

static void build_incoming_message_data(pj_pool_t *pool, pj_json_elem *data,
                                         void *user_data)
{
    const struct incoming_message_ctx *ctx =
        (const struct incoming_message_ctx*)user_data;

    pjsocky_json_add_str(pool, data, "from", ctx->from);
    pjsocky_json_add_str(pool, data, "to", ctx->to);
    pjsocky_json_add_str(pool, data, "mime_type", ctx->mime_type);
    pjsocky_json_add_str(pool, data, "body", ctx->body);
}

void pjsocky_im_on_pager2(pjsua_call_id call_id, const pj_str_t *from,
                          const pj_str_t *to, const pj_str_t *contact,
                          const pj_str_t *mime_type, const pj_str_t *body,
                          pjsip_rx_data *rdata, pjsua_acc_id acc_id)
{
    struct incoming_message_ctx ctx;

    PJ_UNUSED_ARG(call_id);
    PJ_UNUSED_ARG(contact);
    PJ_UNUSED_ARG(rdata);
    PJ_UNUSED_ARG(acc_id);

    /* ctx is a stack local, but pjsocky_events_push() calls
     * build_incoming_message_data() synchronously before returning - see
     * proto/events.h - so it never needs to outlive this function. */
    ctx.from = from;
    ctx.to = to;
    ctx.mime_type = mime_type;
    ctx.body = body;

    pjsocky_events_push(pjsocky_events_instance(), "incoming_message",
                         &build_incoming_message_data, &ctx);
}

struct message_status_ctx
{
    const pj_str_t *to;
    const pj_str_t *body;
    pjsip_status_code status;
    const pj_str_t *reason;
};

static void build_message_status_data(pj_pool_t *pool, pj_json_elem *data,
                                       void *user_data)
{
    const struct message_status_ctx *ctx =
        (const struct message_status_ctx*)user_data;

    pjsocky_json_add_str(pool, data, "to", ctx->to);
    pjsocky_json_add_str(pool, data, "body", ctx->body);
    pjsocky_json_add_number(pool, data, "status", (float)ctx->status);
    pjsocky_json_add_str(pool, data, "reason", ctx->reason);
}

void pjsocky_im_on_pager_status2(pjsua_call_id call_id, const pj_str_t *to,
                                 const pj_str_t *body, void *user_data,
                                 pjsip_status_code status,
                                 const pj_str_t *reason,
                                 pjsip_tx_data *tdata, pjsip_rx_data *rdata,
                                 pjsua_acc_id acc_id)
{
    struct message_status_ctx ctx;

    PJ_UNUSED_ARG(call_id);
    PJ_UNUSED_ARG(user_data);
    PJ_UNUSED_ARG(tdata);
    PJ_UNUSED_ARG(rdata);
    PJ_UNUSED_ARG(acc_id);

    ctx.to = to;
    ctx.body = body;
    ctx.status = status;
    ctx.reason = reason;

    pjsocky_events_push(pjsocky_events_instance(), "message_status",
                         &build_message_status_data, &ctx);
}

struct typing_ctx
{
    const pj_str_t *from;
    const pj_str_t *to;
    pj_bool_t is_typing;
};

static void build_typing_data(pj_pool_t *pool, pj_json_elem *data,
                               void *user_data)
{
    const struct typing_ctx *ctx = (const struct typing_ctx*)user_data;

    pjsocky_json_add_str(pool, data, "from", ctx->from);
    pjsocky_json_add_str(pool, data, "to", ctx->to);
    pjsocky_json_add_bool(pool, data, "is_typing", ctx->is_typing);
}

void pjsocky_im_on_typing2(pjsua_call_id call_id, const pj_str_t *from,
                           const pj_str_t *to, const pj_str_t *contact,
                           pj_bool_t is_typing, pjsip_rx_data *rdata,
                           pjsua_acc_id acc_id)
{
    struct typing_ctx ctx;

    PJ_UNUSED_ARG(call_id);
    PJ_UNUSED_ARG(contact);
    PJ_UNUSED_ARG(rdata);
    PJ_UNUSED_ARG(acc_id);

    ctx.from = from;
    ctx.to = to;
    ctx.is_typing = is_typing;

    pjsocky_events_push(pjsocky_events_instance(), "typing",
                         &build_typing_data, &ctx);
}
