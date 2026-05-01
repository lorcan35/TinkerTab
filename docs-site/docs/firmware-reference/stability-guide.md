---
title: Stability guide
sidebar_label: Stability guide
---

# Stability guide

:::info Coming soon
Three rules. (1) BSS-static caches >1.8 KB push the boot timer task over its SRAM canary → MCAUSE 0x1b; default to PSRAM-lazy. (2) Never `xTimerCreate` from boot — use `esp_timer`. (3) Hide/show overlays, don't destroy/create — internal SRAM fragments otherwise.

This page is a stub. Real content lands in PR 2 (user-facing) or PR 3 (developer-facing) per the docs-site plan.
:::
