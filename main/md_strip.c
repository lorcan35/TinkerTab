/* W15-P03 (#116): inline markdown stripper.  See md_strip.h. */
#include "md_strip.h"

#include <stdbool.h>
#include <string.h>
#include <ctype.h>

static bool is_line_start(const char *buf, const char *p)
{
    if (p == buf) return true;
    return *(p - 1) == '\n';
}

void md_strip_inline(const char *in, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return;
    if (!in) { out[0] = 0; return; }

    size_t oi = 0;
    size_t n = strlen(in);
    for (size_t i = 0; i < n && oi + 1 < out_cap; ) {
        char c = in[i];

        /* Heading markers at line start: "# ", "## ", "### ".
         * Drop the # and space; keep the rest of the line. */
        if (c == '#' && is_line_start(in, &in[i])) {
            size_t j = i;
            while (j < n && in[j] == '#') j++;
            if (j < n && in[j] == ' ') {
                i = j + 1;
                continue;
            }
        }

        /* Leading bullet "- " / "* " → "• ". */
        if ((c == '-' || c == '*') && is_line_start(in, &in[i])
            && i + 1 < n && in[i + 1] == ' ') {
            /* UTF-8 bullet (•) = E2 80 A2 */
            if (oi + 4 < out_cap) {
                out[oi++] = (char)0xE2;
                out[oi++] = (char)0x80;
                out[oi++] = (char)0xA2;
                out[oi++] = ' ';
            }
            i += 2;
            continue;
        }

        /* Bold / italic markers: '**', '__' (pair) or '*', '_' (single).
         * Drop the markers, keep inner text. */
        if (c == '*' || c == '_') {
            bool doubled = (i + 1 < n && in[i + 1] == c);
            i += doubled ? 2 : 1;
            continue;
        }

        out[oi++] = c;
        i++;
    }
    out[oi] = 0;
}

/* Audit C10 (TinkerBox #137 / TinkerTab #200): same as md_strip_inline
 * but appends a UTF-8 ellipsis ("…", 3 bytes) when the source was
 * truncated by `out_cap`.  Reserves 4 trailing bytes for the
 * ellipsis + NUL on the truncation path.  Falls through to the
 * vanilla strip when the source fits comfortably. */
void md_strip_inline_with_ellipsis(const char *in, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return;
    if (!in) { out[0] = 0; return; }

    /* Reserve room for "…" (3 bytes) + NUL.  If out_cap is too small
     * for the marker, fall back to the unmarked strip — better to
     * lose the ellipsis than to refuse a render. */
    if (out_cap < 8) {
        md_strip_inline(in, out, out_cap);
        return;
    }
    size_t reserved = 3;  /* UTF-8 ellipsis */
    md_strip_inline(in, out, out_cap - reserved);
    /* Detect truncation: source had more bytes than we wrote (and we
     * filled to capacity).  md_strip_inline NUL-terminates at the
     * stopping point regardless. */
    size_t written = 0;
    while (written < out_cap - reserved && out[written]) written++;
    /* Use input length as a lower bound — markdown stripping shrinks
     * the input, so source-len > written-cap implies truncation. */
    size_t in_len = 0;
    while (in[in_len]) in_len++;
    if (in_len > written) {
        out[written++] = (char)0xE2;
        out[written++] = (char)0x80;
        out[written++] = (char)0xA6;
        out[written]   = 0;
    }
}
