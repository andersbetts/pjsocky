/*
 * NDJSON (newline-delimited JSON) framing over a connected stream socket,
 * per docs/PROTOCOL.md ("Framing").
 *
 * The connection socket is expected to be in non-blocking mode (server.c
 * sets this on accept): reads are driven by the server's select() loop
 * via pjsocky_framing_fill()/pjsocky_framing_extract_line(), and writes
 * enforce the bounded deadline described in docs/PROTOCOL.md
 * ("Backpressure") instead of blocking forever on a stalled client.
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
    unsigned    write_timeout_msec;        /* docs/PROTOCOL.md "Backpressure" */
    pj_size_t   start;                     /* offset of first unconsumed byte */
    pj_size_t   len;                       /* bytes buffered from `start` on */
    char        buf[PJSOCKY_MAX_LINE + 1];
} pjsocky_framing_t;

void pjsocky_framing_init(pjsocky_framing_t *f, pj_sock_t sock,
                          unsigned write_timeout_msec);

/*
 * Read once from the socket into the framing buffer. Never blocks (the
 * socket is non-blocking); call it when select() reports readability,
 * then drain complete lines with pjsocky_framing_extract_line().
 *
 * Returns PJ_SUCCESS if at least one byte was buffered, PJ_EEOF if the
 * peer closed the connection, PJ_EPENDING on a spurious wakeup (no data
 * after all - not an error), or the underlying pj_sock_recv() error.
 */
pj_status_t pjsocky_framing_fill(pjsocky_framing_t *f);

/*
 * Extract one complete NDJSON line from the framing buffer, if present.
 *
 * On PJ_SUCCESS, *line points into the framing object's own buffer and
 * is NUL-terminated; *line_len excludes the trailing newline. The line
 * stays valid across further extract calls (needed for pipelined
 * requests parsed in place - see dispatch.c's lifetime note) but NOT
 * across the next pjsocky_framing_fill(), which compacts the buffer.
 *
 * Returns PJ_EPENDING if no complete line is buffered yet (fill again
 * once select() reports more data), or PJ_ETOOBIG if the buffer is full
 * with no newline in sight - buffered data is discarded and the caller
 * must close the connection, since the rest of the oversized line would
 * be misread as new messages (docs/PROTOCOL.md "Robustness").
 */
pj_status_t pjsocky_framing_extract_line(pjsocky_framing_t *f,
                                          char **line,
                                          pj_size_t *line_len);

/*
 * Write line_len bytes from line, followed by a single '\n'.
 *
 * If the client isn't draining its socket and the buffer stays full past
 * write_timeout_msec, returns PJ_ETIMEDOUT - the caller should treat the
 * client as dead and drop the connection (docs/PROTOCOL.md
 * "Backpressure").
 */
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
