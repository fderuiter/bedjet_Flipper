// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_flipper.h"
#include <furi.h>
#include <furi_hal.h>
#include <furi_ble/central.h>
#include <stdio.h>
#include <string.h>

#define TAG "BedJet"

static uint32_t time_ms(void* context) {
    UNUSED(context);
    return (uint32_t)((uint64_t)furi_get_tick() * 1000u / furi_kernel_get_tick_frequency());
}
static bool contains(const uint8_t* data, size_t len, const uint8_t* needle, size_t n) {
    for(size_t i = 0; i + n <= len; i++)
        if(!memcmp(data + i, needle, n)) return true;
    return false;
}
static void scan_callback(const BleCentralAdvReport* report, void* context) {
    BedjetFlipper* driver = context;
    if(!driver->scan) return;
    driver->scan_reports++;
    if(report->event_type == BedjetAdvScanRsp) driver->scan_responses++;
    if(report->data_len > 31) {
        driver->scan_oversized++;
        return;
    }
    if(contains(report->data, report->data_len, (const uint8_t*)"BEDJET", 6) ||
       contains(report->data, report->data_len, bedjet_service_uuid, 16)) {
        if(!driver->scan_bedjet_raw++) {
            driver->scan_first_type = report->event_type;
            driver->scan_first_len = report->data_len;
            memcpy(driver->scan_first, report->data, report->data_len);
        }
    }
    BedjetAdvertisement a = {
        .addr_type = report->addr_type,
        .event_type = report->event_type,
        .rssi = report->rssi,
        .data_len = report->data_len};
    memcpy(a.addr, report->addr, sizeof(a.addr));
    memcpy(a.data, report->data, a.data_len);
    driver->scan(driver->scan_context, &a);
}
static void
    notify_callback(uint16_t handle, const uint8_t* bytes, uint16_t length, void* context) {
    BedjetFlipper* driver = context;
    if(driver->notify) driver->notify(driver->notify_context, handle, bytes, length);
}
static bool supported(void* context) {
    UNUSED(context);
    return furi_hal_bt_is_central_supported();
}
static void initialize(void* context) {
    UNUSED(context);
    ble_central_init();
}
static bool advertising(void* context) {
    UNUSED(context);
    return furi_hal_bt_is_advertising();
}
static void advertise(void* context, bool enabled) {
    UNUSED(context);
    if(enabled)
        furi_hal_bt_start_advertising();
    else
        furi_hal_bt_stop_advertising();
}
static bool scan_start(void* context, BedjetScanSink sink, void* sink_context) {
    BedjetFlipper* driver = context;
    driver->scan = sink;
    driver->scan_context = sink_context;
    driver->scan_reports = 0;
    driver->scan_responses = 0;
    driver->scan_oversized = 0;
    driver->scan_bedjet_raw = 0;
    return ble_central_scan_start(scan_callback, driver);
}
static bool scan_stop(void* context) {
    BedjetFlipper* driver = context;
    bool was_scanning = driver->scan != NULL;
    bool ok = ble_central_scan_stop();
    if(ok && was_scanning) {
        /* Unfiltered totals tell "radio heard nothing" from "app filtered all". */
        FURI_LOG_I(
            TAG,
            "Scan heard %lu reports (%lu scan responses, %lu over 31 bytes)",
            (unsigned long)driver->scan_reports,
            (unsigned long)driver->scan_responses,
            (unsigned long)driver->scan_oversized);
        if(driver->scan_bedjet_raw) {
            char hex[31 * 2 + 1];
            for(uint8_t i = 0; i < driver->scan_first_len; i++)
                snprintf(hex + i * 2, 3, "%02X", driver->scan_first[i]);
            hex[driver->scan_first_len * 2] = '\0';
            FURI_LOG_I(
                TAG,
                "Scan raw BedJet matches %lu; first type=%u len=%u: %s",
                (unsigned long)driver->scan_bedjet_raw,
                driver->scan_first_type,
                driver->scan_first_len,
                hex);
        } else {
            FURI_LOG_I(TAG, "Scan raw BedJet matches 0");
        }
    }
    if(ok) {
        driver->scan = NULL;
        driver->scan_context = NULL;
    }
    return ok;
}
static bool scanning(void* context) {
    UNUSED(context);
    return ble_central_is_scanning();
}
static bool connect_peer(void* context, const uint8_t addr[6], uint8_t type, uint32_t timeout_ms) {
    UNUSED(context);
    return ble_central_connect(addr, type, timeout_ms);
}
static bool connected(void* context) {
    UNUSED(context);
    return ble_central_is_connected();
}
static bool disconnect_peer(void* context) {
    UNUSED(context);
    return ble_central_disconnect();
}
static bool find(
    void* context,
    const uint8_t uuid[16],
    uint16_t* handle,
    uint16_t* cccd,
    uint32_t timeout_ms) {
    UNUSED(context);
    return ble_central_find_characteristic_128(
        bedjet_service_uuid, uuid, handle, cccd, timeout_ms);
}
static void set_notify(void* context, BedjetNotifySink sink, void* sink_context) {
    BedjetFlipper* driver = context;
    /* Clear/drain first: never mutate the adapter's function/context while an
     * old BLE callback may still be reading them. */
    ble_central_set_notify_callback(NULL, NULL);
    driver->notify = sink;
    driver->notify_context = sink_context;
    if(sink) ble_central_set_notify_callback(notify_callback, driver);
}
static bool subscribe(void* context, uint16_t cccd, uint32_t timeout_ms) {
    UNUSED(context);
    return ble_central_subscribe(cccd, timeout_ms);
}
static bool read_status(
    void* context,
    uint16_t handle,
    uint8_t* bytes,
    uint8_t capacity,
    uint8_t* length,
    uint32_t timeout_ms) {
    UNUSED(context);
    return ble_central_read(handle, bytes, capacity, length, timeout_ms);
}
static bool write_command(
    void* context,
    uint16_t handle,
    const uint8_t* bytes,
    uint8_t length,
    uint32_t timeout_ms) {
    UNUSED(context);
    return ble_central_write(handle, bytes, length, false, timeout_ms);
}
static bool deinitialize(void* context) {
    BedjetFlipper* driver = context;
    bool done = ble_central_deinit();
    /* The pinned API drains/detaches callbacks even on deferred deinit. */
    driver->scan = NULL;
    driver->scan_context = NULL;
    driver->notify = NULL;
    driver->notify_context = NULL;
    return done;
}
static uint8_t last_status(void* context) {
    UNUSED(context);
    return ble_central_last_status();
}

BedjetTransport bedjet_flipper_transport(void) {
    return (BedjetTransport){
        .supported = supported,
        .init = initialize,
        .is_advertising = advertising,
        .advertise = advertise,
        .scan_start = scan_start,
        .scan_stop = scan_stop,
        .is_scanning = scanning,
        .connect = connect_peer,
        .connected = connected,
        .disconnect = disconnect_peer,
        .find = find,
        .set_notify = set_notify,
        .subscribe = subscribe,
        .read = read_status,
        .write = write_command,
        .deinit = deinitialize,
        .last_status = last_status,
        .now_ms = time_ms,
    };
}
