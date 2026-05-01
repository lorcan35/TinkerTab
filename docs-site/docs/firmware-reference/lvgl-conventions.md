---
title: LVGL conventions
sidebar_label: LVGL conventions
---

# LVGL conventions

:::info Coming soon
All LVGL config lives in `sdkconfig.defaults`, NOT `lv_conf.h` (the ESP-IDF component sets `LV_CONF_SKIP=1`). Always wrap `lv_async_call` with `tab5_lv_async_call(cb, arg)` — the LVGL primitive is not thread-safe in 9.x.

This page is a stub. Real content lands in PR 2 (user-facing) or PR 3 (developer-facing) per the docs-site plan.
:::
