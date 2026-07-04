#include "framing.h"

#include <pj/errno.h>
#include <pj/string.h>

void pjsocky_framing_init(pjsocky_framing_t *f, pj_sock_t sock)
{
    f->sock = sock;
    f->len = 0;
}

static pj_status_t send_all(pj_sock_t sock, const char *buf, pj_size_t len)
{
    pj_size_t sent_total = 0;

    while (sent_total < len) {
        pj_ssize_t sent = (pj_ssize_t)(len - sent_total);
        pj_status_t status = pj_sock_send(sock, buf + sent_total, &sent, 0);

        if (status != PJ_SUCCESS)
            return status;
        if (sent <= 0)
            return PJ_EEOF;

        sent_total += (pj_size_t)sent;
    }

    return PJ_SUCCESS;
}

pj_status_t pjsocky_framing_read_line(pjsocky_framing_t *f,
                                       char **line,
                                       pj_size_t *line_len)
{
    for (;;) {
        char *nl = (char*)pj_memchr(f->buf, '\n', f->len);

        if (nl) {
            pj_size_t consumed = (pj_size_t)(nl - f->buf) + 1;
            pj_size_t remaining;

            *line = f->buf;
            *line_len = (pj_size_t)(nl - f->buf);
            f->buf[*line_len] = '\0';

            remaining = f->len - consumed;
            if (remaining > 0)
                pj_memmove(f->buf, f->buf + consumed, remaining);
            f->len = remaining;

            return PJ_SUCCESS;
        }

        if (f->len >= PJSOCKY_MAX_LINE) {
            /* Discard whatever is buffered; caller is expected to close
             * the connection rather than try to resynchronize mid-line. */
            f->len = 0;
            return PJ_ETOOBIG;
        }

        {
            pj_ssize_t recv_len = (pj_ssize_t)(PJSOCKY_MAX_LINE - f->len);
            pj_status_t status = pj_sock_recv(f->sock, f->buf + f->len,
                                               &recv_len, 0);

            if (status != PJ_SUCCESS)
                return status;
            if (recv_len == 0)
                return PJ_EEOF;

            f->len += (pj_size_t)recv_len;
        }
    }
}

pj_status_t pjsocky_framing_write_line(pjsocky_framing_t *f,
                                        const char *line,
                                        pj_size_t line_len)
{
    pj_status_t status = send_all(f->sock, line, line_len);

    if (status != PJ_SUCCESS)
        return status;

    return send_all(f->sock, "\n", 1);
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
