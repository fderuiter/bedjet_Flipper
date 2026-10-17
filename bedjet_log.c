// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_log.h"
#include <furi.h>
#include <furi_hal_rtc.h>
#include <storage/storage.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TAG              "BedJet"
#define LOG_DIR          APP_DATA_PATH("")
#define LOG_PATH         APP_DATA_PATH("session.log")
#define LOG_OLD_PATH     APP_DATA_PATH("session.1.log")
/* One previous file is kept; together at most about 512 KiB. */
#define LOG_ROTATE_BYTES (256u * 1024u)

static FuriMutex* log_mutex;
static Storage* log_storage;
static File* log_file;
static unsigned log_unsynced;

void bedjet_log_open(void) {
    if(log_mutex) return;
    log_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    log_storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(log_storage, LOG_DIR);
    FileInfo info;
    if(storage_common_stat(log_storage, LOG_PATH, &info) == FSE_OK &&
       info.size > LOG_ROTATE_BYTES) {
        storage_common_remove(log_storage, LOG_OLD_PATH);
        storage_common_rename(log_storage, LOG_PATH, LOG_OLD_PATH);
    }
    log_file = storage_file_alloc(log_storage);
    if(!storage_file_open(log_file, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FURI_LOG_W(TAG, "Session log unavailable");
        storage_file_free(log_file);
        log_file = NULL;
        return;
    }
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    bedjet_log(
        'I',
        "=== FredJet session %04u-%02u-%02u %02u:%02u:%02u ===",
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second);
}

void bedjet_log_close(void) {
    if(!log_mutex) return;
    furi_check(furi_mutex_acquire(log_mutex, FuriWaitForever) == FuriStatusOk);
    if(log_file) {
        storage_file_close(log_file);
        storage_file_free(log_file);
        log_file = NULL;
    }
    furi_record_close(RECORD_STORAGE);
    log_storage = NULL;
    furi_check(furi_mutex_release(log_mutex) == FuriStatusOk);
    furi_mutex_free(log_mutex);
    log_mutex = NULL;
}

void bedjet_log(char level, const char* format, ...) {
    char text[160];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if(level == 'E')
        FURI_LOG_E(TAG, "%s", text);
    else if(level == 'W')
        FURI_LOG_W(TAG, "%s", text);
    else
        FURI_LOG_I(TAG, "%s", text);
    if(!log_mutex) return;
    furi_check(furi_mutex_acquire(log_mutex, FuriWaitForever) == FuriStatusOk);
    if(log_file) {
        char line[180];
        int n = snprintf(
            line, sizeof(line), "%lu [%c] %s\n", (unsigned long)furi_get_tick(), level, text);
        if(n > 0)
            storage_file_write(
                log_file, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        /* Bound what a crash or a pulled battery can lose. */
        if(++log_unsynced >= 16) {
            storage_file_sync(log_file);
            log_unsynced = 0;
        }
    }
    furi_check(furi_mutex_release(log_mutex) == FuriStatusOk);
}
