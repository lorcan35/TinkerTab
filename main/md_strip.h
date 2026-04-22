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
