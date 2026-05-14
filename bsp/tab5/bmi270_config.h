/**
 * @file bmi270_config.h
 * @brief Public symbol declarations for the Bosch BMI270 firmware config
 *        blob.  The actual array lives in bmi270_config.c — see that file
 *        for the SPDX-BSD-3-Clause license + attribution.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t bmi270_config_file[];
extern const size_t  bmi270_config_file_len;

#ifdef __cplusplus
}
#endif
