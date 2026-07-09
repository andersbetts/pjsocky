/*
 * A mock video render device, registered at runtime via
 * pjmedia_vid_register_factory() (see main.c).
 *
 * This hardware has no real video display: device.list_video only ever
 * enumerates capture-direction devices (the real camera plus pjmedia's own
 * Colorbar test sources - see colorbar_dev.c). pjsua-lib's video pipeline,
 * however, unconditionally needs a render-capable device to resolve to
 * whenever a video stream's decoding direction is active OR whenever local
 * capture preview is wired up (pjsua_vid_channel_update() creates a hidden
 * "preview" window even for a send-only stream, to loop the local capture
 * through to the encoder - see pjsua_vid.c). With no render device at all,
 * both cases fail immediately with PJMEDIA_EVID_NODEFDEV ("Unable to find
 * default video device"), before the encoder ever gets a chance to run.
 *
 * This factory registers exactly one PJMEDIA_DIR_RENDER device that simply
 * discards every frame handed to it. It exists purely so pjsua's window/port
 * creation succeeds; nothing is ever actually displayed.
 */
#include <pjmedia-videodev/videodev_imp.h>
#include <pj/pool.h>
#include <pj/log.h>
#include <pj/assert.h>
#include <pj/errno.h>

#define THIS_FILE "null_video_dev.c"

struct null_vid_factory
{
    pjmedia_vid_dev_factory base;
    pj_pool_t              *pool;
    pj_pool_factory        *pf;
    pjmedia_vid_dev_info    info;
};

struct null_vid_stream
{
    pjmedia_vid_dev_stream base;
    pjmedia_vid_dev_param  param;
    pj_pool_t             *pool;
};

static pj_status_t factory_init(pjmedia_vid_dev_factory *f);
static pj_status_t factory_destroy(pjmedia_vid_dev_factory *f);
static pj_status_t factory_refresh(pjmedia_vid_dev_factory *f);
static unsigned    factory_get_dev_count(pjmedia_vid_dev_factory *f);
static pj_status_t factory_get_dev_info(pjmedia_vid_dev_factory *f,
                                        unsigned index,
                                        pjmedia_vid_dev_info *info);
static pj_status_t factory_default_param(pj_pool_t *pool,
                                         pjmedia_vid_dev_factory *f,
                                         unsigned index,
                                         pjmedia_vid_dev_param *param);
static pj_status_t factory_create_stream(pjmedia_vid_dev_factory *f,
                                         pjmedia_vid_dev_param *param,
                                         const pjmedia_vid_dev_cb *cb,
                                         void *user_data,
                                         pjmedia_vid_dev_stream **p_vid_strm);

static pj_status_t stream_get_param(pjmedia_vid_dev_stream *s,
                                    pjmedia_vid_dev_param *pi);
static pj_status_t stream_get_cap(pjmedia_vid_dev_stream *s,
                                  pjmedia_vid_dev_cap cap, void *pval);
static pj_status_t stream_set_cap(pjmedia_vid_dev_stream *s,
                                  pjmedia_vid_dev_cap cap, const void *pval);
static pj_status_t stream_start(pjmedia_vid_dev_stream *s);
static pj_status_t stream_put_frame(pjmedia_vid_dev_stream *s,
                                    const pjmedia_frame *frame);
static pj_status_t stream_stop(pjmedia_vid_dev_stream *s);
static pj_status_t stream_destroy(pjmedia_vid_dev_stream *s);

static pjmedia_vid_dev_factory_op factory_op =
{
    &factory_init,
    &factory_destroy,
    &factory_get_dev_count,
    &factory_get_dev_info,
    &factory_default_param,
    &factory_create_stream,
    &factory_refresh
};

static pjmedia_vid_dev_stream_op stream_op =
{
    &stream_get_param,
    &stream_get_cap,
    &stream_set_cap,
    &stream_start,
    NULL, /* get_frame: render-only, never pulled from */
    &stream_put_frame,
    &stream_stop,
    &stream_destroy
};

pjmedia_vid_dev_factory *pjsocky_null_vid_factory(pj_pool_factory *pf)
{
    pj_pool_t *pool = pj_pool_create(pf, "null-vid-render", 512, 512, NULL);
    struct null_vid_factory *f = PJ_POOL_ZALLOC_T(pool, struct null_vid_factory);

    f->pf = pf;
    f->pool = pool;
    f->base.op = &factory_op;

    return &f->base;
}

static pj_status_t factory_init(pjmedia_vid_dev_factory *f)
{
    struct null_vid_factory *nf = (struct null_vid_factory *)f;

    pj_bzero(&nf->info, sizeof(nf->info));
    pj_ansi_strxcpy(nf->info.name, "Null renderer", sizeof(nf->info.name));
    pj_ansi_strxcpy(nf->info.driver, "Null", sizeof(nf->info.driver));
    nf->info.dir = PJMEDIA_DIR_RENDER;
    nf->info.has_callback = PJ_FALSE;
    nf->info.caps = 0;
    nf->info.fmt_cnt = 1;
    pjmedia_format_init_video(&nf->info.fmt[0], PJMEDIA_FORMAT_I420,
                              640, 480, 25, 1);

    PJ_LOG(4, (THIS_FILE, "Null video renderer initialized"));

    return PJ_SUCCESS;
}

static pj_status_t factory_destroy(pjmedia_vid_dev_factory *f)
{
    struct null_vid_factory *nf = (struct null_vid_factory *)f;

    pj_pool_safe_release(&nf->pool);

    return PJ_SUCCESS;
}

static pj_status_t factory_refresh(pjmedia_vid_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return PJ_SUCCESS;
}

static unsigned factory_get_dev_count(pjmedia_vid_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return 1;
}

static pj_status_t factory_get_dev_info(pjmedia_vid_dev_factory *f,
                                        unsigned index,
                                        pjmedia_vid_dev_info *info)
{
    struct null_vid_factory *nf = (struct null_vid_factory *)f;

    PJ_ASSERT_RETURN(index == 0, PJMEDIA_EVID_INVDEV);

    pj_memcpy(info, &nf->info, sizeof(*info));

    return PJ_SUCCESS;
}

static pj_status_t factory_default_param(pj_pool_t *pool,
                                         pjmedia_vid_dev_factory *f,
                                         unsigned index,
                                         pjmedia_vid_dev_param *param)
{
    struct null_vid_factory *nf = (struct null_vid_factory *)f;

    PJ_ASSERT_RETURN(index == 0, PJMEDIA_EVID_INVDEV);
    PJ_UNUSED_ARG(pool);

    pj_bzero(param, sizeof(*param));
    param->dir = PJMEDIA_DIR_RENDER;
    param->rend_id = index;
    param->cap_id = PJMEDIA_VID_INVALID_DEV;
    param->clock_rate = 0;
    param->flags = 0;
    pj_memcpy(&param->fmt, &nf->info.fmt[0], sizeof(param->fmt));

    return PJ_SUCCESS;
}

static pj_status_t factory_create_stream(pjmedia_vid_dev_factory *f,
                                         pjmedia_vid_dev_param *param,
                                         const pjmedia_vid_dev_cb *cb,
                                         void *user_data,
                                         pjmedia_vid_dev_stream **p_vid_strm)
{
    struct null_vid_factory *nf = (struct null_vid_factory *)f;
    pj_pool_t *pool;
    struct null_vid_stream *strm;

    PJ_UNUSED_ARG(cb);
    PJ_UNUSED_ARG(user_data);
    PJ_ASSERT_RETURN(f && param && p_vid_strm, PJ_EINVAL);

    pool = pj_pool_create(nf->pf, "null-vid-render-strm", 512, 512, NULL);
    PJ_ASSERT_RETURN(pool != NULL, PJ_ENOMEM);

    strm = PJ_POOL_ZALLOC_T(pool, struct null_vid_stream);
    strm->pool = pool;
    pj_memcpy(&strm->param, param, sizeof(*param));

    strm->base.op = &stream_op;
    *p_vid_strm = &strm->base;

    return PJ_SUCCESS;
}

static pj_status_t stream_get_param(pjmedia_vid_dev_stream *s,
                                    pjmedia_vid_dev_param *pi)
{
    struct null_vid_stream *strm = (struct null_vid_stream *)s;

    PJ_ASSERT_RETURN(strm && pi, PJ_EINVAL);

    pj_memcpy(pi, &strm->param, sizeof(*pi));

    return PJ_SUCCESS;
}

static pj_status_t stream_get_cap(pjmedia_vid_dev_stream *s,
                                  pjmedia_vid_dev_cap cap, void *pval)
{
    PJ_UNUSED_ARG(s);
    PJ_UNUSED_ARG(cap);
    PJ_UNUSED_ARG(pval);
    return PJMEDIA_EVID_INVCAP;
}

static pj_status_t stream_set_cap(pjmedia_vid_dev_stream *s,
                                  pjmedia_vid_dev_cap cap, const void *pval)
{
    PJ_UNUSED_ARG(s);
    PJ_UNUSED_ARG(cap);
    PJ_UNUSED_ARG(pval);
    return PJMEDIA_EVID_INVCAP;
}

static pj_status_t stream_start(pjmedia_vid_dev_stream *s)
{
    PJ_UNUSED_ARG(s);
    return PJ_SUCCESS;
}

static pj_status_t stream_put_frame(pjmedia_vid_dev_stream *s,
                                    const pjmedia_frame *frame)
{
    /* Discard: this is a mock sink for hardware with no real video display. */
    PJ_UNUSED_ARG(s);
    PJ_UNUSED_ARG(frame);
    return PJ_SUCCESS;
}

static pj_status_t stream_stop(pjmedia_vid_dev_stream *s)
{
    PJ_UNUSED_ARG(s);
    return PJ_SUCCESS;
}

static pj_status_t stream_destroy(pjmedia_vid_dev_stream *s)
{
    struct null_vid_stream *strm = (struct null_vid_stream *)s;

    pj_pool_safe_release(&strm->pool);

    return PJ_SUCCESS;
}
