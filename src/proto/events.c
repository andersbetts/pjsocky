#include "events.h"
#include "jsonutil.h"

#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/os.h>
#include <pj/pool.h>
#include <pj/string.h>

struct pjsocky_events
{
    pj_pool_factory     *pool_factory;
    pj_mutex_t          *lock;
    pjsocky_framing_t   *framing;   /* NULL when no client is connected */
};

static pjsocky_events_t *g_instance;

pj_status_t pjsocky_events_create(pj_pool_t *pool, pjsocky_events_t **p_events)
{
    pjsocky_events_t *events;
    pj_status_t status;

    PJ_ASSERT_RETURN(pool && p_events, PJ_EINVAL);

    events = PJ_POOL_ZALLOC_T(pool, pjsocky_events_t);
    events->pool_factory = pool->factory;

    status = pj_mutex_create_simple(pool, "pjsocky-events", &events->lock);
    if (status != PJ_SUCCESS)
        return status;

    g_instance = events;
    *p_events = events;
    return PJ_SUCCESS;
}

pjsocky_events_t *pjsocky_events_instance(void)
{
    return g_instance;
}

void pjsocky_events_set_conn(pjsocky_events_t *events, pjsocky_framing_t *framing)
{
    if (!events)
        return;

    pj_mutex_lock(events->lock);
    events->framing = framing;
    pj_mutex_unlock(events->lock);
}

pj_status_t pjsocky_events_write_response(pjsocky_events_t *events,
                                           const pj_json_elem *response,
                                           char *scratch,
                                           pj_size_t scratch_size)
{
    pj_status_t status;

    PJ_ASSERT_RETURN(events, PJ_EINVAL);

    pj_mutex_lock(events->lock);

    if (!events->framing) {
        pj_mutex_unlock(events->lock);
        return PJ_SUCCESS;
    }

    status = pjsocky_framing_write_json(events->framing, response,
                                         scratch, scratch_size);

    pj_mutex_unlock(events->lock);
    return status;
}

pj_status_t pjsocky_events_push(pjsocky_events_t *events,
                                 const char *event_name,
                                 pjsocky_event_builder builder,
                                 void *user_data)
{
    pj_pool_t *pool;
    pj_json_elem event, data;
    pj_str_t name;
    char scratch[PJSOCKY_MAX_LINE];
    pj_status_t status;

    if (!events)
        return PJ_EINVAL;

    pj_mutex_lock(events->lock);

    if (!events->framing) {
        pj_mutex_unlock(events->lock);
        return PJ_SUCCESS;
    }

    pool = pj_pool_create(events->pool_factory, "pjsocky-event", 1000, 1000, NULL);
    if (!pool) {
        pj_mutex_unlock(events->lock);
        return PJ_ENOMEM;
    }

    name = pj_str("");
    pj_json_elem_obj(&event, &name);
    pjsocky_json_add_string(pool, &event, "event", event_name);

    name = pj_str("data");
    pj_json_elem_obj(&data, &name);
    if (builder)
        (*builder)(pool, &data, user_data);
    pj_json_elem_add(&event, &data);

    status = pjsocky_framing_write_json(events->framing, &event,
                                         scratch, sizeof(scratch));

    pj_pool_release(pool);
    pj_mutex_unlock(events->lock);

    return status;
}
