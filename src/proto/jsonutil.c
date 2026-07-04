#include "jsonutil.h"

#include <pj/string.h>

pj_json_elem *pjsocky_json_find(const pj_json_elem *obj, const char *name)
{
    pj_json_elem *it;

    if (!obj || obj->type != PJ_JSON_VAL_OBJ)
        return NULL;

    it = obj->value.children.next;
    while (it != (pj_json_elem*)&obj->value.children) {
        if (pj_strcmp2(&it->name, name) == 0)
            return it;
        it = it->next;
    }
    return NULL;
}

void pjsocky_json_add_bool(pj_pool_t *pool, pj_json_elem *obj,
                            const char *name, pj_bool_t val)
{
    pj_json_elem *el = PJ_POOL_ALLOC_T(pool, pj_json_elem);
    pj_str_t name_str = pj_str((char*)name);

    pj_json_elem_bool(el, &name_str, val);
    pj_json_elem_add(obj, el);
}

void pjsocky_json_add_string(pj_pool_t *pool, pj_json_elem *obj,
                              const char *name, const char *val)
{
    pj_json_elem *el = PJ_POOL_ALLOC_T(pool, pj_json_elem);
    pj_str_t name_str = pj_str((char*)name);
    pj_str_t val_str = pj_str((char*)val);

    pj_json_elem_string(el, &name_str, &val_str);
    pj_json_elem_add(obj, el);
}

void pjsocky_json_add_str(pj_pool_t *pool, pj_json_elem *obj,
                           const char *name, const pj_str_t *val)
{
    pj_json_elem *el = PJ_POOL_ALLOC_T(pool, pj_json_elem);
    pj_str_t name_str = pj_str((char*)name);

    pj_json_elem_string(el, &name_str, (pj_str_t*)val);
    pj_json_elem_add(obj, el);
}

void pjsocky_json_add_number(pj_pool_t *pool, pj_json_elem *obj,
                              const char *name, float val)
{
    pj_json_elem *el = PJ_POOL_ALLOC_T(pool, pj_json_elem);
    pj_str_t name_str = pj_str((char*)name);

    pj_json_elem_number(el, &name_str, val);
    pj_json_elem_add(obj, el);
}
