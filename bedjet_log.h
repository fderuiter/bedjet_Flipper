// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

/* Session log on the SD card (apps_data/fredjet/session.log) so sessions run
 * away from a computer can be read later; every line also goes to the Flipper
 * log. Addresses are logged by their last two bytes only. Thread-safe; call
 * only from the worker or main thread, never from BLE callbacks. */
void bedjet_log_open(void);
void bedjet_log_close(void);
void bedjet_log(char level, const char* format, ...) __attribute__((format(printf, 2, 3)));

#define BJ_LOG_I(...) bedjet_log('I', __VA_ARGS__)
#define BJ_LOG_W(...) bedjet_log('W', __VA_ARGS__)
#define BJ_LOG_E(...) bedjet_log('E', __VA_ARGS__)
