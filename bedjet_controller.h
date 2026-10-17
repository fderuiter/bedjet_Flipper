// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bedjet_protocol.h"
#include <stdatomic.h>

#define BEDJET_DEVICE_COUNT       6
#define BEDJET_QUEUE_CAPACITY     24
#define BEDJET_STATUS_FRESH_MS    15000u
#define BEDJET_BLE_TIMEOUT_MS     4000u
#define BEDJET_SCAN_MS            15000u
/* The BedJet 3 advertises at -21 dBm; at the edge of range a link needs a
 * longer wait (Home Assistant's integration allows 30 s). */
#define BEDJET_CONNECT_TIMEOUT_MS 20000u
#define BEDJET_COMMAND_TIMEOUT_MS 5000u
#define BEDJET_PRESET_SETTLE_MS   2000u
#define BEDJET_CLEANUP_ATTEMPTS   3u
#define BEDJET_REFRESH_MIN_MS     3000u
/* Off needs a deliberate hold; the firmware reports Long after only 300 ms. */
#define BEDJET_OFF_HOLD_MS        1000u
#define BEDJET_LONG_PRESS_MS      300u
#define BEDJET_INFO_PAGES         4u

typedef enum {
    BedjetCommandNone,
    BedjetCommandMode,
    BedjetCommandTemperature,
    BedjetCommandFan,
    BedjetCommandTimer,
    BedjetCommandMemory,
    BedjetCommandOff
} BedjetCommandKind;
typedef enum {
    BedjetCommandIdle,
    BedjetCommandQueued,
    BedjetCommandPending,
    BedjetCommandApplied,
    BedjetCommandRejected,
    BedjetCommandTimedOut,
    BedjetCommandConflict,
    BedjetCommandCancelled,
    BedjetCommandUnverified
} BedjetCommandState;
typedef struct {
    BedjetCommandKind kind;
    BedjetCommandState state;
    uint16_t desired, sent;
    uint32_t sent_ms;
    uint8_t sent_mode;
    char result[32];
} BedjetCommand;
/* One command awaits observation; temperature/fan repeats replace only its
 * desired target. Applied means a newer matching observation, not a device ACK.
 * Terminal failures discard the coalesced target and older raw inputs. */

typedef enum {
    Home,
    Scanning,
    Devices,
    Connecting,
    Control,
    Unsupported,
    Closing,
    Stopped
} BedjetScreen;
typedef enum {
    BedjetUp,
    BedjetDown,
    BedjetLeft,
    BedjetRight,
    BedjetOk,
    BedjetBack,
    BedjetOff,
    BedjetOffHint /* OK held past Long but released before Off: explain, do nothing */
} BedjetKey;
/* Hardware input event types, mirroring the Flipper input service. */
typedef enum {
    BedjetInputPress,
    BedjetInputRelease,
    BedjetInputShort,
    BedjetInputLong,
    BedjetInputRepeat
} BedjetInputType;
typedef enum {
    BedjetRowMode,
    BedjetRowTemperature,
    BedjetRowFan,
    BedjetRowTimer,
    BedjetRowM1,
    BedjetRowM2,
    BedjetRowM3,
    BedjetRowOff,
    BedjetRowUnits,
    BedjetRowInfo,
    BedjetRowCount
} BedjetRow;
/* HCI advertising report event types (pinned central.h). An active scan
 * reports the primary advertisement and its scan response separately. */
enum {
    BedjetAdvInd,
    BedjetAdvDirectInd,
    BedjetAdvScanInd,
    BedjetAdvNonConnInd,
    BedjetAdvScanRsp
};
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    uint8_t event_type;
    int8_t rssi;
    uint8_t data_len;
    uint8_t data[31];
} BedjetAdvertisement;

/* A discovered unit, merged from its advertisement and scan response. */
enum {
    BedjetDeviceService = 1, /* advertised the BedJet V3 service UUID */
    BedjetDeviceNamed = 2, /* local name starts with BEDJET */
    BedjetDeviceConnectable = 4, /* seen a connectable primary advertisement */
    BedjetDeviceCompleteName = 8
};
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint8_t flags;
    char name[21]; /* printable ASCII local name, if any */
} BedjetDevice;

/* Per-connection telemetry evidence for bring-up and fixture capture. Raw
 * frames are kept so a user can record what the unit actually sends; nothing
 * here is interpreted beyond the validated core status. */
typedef struct {
    uint32_t frames_ok, frames_rejected, frames_late, frames_oversize;
    uint32_t rejected_since_ok, reads_ok, reads_failed, last_frame_ms;
    uint8_t last_reason, read_result, ble_status;
    uint8_t rejected_len, accepted_len, read_len;
    uint8_t rejected[32], accepted[32], read[32];
} BedjetDiagnostics;

typedef struct {
    BedjetScreen screen;
    char message[64];
    BedjetDevice devices[BEDJET_DEVICE_COUNT];
    uint8_t count, selected, row, mode_choice;
    bool more_devices; /* a further matching unit did not fit the list */
    bool scan_all; /* advanced scan: list every device heard, not only BedJets */
    uint16_t timer_choice;
    bool connected, has_status, cleanup_deferred, limits_ready;
    bool celsius; /* display only; the protocol stays in half-degree C steps */
    uint8_t info_page;
    BedjetStatus status;
    BedjetCommand command;
    uint32_t status_ms;
    uint32_t dropped_events;
    BedjetDiagnostics diagnostics;
} BedjetModel;

typedef void (*BedjetScanSink)(void* context, const BedjetAdvertisement* advertisement);
typedef void (
    *BedjetNotifySink)(void* context, uint16_t handle, const uint8_t* bytes, size_t length);

/* All operations run on the consumer/worker, except now_ms (also used by
 * callbacks). Operations are synchronous with configured firmware wait budgets;
 * see docs/cancellation.md for native latency limits. A false
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
    uint32_t input_epoch;
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
    atomic_bool queue_busy, shutdown_requested, accepting_input, accepting_callbacks,
        control_ready;
    /* Session generation and priority bits share one atomic word. A producer
     * cannot accidentally attach an old request to a replacement session. */
    atomic_uint priority, dropped_events, input_epoch, oversize_frames;
    /* Diagnostics for the current scan, reset when it starts: reports that
     * matched a BedJet, were filtered out, or arrived while callbacks were
     * closed. Only counted; nothing reads them for control. */
    atomic_uint scan_matched, scan_filtered, scan_refused;
    /* Advanced scan: admit every report, keeping inbox headroom for BedJets. */
    atomic_bool scan_all;
    BedjetCallbackContext callback_context;
    /* Matching addresses seen in this scan, owned by the single scan producer
     * between scan start and a successful stop; lets a scan response for a
     * known unit through the producer-side filter. */
    struct {
        uint8_t addr[6], addr_type;
    } scan_seen[BEDJET_DEVICE_COUNT];
    uint8_t scan_seen_count;
    uint16_t status_handle, command_handle, cccd;
    uint32_t phase_started_ms;
    uint32_t limits_after_ms, refresh_ms;
    bool refreshed;
    uint8_t limits_mode, limits_from_mode;
    bool limits_waiting;
    unsigned cleanup_attempts;
    /* OK hold tracking, owned by the single GUI input thread. */
    uint32_t ok_press_ms;
    bool ok_down, ok_long, ok_off_sent;
    bool initialized, was_advertising, teardown_started, setup_failed;
} BedjetController;

/* Configure before registering GUI callbacks. No transport calls happen here.
 * step and model access are single-consumer; the GUI uses an adapter snapshot.
 * Posting is multi-producer, bounded and nonblocking. Contended/full ordinary
 * events are dropped and counted. Back/Off bypass that inbox; shutdown is a
 * global latch. Repeated priority requests coalesce. Off is accepted only when
 * controls are ready, invalidates older queued work, and is never retried or
 * replayed. Session tokens are opaque and captured when work originates. */
bool bedjet_controller_init(BedjetController*, const BedjetTransport*, void* context);
void bedjet_controller_step(BedjetController*);
bool bedjet_controller_post_key(BedjetController*, uint32_t session, BedjetKey);
/* Interpret one hardware input event from the GUI input thread: Back acts on
 * Press; Off needs OK held BEDJET_OFF_HOLD_MS on the control screen; a shorter
 * hold there does nothing but explain, while elsewhere a Long OK counts as OK;
 * Long and Repeat step the arrow keys. Returns whether anything was posted. */
bool bedjet_controller_input(BedjetController*, BedjetKey, BedjetInputType, uint32_t now_ms);
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
/* Manual resync: re-enable notifications and read the status characteristic
 * once. Never writes a command; rate-limited to one per BEDJET_REFRESH_MIN_MS. */
bool bedjet_controller_refresh(BedjetController*, uint32_t session);
void bedjet_controller_shutdown(BedjetController*);
uint32_t bedjet_controller_session(const BedjetController*);
bool bedjet_controller_fresh(const BedjetModel*, uint32_t now_ms);
