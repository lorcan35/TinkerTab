#pragma once
#include <stddef.h>

/* W15-P03 (#116): strip common inline-markdown markers so a chat label
 * renders as clean prose instead of leaking '**', '*', '#', '- ', etc.
 * to the user.  Not a full markdown parser — just visual noise removal.
 *
 * Rules applied in order:
 *   - '**' and '__' pair markers        → removed, content kept
 *   - '*' and '_' pair markers (single) → removed, content kept
 *   - leading '# ' / '## ' / '### '     → removed from line start
 *   - leading '- ' / '* '                → replaced with '• '
 *   - leading '1. ' / '2. ' etc          → kept as-is (numbered lists read fine)
 *
 * Works in-place if `out == in`, truncates to `out_cap - 1` bytes
 * (always NUL-terminated).  Safe for empty / NULL input.
 */
void md_strip_inline(const char *in, char *out, size_t out_cap);

/* Audit C10 (TinkerBox #137 / TinkerTab #200): voice-overlay caption
 * formatter.  Same as md_strip_inline but appends a UTF-8 ellipsis
 * "…" (3 bytes) when the source text was truncated by `out_cap`.
 *
 * The voice overlay caption is a small bubble in the LVGL voice
 * overlay; rendering hundreds of characters there overflows the
 * visual bubble + thrashes LVGL.  The full reply is always available
 * in the chat screen below — the voice caption only needs enough
 * to indicate progress.
 *
 * Always NUL-terminated.  Safe for empty / NULL input.
 */
void md_strip_inline_with_ellipsis(const char *in, char *out, size_t out_cap);

/* Issues #78 + #160: strip Dragon's tool-call XML markers from a
 * streamed LLM response so the user doesn't see raw
 *   <tool>recall</tool><args>{"query":"..."}</args>
 * land as a chat bubble.  Tab5-side defensive scrub — Dragon may also
 * strip server-side, but a token-stream timing race can still leak the
 * markers (the audit issue caught this in a real screenshot).
 *
 * Removes any well-formed `<tool>...</tool>` and `<args>...</args>`
 * sub-strings (case-insensitive, span newlines), collapses the
 * resulting double-spaces, and trims leading/trailing whitespace.
 * Works in-place when out == in.  Always NUL-terminated.
 */
void md_strip_tool_markers(const char *in, char *out, size_t out_cap);
