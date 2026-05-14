/* Host shim for freertos/FreeRTOS.h — empty include so headers that
 * unconditionally #include "freertos/FreeRTOS.h" before semphr.h still
 * compile under tests/host/.  All real types live in semphr.h. */
#pragma once
