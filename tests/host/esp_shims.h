/* esp_shims.h — minimal host-build shims so pure-logic ESP-IDF modules
 * compile in tests/host/ without dragging in the real ESP-IDF tree.
 *
 * Strategy: each ESP-IDF header that a target module includes gets a
 * tiny stand-in in this directory (esp_heap_caps.h, esp_log.h, esp_err.h).
 * Those stand-ins reduce to <stdlib.h> / <stdio.h> calls.  The shim
 * directory is added FIRST on the include path so the compiler picks
 * these up before any real ESP-IDF headers (which aren't on the host
 * path anyway, but belt-and-suspenders).
 *
 * NOT a general-purpose ESP-IDF emulator — only the surfaces used by
 * the modules we host-test live here.  Add new shims as new modules
 * join the suite. */
#pragma once
