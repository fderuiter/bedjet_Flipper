// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_flipper.h"
#include "bedjet_log.h"
#include <furi.h>
#include <furi_hal.h>
#include <furi_ble/central.h>
#include <toolbox/version.h>
#include <stdio.h>
#include <string.h>

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
    driver->notifications++;
    if(driver->notify) driver->notify(driver->notify_context, handle, bytes, length);
}

/* Diagnostics: every transport call logs its arguments, result, duration and
 * the firmware's last controller status; polled queries log only changes.
 * Addresses show their last two bytes, as on screen. Worker thread only. */
static const char* uuid_name(const uint8_t uuid[16]) {
    if(!memcmp(uuid, bedjet_status_uuid, 16)) return "status";
    if(!memcmp(uuid, bedjet_command_uuid, 16)) return "command";
    if(!memcmp(uuid, bedjet_service_uuid, 16)) return "service";
    return "other";
}
static void log_call(BedjetFlipper* driver, const char* what, bool ok, uint32_t started) {
    uint8_t status = ble_central_last_status();
    uint32_t took = time_ms(NULL) - started;
    if(ok)
        BJ_LOG_I("BLE %s: ok in %lums (status 0x%02X)", what, (unsigned long)took, status);
    else
        BJ_LOG_E(
            "BLE %s: FAILED after %lums, status 0x%02X, scanning=%d connected=%d advertising=%d",
            what,
            (unsigned long)took,
            status,
            ble_central_is_scanning(),
            ble_central_is_connected(),
            furi_hal_bt_is_advertising());
    driver->seen_status = status;
}
static void log_hex(const char* label, const uint8_t* bytes, size_t length) {
    char hex[3 * 32 + 1];
    size_t used = 0;
    hex[0] = '\0';
    for(size_t i = 0; i < length && i < 32; i++)
        used += snprintf(hex + used, sizeof(hex) - used, "%02X ", bytes[i]);
    BJ_LOG_I("%s len=%u: %s", label, (unsigned)length, hex);
}

static bool supported(void* context) {
    UNUSED(context);
    const Version* v = furi_hal_version_get_firmware_version();
    FuriHalBtStack stack = furi_hal_bt_get_radio_stack();
    bool central = furi_hal_bt_is_central_supported();
    BJ_LOG_I(
        "Firmware %s %s (%s%s), target f%u",
        version_get_version(v),
        version_get_githash(v),
        version_get_gitbranch(v),
        version_get_dirty_flag(v) ? ", dirty" : "",
        version_get_target(v));
    BJ_LOG_I(
        "Radio stack %s, central %s, free heap %u",
        stack == FuriHalBtStackFull  ? "Full" :
        stack == FuriHalBtStackLight ? "Light" :
                                       "unknown",
        central ? "supported" : "NOT supported",
        (unsigned)memmgr_get_free_heap());
    return central;
}
static void initialize(void* context) {
    BedjetFlipper* driver = context;
    uint32_t started = time_ms(NULL);
    ble_central_init();
    driver->seen_advertising = furi_hal_bt_is_advertising();
    driver->seen_scanning = ble_central_is_scanning();
    driver->seen_connected = ble_central_is_connected();
    BJ_LOG_I(
        "BLE init in %lums: advertising=%d scanning=%d connected=%d",
        (unsigned long)(time_ms(NULL) - started),
        driver->seen_advertising,
        driver->seen_scanning,
        driver->seen_connected);
}
static bool advertising(void* context) {
    BedjetFlipper* driver = context;
    bool now = furi_hal_bt_is_advertising();
    if(now != driver->seen_advertising) BJ_LOG_I("Flipper advertising now %s", now ? "on" : "off");
    driver->seen_advertising = now;
    return now;
}
static void advertise(void* context, bool enabled) {
    UNUSED(context);
    BJ_LOG_I("%s Flipper advertising", enabled ? "Restoring" : "Pausing");
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
    uint32_t started = time_ms(NULL);
    bool ok = ble_central_scan_start(scan_callback, driver);
    log_call(driver, "scan start", ok, started);
    return ok;
}
static bool scan_stop(void* context) {
    BedjetFlipper* driver = context;
    bool was_scanning = driver->scan != NULL;
    uint32_t started = time_ms(NULL);
    bool ok = ble_central_scan_stop();
    log_call(driver, "scan stop", ok, started);
    if(ok && was_scanning) {
        /* Unfiltered totals tell "radio heard nothing" from "app filtered all". */
        BJ_LOG_I(
            "Scan heard %lu reports (%lu scan responses, %lu over 31 bytes)",
            (unsigned long)driver->scan_reports,
            (unsigned long)driver->scan_responses,
            (unsigned long)driver->scan_oversized);
        if(driver->scan_bedjet_raw) {
            char hex[31 * 2 + 1];
            for(uint8_t i = 0; i < driver->scan_first_len; i++)
                snprintf(hex + i * 2, 3, "%02X", driver->scan_first[i]);
            hex[driver->scan_first_len * 2] = '\0';
            BJ_LOG_I(
                "Scan raw BedJet matches %lu; first type=%u len=%u: %s",
                (unsigned long)driver->scan_bedjet_raw,
                driver->scan_first_type,
                driver->scan_first_len,
                hex);
        } else {
            BJ_LOG_I("Scan raw BedJet matches 0");
        }
    }
    if(ok) {
        driver->scan = NULL;
        driver->scan_context = NULL;
    }
    return ok;
}
static bool scanning(void* context) {
    BedjetFlipper* driver = context;
    bool now = ble_central_is_scanning();
    if(now != driver->seen_scanning) BJ_LOG_I("Scanning now %s", now ? "on" : "off");
    driver->seen_scanning = now;
    return now;
}
static bool connect_peer(void* context, const uint8_t addr[6], uint8_t type, uint32_t timeout_ms) {
    BedjetFlipper* driver = context;
    BJ_LOG_I(
        "Connecting to ..%02X:%02X (address type %u), timeout %lums",
        addr[1],
        addr[0],
        type,
        (unsigned long)timeout_ms);
    uint32_t started = time_ms(NULL);
    bool ok = ble_central_connect(addr, type, timeout_ms);
    log_call(driver, "connect", ok, started);
    if(ok) {
        driver->connected_ms = time_ms(NULL);
        driver->notifications = 0;
        driver->seen_connected = true;
        BJ_LOG_I("Connection handle 0x%04X", ble_central_connection_handle());
    }
    return ok;
}
static bool connected(void* context) {
    BedjetFlipper* driver = context;
    bool now = ble_central_is_connected();
    if(now != driver->seen_connected) {
        if(now)
            BJ_LOG_I("Link up");
        else
            BJ_LOG_W(
                "Link down after %lums, %lu notifications, last status 0x%02X",
                (unsigned long)(time_ms(NULL) - driver->connected_ms),
                (unsigned long)driver->notifications,
                ble_central_last_status());
    }
    driver->seen_connected = now;
    return now;
}
static bool disconnect_peer(void* context) {
    BedjetFlipper* driver = context;
    BJ_LOG_I(
        "Disconnecting after %lums, %lu notifications",
        (unsigned long)(time_ms(NULL) - driver->connected_ms),
        (unsigned long)driver->notifications);
    uint32_t started = time_ms(NULL);
    bool ok = ble_central_disconnect();
    log_call(driver, "disconnect", ok, started);
    if(ok) driver->seen_connected = ble_central_is_connected();
    return ok;
}
static bool find(
    void* context,
    const uint8_t uuid[16],
    uint16_t* handle,
    uint16_t* cccd,
    uint32_t timeout_ms) {
    BedjetFlipper* driver = context;
    uint32_t started = time_ms(NULL);
    bool ok =
        ble_central_find_characteristic_128(bedjet_service_uuid, uuid, handle, cccd, timeout_ms);
    char what[48];
    snprintf(what, sizeof(what), "find %s characteristic", uuid_name(uuid));
    log_call(driver, what, ok, started);
    if(ok)
        BJ_LOG_I(
            "%s characteristic: value handle 0x%04X, CCCD 0x%04X",
            uuid_name(uuid),
            handle ? *handle : 0,
            cccd ? *cccd : 0);
    return ok;
}
static void set_notify(void* context, BedjetNotifySink sink, void* sink_context) {
    BedjetFlipper* driver = context;
    /* Clear/drain first: never mutate the adapter's function/context while an
     * old BLE callback may still be reading them. */
    ble_central_set_notify_callback(NULL, NULL);
    driver->notify = sink;
    driver->notify_context = sink_context;
    if(sink) ble_central_set_notify_callback(notify_callback, driver);
    BJ_LOG_I("Notifications %s", sink ? "routed to the app" : "detached");
}
static bool subscribe(void* context, uint16_t cccd, uint32_t timeout_ms) {
    BedjetFlipper* driver = context;
    uint32_t started = time_ms(NULL);
    bool ok = ble_central_subscribe(cccd, timeout_ms);
    char what[32];
    snprintf(what, sizeof(what), "subscribe CCCD 0x%04X", cccd);
    log_call(driver, what, ok, started);
    return ok;
}
static bool read_status(
    void* context,
    uint16_t handle,
    uint8_t* bytes,
    uint8_t capacity,
    uint8_t* length,
    uint32_t timeout_ms) {
    BedjetFlipper* driver = context;
    uint32_t started = time_ms(NULL);
    bool ok = ble_central_read(handle, bytes, capacity, length, timeout_ms);
    char what[32];
    snprintf(what, sizeof(what), "read 0x%04X", handle);
    log_call(driver, what, ok, started);
    if(ok && length) log_hex("BLE read data", bytes, *length);
    return ok;
}
static bool write_command(
    void* context,
    uint16_t handle,
    const uint8_t* bytes,
    uint8_t length,
    uint32_t timeout_ms) {
    BedjetFlipper* driver = context;
    log_hex("BLE write data", bytes, length);
    uint32_t started = time_ms(NULL);
    bool ok = ble_central_write(handle, bytes, length, false, timeout_ms);
    char what[32];
    snprintf(what, sizeof(what), "write 0x%04X", handle);
    log_call(driver, what, ok, started);
    return ok;
}
static bool deinitialize(void* context) {
    BedjetFlipper* driver = context;
    uint32_t started = time_ms(NULL);
    bool done = ble_central_deinit();
    log_call(driver, "deinit", done, started);
    /* The pinned API drains/detaches callbacks even on deferred deinit. */
    driver->scan = NULL;
    driver->scan_context = NULL;
    driver->notify = NULL;
    driver->notify_context = NULL;
    return done;
}
static uint8_t last_status(void* context) {
    BedjetFlipper* driver = context;
    uint8_t status = ble_central_last_status();
    if(status != driver->seen_status) BJ_LOG_I("BLE last status now 0x%02X", status);
    driver->seen_status = status;
    return status;
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
