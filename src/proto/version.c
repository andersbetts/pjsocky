#include "version.h"
#include "jsonutil.h"

#include <pj/config.h>

void pjsocky_version_fill(pj_pool_t *pool, pj_json_elem *obj)
{
    pjsocky_json_add_string(pool, obj, "protocol_version",
                            PJSOCKY_PROTOCOL_VERSION);
    pjsocky_json_add_string(pool, obj, "daemon_version", PJSOCKY_VERSION);

    /* Which pjproject went into this binary. It is reachable nowhere else
     * from outside the process - a controller links no pjsip of its own,
     * and the daemon has no command-line flag to ask - so without this a
     * SIP stack bug in the field can be traced to a pjsocky release but
     * not to the pjproject under it, which is where such bugs usually
     * live. Static storage, from pjlib's own version constant. */
    pjsocky_json_add_string(pool, obj, "pjsip_version", pj_get_version());
}
