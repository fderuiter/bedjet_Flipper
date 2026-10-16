// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bedjet_protocol.h"
#include <stdatomic.h>

#define BEDJET_DEVICE_COUNT    6
#define BEDJET_QUEUE_CAPACITY  24
#define BEDJET_STATUS_FRESH_MS 15000u
#define BEDJET_BLE_TIMEOUT_MS  4000u

typedef enum {
    Home,
    Scanning,
    Devices,
    Connecting,
    Control,
    Unsupported,
    Stopped
} BedjetScreen;
typedef enum {
    BedjetUp,
    BedjetDown,
    BedjetLeft,
    BedjetRight,
    BedjetOk,
    BedjetBack
} BedjetKey;
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint8_t data_len;
    uint8_t data[31];
} BedjetAdvertisement;

typedef struct {
    BedjetScreen screen;
    char message[64];
    BedjetAdvertisement devices[BEDJET_DEVICE_COUNT];
    uint8_t count, selected, row, mode_choice;
    uint16_t timer_choice;
    bool connected, has_status, cleanup_deferred;
    BedjetStatus status;
    uint32_t status_ms;
    uint32_t dropped_events;
} BedjetModel;

typedef void (*BedjetScanSink)(void* context, const BedjetAdvertisement* advertisement);
typedef void (
    *BedjetNotifySink)(void* context, uint16_t handle, const uint8_t* bytes, size_t length);

/* All operations run on the consumer/worker, except now_ms (also used by
 * callbacks). Controller operations are bounded synchronous calls. A false
 * deinit result still MUST detach and drain both callback types before return,
 * as guaranteed by the pinned firmware API. set_notify(NULL) also drains any
 * in-flight invocation. scan_stop drains its callback only on success.
 * The driver owns its asynchronous Bluetooth state; this is not a BLE stack. */
typedef struct {
    bool (*supported)(void*);
    void (*init)(void*);
    bool (*is_advertising)(void*);
    void (*advertise)(void*, bool enabled);
    bool (*scan_start)(void*, BedjetScanSink, void*);
    bool (*scan_stop)(void*);
    bool (*is_scanning)(void*);
    bool (*connect)(void*, const uint8_t addr[6], uint8_t type, uint32_t timeout_ms);
    bool (*connected)(void*);
    bool (*disconnect)(void*);
    bool (*find)(
        void*,
        const uint8_t uuid[16],
        uint16_t* handle,
        uint16_t* cccd,
        uint32_t timeout_ms);
    void (*set_notify)(void*, BedjetNotifySink, void*);
    bool (*subscribe)(void*, uint16_t cccd, uint32_t timeout_ms);
    bool (*read)(
        void*,
        uint16_t handle,
        uint8_t* bytes,
        uint8_t capacity,
        uint8_t* length,
        uint32_t timeout_ms);
    bool (
        *write)(void*, uint16_t handle, const uint8_t* bytes, uint8_t length, uint32_t timeout_ms);
    bool (*deinit)(void*);
    uint8_t (*last_status)(void*);
    uint32_t (*now_ms)(void*);
} BedjetTransport;

typedef enum {
    BedjetBoot,
    BedjetIdle,
    BedjetPauseAdvertising,
    BedjetScan,
    BedjetConnect,
    BedjetFindCommand,
    BedjetFindStatus,
    BedjetSubscribe,
    BedjetInitialRead,
    BedjetReady,
    BedjetDisconnecting,
    BedjetStopping,
    BedjetDone
} BedjetPhase;
typedef enum {
    BedjetEventKey,
    BedjetEventAdvertisement,
    BedjetEventStatus,
    BedjetEventRefresh
} BedjetEventType;
typedef struct {
    BedjetEventType type;
    uint32_t session;
    uint32_t received_ms;
    union {
        BedjetKey key;
        BedjetAdvertisement advertisement;
        struct {
            uint16_t handle;
            uint8_t length;
            uint8_t bytes[32];
        } status;
    } value;
} BedjetEvent;

struct BedjetController;
typedef struct {
    struct BedjetController* controller;
    uint32_t session;
} BedjetCallbackContext;
typedef struct BedjetController {
    BedjetTransport transport;
    void* transport_context;
    BedjetModel model;
    BedjetPhase phase;
    BedjetEvent queue[BEDJET_QUEUE_CAPACITY];
    size_t queue_head, queue_count;
    atomic_bool queue_busy, back_requested, shutdown_requested, accepting_input,
        accepting_callbacks;
    atomic_uint session, dropped_events;
    BedjetCallbackContext callback_context;
    uint16_t status_handle, command_handle, cccd;
    uint32_t phase_started_ms;
    unsigned cleanup_attempts;
    bool initialized, was_advertising, teardown_started, setup_failed;
} BedjetController;

/* Configure before registering GUI callbacks. No transport calls happen here.
 * step and model access are single-consumer; the GUI uses an adapter snapshot.
 * Posting is multi-producer, bounded and nonblocking. Contended/full ordinary
 * events are dropped and counted. Back/shutdown use independent atomic latches.
 * session tokens must be captured when input/callback work originates. */
bool bedjet_controller_init(BedjetController*, const BedjetTransport*, void* context);
void bedjet_controller_step(BedjetController*);
bool bedjet_controller_post_key(BedjetController*, uint32_t session, BedjetKey);
bool bedjet_controller_post_advertisement(
    BedjetController*,
    uint32_t session,
    const BedjetAdvertisement*);
bool bedjet_controller_post_status(
    BedjetController*,
    uint32_t session,
    uint16_t handle,
    const uint8_t*,
    size_t length,
    uint32_t received_ms);
bool bedjet_controller_refresh(BedjetController*, uint32_t session);
void bedjet_controller_shutdown(BedjetController*);
uint32_t bedjet_controller_session(const BedjetController*);
bool bedjet_controller_fresh(const BedjetModel*, uint32_t now_ms);
