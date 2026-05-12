# 0001 — Host-test infra uses plain `<assert.h>` + tiny ESP-IDF shims

- **Status:** Accepted
- **Date:** 2026-05-12
- **Deciders:** lorcan35
- **Tags:** testing / build / tab5

## Context

W9-A from the 2026-05-11 cross-stack audit asked for a "CMocka host
CMake target" so the pure-logic Tab5 modules (`md_strip`,
`openrouter_sse`, `spring_anim`, etc.) could be unit-tested off-device.
CMocka was the audit's first guess because it's the canonical
ESP-IDF unit-test framework — but ESP-IDF only ships it for on-target
runs, not for host builds.  Bringing CMocka onto the host means either
an apt dependency (`libcmocka-dev`) or vendoring it into the tree.

Several modules (everything except `md_strip`) include ESP-IDF
headers (`esp_heap_caps.h`, `esp_log.h`, `esp_err.h`) for tiny
surfaces — `heap_caps_malloc` aliasing into `malloc`, `ESP_LOGW`
formatting to stderr, the `esp_err_t` enum.  A full ESP-IDF host
build is wildly out of scope; a minimal stand-in is plenty.

## Decision

Host tests use:

1. **Plain `<assert.h>` + a hand-rolled per-file driver** — no CMocka
   dependency at all.  Each `tests/host/test_*.c` is its own
   executable that returns non-zero on first failed assertion and
   prints `file:line` of the failure.
2. **Tiny shim headers in `tests/host/shim/`** — `esp_heap_caps.h`,
   `esp_log.h`, `esp_err.h` reduced to the surface the tested modules
   actually touch.  Shim directory is added first on the include path
   so the compiler picks these up before any real ESP-IDF headers
   (which aren't on the host path anyway, but belt-and-suspenders).
3. **One CMake target per module** in `tests/host/CMakeLists.txt`,
   wired into `ctest` via `add_test`.
4. **CI runs via a new `host-tests` workflow** that does
   `cmake -S tests/host && cmake --build && ctest` — sub-7 s gate.

## Consequences

- **Easier:** CI image stays minimal (no apt-install), test driver
  is portable to any C compiler, adding a new module to the suite is
  one CMake entry + one test file.  Sub-second test runtime.
- **Harder:** No mocking framework.  When a module needs mock state
  (Dragon HTTP responses, NVS reads), we hand-roll capture-by-context
  patterns in the test driver.  Acceptable for the modules in scope;
  if a future module needs real mocking we re-open the CMocka
  question.
- **Off the table without a new ADR:** dragging Google Test, Unity,
  or CMocka into the build.  Adding a host-test for a module that
  needs more than 3–4 ESP-IDF surface shims (those should grow the
  shim directory; if the surface keeps expanding the module is too
  ESP-IDF-coupled to host-test).

## Alternatives considered

- **CMocka via apt** — works but adds a CI dependency for a
  rebuilt-from-scratch image; CMocka's `_test_setup`/`expect_value`
  features aren't actually needed by any module in scope.
- **Google Test (C++)** — would force a C++ link step and a heavier
  shim layer.  Tab5 firmware is pure C; the test infra should
  match.
- **Unity** — closer to the right shape but still a vendor decision
  for ~30 assertions per module.  Plain `<assert.h>` is enough.
- **Run tests on the ESP-IDF host target** — too much setup for a
  feature we want every PR to gate on.  Sub-second matters.
