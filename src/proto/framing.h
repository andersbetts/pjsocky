/*
 * NDJSON (newline-delimited JSON) framing over a connected stream socket,
 * per docs/PROTOCOL.md ("Framing").
 */
#ifndef PJSOCKY_FRAMING_H
#define PJSOCKY_FRAMING_H

#include <pj/sock.h>
#include <pj/types.h>
#include <pjlib-util/json.h>

PJ_BEGIN_DECL

/* Max line length, matches docs/PROTOCOL.md ("Framing"). */
#define PJSOCKY_MAX_LINE    8192

typedef struct pjsocky_framing
{
    pj_sock_t   sock;
    pj_size_t   len;                       /* bytes currently buffered */
    char        buf[PJSOCKY_MAX_LINE + 1];
} pjsocky_framing_t;

void pjsocky_framing_init(pjsocky_framing_t *f, pj_sock_t sock);

/*
 * Block until one complete NDJSON line is available, the peer closes the
 * connection, or an error occurs.
 *
 * On PJ_SUCCESS, *line points into the framing object's own buffer (valid
 * only until the next call on this object) and is NUL-terminated;
 * *line_len excludes the trailing newline.
 *
 * Returns PJ_EEOF if the peer closed the connection, PJ_ETOOBIG if a line
 * exceeds PJSOCKY_MAX_LINE bytes (buffered data is discarded - the caller
 * should close the connection, per docs/PROTOCOL.md), or the underlying
 * pj_sock_recv() error otherwise.
 */
pj_status_t pjsocky_framing_read_line(pjsocky_framing_t *f,
                                       char **line,
                                       pj_size_t *line_len);

/* Write line_len bytes from line, followed by a single '\n'. */
pj_status_t pjsocky_framing_write_line(pjsocky_framing_t *f,
                                        const char *line,
                                        pj_size_t line_len);

/*
 * Serialize `elem` and write it as one NDJSON line. pj_json_write()
 * always pretty-prints (indentation and embedded newlines), which would
 * violate the "one JSON object per line" framing contract if written
 * as-is - this compacts the rendered JSON (dropping insignificant
 * whitespace outside of quoted strings) before writing it.
 *
 * `scratch` is caller-provided workspace of at least `scratch_size`
 * bytes (PJSOCKY_MAX_LINE is enough for any response/event this
 * protocol produces); its contents are undefined on return.
 */
pj_status_t pjsocky_framing_write_json(pjsocky_framing_t *f,
                                        const pj_json_elem *elem,
                                        char *scratch,
                                        pj_size_t scratch_size);

PJ_END_DECL

#endif /* PJSOCKY_FRAMING_H */
