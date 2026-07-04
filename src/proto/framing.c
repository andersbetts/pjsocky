#include "framing.h"

#include <pj/errno.h>
#include <pj/os.h>
#include <pj/string.h>

#include <errno.h>
#include <sys/select.h>

void pjsocky_framing_init(pjsocky_framing_t *f, pj_sock_t sock,
                          unsigned write_timeout_msec)
{
    f->sock = sock;
    f->write_timeout_msec = write_timeout_msec;
    f->start = 0;
    f->len = 0;
}

static pj_bool_t is_would_block(pj_status_t status)
{
    return status == PJ_STATUS_FROM_OS(EAGAIN)
#if EAGAIN != EWOULDBLOCK
        || status == PJ_STATUS_FROM_OS(EWOULDBLOCK)
#endif
        ;
}

/*
 * Send the whole buffer, enforcing the write deadline from
 * docs/PROTOCOL.md "Backpressure": the socket is non-blocking, so a full
 * socket buffer surfaces as EWOULDBLOCK here rather than blocking the
 * calling thread (which may be pjsua's own worker thread pushing an
 * event - see proto/events.h). Wait for writability only as long as the
 * deadline allows, measured across the whole message, not per send()
 * call.
 */
static pj_status_t send_all(pj_sock_t sock, const char *buf, pj_size_t len,
                            unsigned timeout_msec)
{
    pj_size_t sent_total = 0;
    pj_time_val start;

    pj_gettickcount(&start);

    while (sent_total < len) {
        pj_ssize_t sent = (pj_ssize_t)(len - sent_total);
        pj_status_t status = pj_sock_send(sock, buf + sent_total, &sent, 0);

        if (status == PJ_SUCCESS) {
            if (sent <= 0)
                return PJ_EEOF;
            sent_total += (pj_size_t)sent;
            continue;
        }

        if (!is_would_block(status))
            return status;

        /* Socket buffer full: wait for writability within what's left of
         * the deadline. */
        {
            pj_time_val now;
            long remaining_msec;
            fd_set wfds;
            struct timeval tv;
            int n;

            pj_gettickcount(&now);
            PJ_TIME_VAL_SUB(now, start);
            remaining_msec = (long)timeout_msec - PJ_TIME_VAL_MSEC(now);
            if (remaining_msec <= 0)
                return PJ_ETIMEDOUT;

            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            tv.tv_sec = remaining_msec / 1000;
            tv.tv_usec = (remaining_msec % 1000) * 1000;

            n = select((int)sock + 1, NULL, &wfds, NULL, &tv);
            if (n < 0) {
                pj_status_t sel_status = pj_get_os_error();

                if (sel_status == PJ_STATUS_FROM_OS(EINTR))
                    continue;
                return sel_status;
            }
            if (n == 0)
                return PJ_ETIMEDOUT;
        }
    }

    return PJ_SUCCESS;
}

pj_status_t pjsocky_framing_fill(pjsocky_framing_t *f)
{
    pj_ssize_t recv_len;
    pj_status_t status;

    /* Reclaim the space of already-extracted lines so recv() has the
     * full remaining capacity. This is the only place buffered data
     * ever moves: extract_line() must not memmove, because the line it
     * returns points into this same buffer and stays live (dispatch
     * parses it in place) until after any further pipelined lines have
     * been extracted - moving memory there corrupts a line mid-use.
     * That's not hypothetical: the original read_line() compacted at
     * extract time and two requests arriving in one recv() made the
     * daemon parse the second request twice, first response lost. By
     * the time the server calls fill() again, every previously returned
     * line has been fully handled (framing.h documents the lifetime). */
    if (f->start > 0) {
        if (f->len > 0)
            pj_memmove(f->buf, f->buf + f->start, f->len);
        f->start = 0;
    }

    /* Buffer already full with no newline: extract_line() will report
     * PJ_ETOOBIG; nothing sensible to read into until the caller acts
     * on that. */
    recv_len = (pj_ssize_t)(PJSOCKY_MAX_LINE - f->len);
    if (recv_len <= 0)
        return PJ_SUCCESS;

    status = pj_sock_recv(f->sock, f->buf + f->len, &recv_len, 0);
    if (status != PJ_SUCCESS)
        return is_would_block(status) ? PJ_EPENDING : status;
    if (recv_len == 0)
        return PJ_EEOF;

    f->len += (pj_size_t)recv_len;
    return PJ_SUCCESS;
}

pj_status_t pjsocky_framing_extract_line(pjsocky_framing_t *f,
                                          char **line,
                                          pj_size_t *line_len)
{
    char *base = f->buf + f->start;
    char *nl = (char*)pj_memchr(base, '\n', f->len);

    if (!nl) {
        if (f->len >= PJSOCKY_MAX_LINE) {
            /* Discard whatever is buffered; caller is expected to close
             * the connection rather than try to resynchronize mid-line. */
            f->len = 0;
            f->start = 0;
            return PJ_ETOOBIG;
        }
        return PJ_EPENDING;
    }

    {
        pj_size_t consumed = (pj_size_t)(nl - base) + 1;

        *line = base;
        *line_len = (pj_size_t)(nl - base);
        base[*line_len] = '\0';

        f->start += consumed;
        f->len -= consumed;
        if (f->len == 0)
            f->start = 0;
    }

    return PJ_SUCCESS;
}

pj_status_t pjsocky_framing_write_line(pjsocky_framing_t *f,
                                        const char *line,
                                        pj_size_t line_len)
{
    pj_status_t status = send_all(f->sock, line, line_len,
                                   f->write_timeout_msec);

    if (status != PJ_SUCCESS)
        return status;

    return send_all(f->sock, "\n", 1, f->write_timeout_msec);
}

/*
 * Compact pj_json_write()'s pretty-printed output in place by dropping
 * whitespace outside of quoted strings. pj_json_write() escapes quotes,
 * backslashes and control characters (including real newlines) within
 * string values, so any raw space/tab/CR/LF byte encountered while not
 * inside a quoted string is always pretty-printer formatting, never
 * string content - safe to drop unconditionally.
 */
static pj_size_t compact_json(char *buf, pj_size_t len)
{
    pj_size_t i, out = 0;
    pj_bool_t in_string = PJ_FALSE;
    pj_bool_t escaped = PJ_FALSE;

    for (i = 0; i < len; i++) {
        char c = buf[i];

        if (in_string) {
            buf[out++] = c;
            if (escaped) {
                escaped = PJ_FALSE;
            } else if (c == '\\') {
                escaped = PJ_TRUE;
            } else if (c == '"') {
                in_string = PJ_FALSE;
            }
            continue;
        }

        if (c == '"') {
            in_string = PJ_TRUE;
            buf[out++] = c;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;

        buf[out++] = c;
    }

    return out;
}

pj_status_t pjsocky_framing_write_json(pjsocky_framing_t *f,
                                        const pj_json_elem *elem,
                                        char *scratch,
                                        pj_size_t scratch_size)
{
    unsigned size = (unsigned)scratch_size;
    pj_status_t status;
    pj_size_t compact_len;

    status = pj_json_write(elem, scratch, &size);
    if (status != PJ_SUCCESS)
        return status;

    compact_len = compact_json(scratch, (pj_size_t)size);

    return pjsocky_framing_write_line(f, scratch, compact_len);
}
