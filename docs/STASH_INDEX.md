# Stash Index

## stash@{0}
**Saved:** 20260331-132117
**Branch:** main (before rollback)
**Contains:** Voice and chat UI improvements before creating the rollback branch.
Files:
- `main/main.c`
- `main/ui_chat.c`
- `main/ui_chat.h`
- `main/ui_home.c`
- `main/voice.c`
- `main/voice.h`
- `main/CMakeLists.txt`
- `main/lv_conf.h`
**Status:** experimental — uncommitted work before rollback, preserved for reference

## stash@{1}
**Saved:** WIP on feat/linux-hal-53 (commit 3c1e638)
**Branch:** feat/linux-hal-53
**Contains:** Wokwi project files and sim assessment (refs #52). Large removal: SDIO driver and os_wrapper removed.
Files:
- `main/CMakeLists.txt`
- `main/lv_conf.h`
- `main/ui_chat.c`
- `main/ui_home.c`
- `managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c`
- `managed_components/espressif__esp_hosted/host/port/include/os_wrapper.h`
- `sdkconfig.defaults`
**Status:** work-in-progress — Linux HAL feature branch, likely superseded

## stash@{2}
**Saved:** WIP on feat/chat-ui (commit c5dd819)
**Branch:** feat/chat-ui
**Contains:** Dedicated chat screen with text and voice input (closes #48). Large refactor of ui_chat.c.
Files:
- `main/ui_chat.c`
- `main/ui_core.c`
**Status:** work-in-progress — chat UI feature, likely superseded

## stash@{3}
**Saved:** WIP on main (commit b1d8949)
**Branch:** main
**Contains:** FPS counter fix — counts frames at SOI detection instead of decode (closes #17).
Files:
- `main/debug_server.c`
**Status:** work-in-progress — debug/FPS fix on main, may conflict with current state

## stash@{4}
**Saved:** WIP on feat/udp-jpeg-stream (commit 1c63f11)
**Branch:** feat/udp-jpeg-stream
**Contains:** UDP JPEG receiver with HW decode for Dragon streaming (refs #12). WiFi UI updates.
Files:
- `main/CMakeLists.txt`
- `main/ui_wifi.c`
**Status:** work-in-progress — UDP JPEG streaming feature

## stash@{5}
**Saved:** WIP on feat/udp-jpeg-stream (commit 1c63f11)
**Branch:** feat/udp-jpeg-stream
**Contains:** Same as stash@{4} (same commit). Minor follow-up change to ui_wifi.c.
Files:
- `main/ui_wifi.c`
**Status:** work-in-progress — minor follow-up to UDP JPEG streaming

---

## Summary
| Stash | Branch | Topic | Status |
|-------|--------|-------|--------|
| stash@{0} | main (pre-rollback) | Voice + chat UI improvements | Reference only |
| stash@{1} | feat/linux-hal-53 | Wokwi sim files + SDIO removal | Likely superseded |
| stash@{2} | feat/chat-ui | Dedicated chat screen (closes #48) | Likely superseded |
| stash@{3} | main | FPS counter at SOI detection fix (closes #17) | May be valuable |
| stash@{4} | feat/udp-jpeg-stream | UDP JPEG receiver + WiFi UI (refs #12) | Work-in-progress |
| stash@{5} | feat/udp-jpeg-stream | Minor WiFi UI tweak (same branch as stash@{4}) | Work-in-progress |
