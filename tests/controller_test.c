// SPDX-License-Identifier: GPL-3.0-or-later
#include "../bedjet_controller.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    InitOp,
    ScanStartOp,
    ScanStopOp,
    ConnectOp,
    CommandOp,
    StatusOp,
    SubscribeOp,
    ReadOp,
    WriteOp,
    DisconnectOp,
    DetachOp,
    DeinitOp,
    OpCount
} Operation;
typedef struct {
    BedjetController* controller;
    atomic_uint clock;
    bool supported, advertising, scanning, link, fail[OpCount], no_cccd, short_read;
    bool refuse_advertising_stop;
    unsigned calls[OpCount], deinit_failures, restored;
    unsigned write_count;
    uint8_t writes[64][3], write_lengths[64];
    Operation cancel_during;
    bool cancel_enabled, cancel_shutdown;
    bool notify_during_read, notify_during_write, notify_during_remove, notify_during_subscribe;
    bool scan_during_deinit;
    BedjetScanSink scan;
    void* scan_context;
    BedjetNotifySink notify;
    void* notify_context;
    uint8_t read_packet[32], notification[32];
} Fake;

static const uint8_t packet[32] = {0, 0x56, 32, 1, 2, 30, 15, 50, 60, 1, 9, 4, 0, 38, 86, 0, 0, 42};
static uint32_t clock_ms(void* context) {
    return atomic_load(&((Fake*)context)->clock);
}
static void advance(Fake* f, uint32_t ms) {
    atomic_fetch_add(&f->clock, ms);
}
static bool operation(Fake* f, Operation op, uint32_t timeout) {
    f->calls[op]++;
    if(f->cancel_enabled && f->cancel_during == op) {
        f->cancel_enabled = false;
        if(f->cancel_shutdown)
            bedjet_controller_shutdown(f->controller);
        else
            assert(bedjet_controller_post_key(
                f->controller, bedjet_controller_session(f->controller), BedjetBack));
    }
    if(f->fail[op]) {
        advance(f, timeout);
        return false;
    }
    return true;
}
static void emit_status(Fake* f) {
    if(f->notify) f->notify(f->notify_context, 13, f->notification, sizeof(f->notification));
}
static BedjetAdvertisement advertisement(unsigned address) {
    BedjetAdvertisement a = {
        .addr = {0, 1, 2, 3, 4, 5},
        .addr_type = 1,
        .rssi = -50,
        .data_len = 8,
        .data = {7, 9, 'B', 'e', 'd', 'J', 'e', 't'}};
    a.addr[0] = address;
    return a;
}
static void emit_advertisement(Fake* f, const BedjetAdvertisement* a) {
    if(f->scan) f->scan(f->scan_context, a);
}
static bool supported(void* context) {
    return ((Fake*)context)->supported;
}
static void initialize(void* context) {
    operation(context, InitOp, 0);
}
static bool advertising(void* context) {
    return ((Fake*)context)->advertising;
}
static void advertise(void* context, bool enabled) {
    Fake* f = context;
    if(enabled) {
        f->advertising = true;
        f->restored++;
    } else if(!f->refuse_advertising_stop)
        f->advertising = false;
}
static bool fake_start_scan(void* context, BedjetScanSink sink, void* sink_context) {
    Fake* f = context;
    if(!operation(f, ScanStartOp, 0)) return false;
    assert(!f->advertising);
    f->scanning = true;
    f->scan = sink;
    f->scan_context = sink_context;
    return true;
}
static bool stop_scan(void* context) {
    Fake* f = context;
    if(!operation(f, ScanStopOp, BEDJET_BLE_TIMEOUT_MS)) return false;
    f->scanning = false;
    f->scan = NULL;
    f->scan_context = NULL;
    return true;
}
static bool scanning(void* context) {
    return ((Fake*)context)->scanning;
}
static bool connect_peer(void* context, const uint8_t addr[6], uint8_t type, uint32_t timeout) {
    Fake* f = context;
    assert(addr[5] == 5 && type == 1 && timeout == 8000);
    assert(!f->scanning);
    f->link = operation(f, ConnectOp, timeout);
    return f->link;
}
static bool connected(void* context) {
    return ((Fake*)context)->link;
}
static bool disconnect_peer(void* context) {
    Fake* f = context;
    if(!operation(f, DisconnectOp, BEDJET_BLE_TIMEOUT_MS)) return false;
    f->link = false;
    return true;
}
static bool
    find(void* context, const uint8_t uuid[16], uint16_t* handle, uint16_t* cccd, uint32_t timeout) {
    Fake* f = context;
    assert(timeout == BEDJET_BLE_TIMEOUT_MS && f->link);
    bool command = !memcmp(uuid, bedjet_command_uuid, 16);
    assert(command || !memcmp(uuid, bedjet_status_uuid, 16));
    if(!operation(f, command ? CommandOp : StatusOp, timeout)) return false;
    *handle = command ? 12 : 13;
    if(cccd) *cccd = f->no_cccd ? 0 : 14;
    return true;
}
static void set_notify(void* context, BedjetNotifySink sink, void* sink_context) {
    Fake* f = context;
    if(!sink) {
        operation(f, DetachOp, 0);
        /* Simulate a callback completing while removal is in progress. */
        if(f->notify_during_remove) emit_status(f);
    }
    f->notify = sink;
    f->notify_context = sink_context;
}
static bool subscribe(void* context, uint16_t cccd, uint32_t timeout) {
    Fake* f = context;
    assert(cccd == 14 && timeout == BEDJET_BLE_TIMEOUT_MS);
    if(f->notify_during_subscribe) emit_status(f);
    return operation(f, SubscribeOp, timeout);
}
static bool read_status(
    void* context,
    uint16_t handle,
    uint8_t* bytes,
    uint8_t capacity,
    uint8_t* length,
    uint32_t timeout) {
    Fake* f = context;
    assert(handle == 13 && capacity == 32 && timeout == BEDJET_BLE_TIMEOUT_MS);
    if(f->notify_during_read) emit_status(f);
    if(!operation(f, ReadOp, timeout)) return false;
    *length = f->short_read ? 11 : 32;
    memcpy(bytes, f->read_packet, *length);
    return true;
}
static bool write_command(
    void* context,
    uint16_t handle,
    const uint8_t* bytes,
    uint8_t length,
    uint32_t timeout) {
    Fake* f = context;
    assert(handle == 12 && length >= 2 && length <= 3 && timeout == BEDJET_BLE_TIMEOUT_MS);
    assert(f->write_count < 64);
    memcpy(f->writes[f->write_count], bytes, length);
    f->write_lengths[f->write_count++] = length;
    if(f->notify_during_write) emit_status(f);
    return operation(f, WriteOp, timeout);
}
static bool deinitialize(void* context) {
    Fake* f = context;
    operation(f, DeinitOp, 0);
    if(f->scan_during_deinit) {
        BedjetAdvertisement a = advertisement(1);
        emit_advertisement(f, &a);
    }
    /* Match the real API contract: callbacks detach even on deferred teardown. */
    f->scan = NULL;
    f->scan_context = NULL;
    f->notify = NULL;
    f->notify_context = NULL;
    if(f->deinit_failures) {
        f->deinit_failures--;
        return false;
    }
    f->scanning = f->link = false;
    return true;
}
static uint8_t status_code(void* context) {
    (void)context;
    return 0x42;
}
static const BedjetTransport transport = {
    .supported = supported,
    .init = initialize,
    .is_advertising = advertising,
    .advertise = advertise,
    .scan_start = fake_start_scan,
    .scan_stop = stop_scan,
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
    .last_status = status_code,
    .now_ms = clock_ms,
};

static void configure(BedjetController* c, Fake* f) {
    memset(f, 0, sizeof(*f));
    atomic_init(&f->clock, 0);
    f->controller = c;
    f->supported = true;
    f->advertising = true;
    memcpy(f->read_packet, packet, 32);
    memcpy(f->notification, packet, 32);
    assert(bedjet_controller_init(c, &transport, f));
}
static void step(BedjetController* c, unsigned count) {
    for(unsigned i = 0; i < count; i++)
        bedjet_controller_step(c);
}
static void press(BedjetController* c, BedjetKey key) {
    assert(bedjet_controller_post_key(c, bedjet_controller_session(c), key));
    step(c, 1);
}
static void start_scan(BedjetController* c) {
    press(c, BedjetOk);
    step(c, 1);
    assert(c->phase == BedjetScan);
}
static void device_list(BedjetController* c, Fake* f) {
    start_scan(c);
    BedjetAdvertisement a = advertisement(1);
    emit_advertisement(f, &a);
    step(c, 1);
    advance(f, 8000);
    step(c, 1);
    assert(c->model.screen == Devices && c->model.count == 1);
}
static void connect_device(BedjetController* c, Fake* f) {
    device_list(c, f);
    press(c, BedjetOk);
    for(unsigned i = 0; i < 10 && c->phase != BedjetReady; i++)
        step(c, 1);
    assert(c->phase == BedjetReady && c->model.connected);
}
static void ready(BedjetController* c, Fake* f) {
    configure(c, f);
    step(c, 1);
    connect_device(c, f);
}
static void select_row(BedjetController* c, unsigned row) {
    while(c->model.row != row)
        press(c, BedjetDown);
}
static void stop(BedjetController* c) {
    bedjet_controller_shutdown(c);
    for(unsigned i = 0; i < 5 && c->phase != BedjetDone; i++)
        step(c, 1);
    assert(c->phase == BedjetDone);
}

static void test_unsupported(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    f.supported = false;
    step(&c, 1);
    assert(c.model.screen == Unsupported && f.calls[InitOp] == 0);
    assert(!bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOk));
    press(&c, BedjetBack);
    assert(c.phase == BedjetDone && f.calls[DeinitOp] == 0 && !f.write_count);
}
static void test_empty_scan(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    advance(&f, 7999);
    step(&c, 1);
    assert(c.phase == BedjetScan);
    advance(&f, 1);
    step(&c, 1);
    assert(c.model.screen == Devices && !c.model.count);
    press(&c, BedjetBack);
    assert(c.model.screen == Home);
    press(&c, BedjetBack);
    assert(c.phase == BedjetDone && f.restored == 1 && !f.write_count);
}
static void test_discovery_bounds(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    BedjetAdvertisement a = advertisement(1);
    emit_advertisement(&f, &a);
    step(&c, 1);
    a.rssi = -20;
    emit_advertisement(&f, &a);
    step(&c, 1);
    assert(c.model.count == 1 && c.model.devices[0].rssi == -20);
    a.data[0] = 31;
    emit_advertisement(&f, &a);
    step(&c, 1);
    assert(c.model.count == 1);
    for(unsigned i = 2; i <= 8; i++) {
        a = advertisement(i);
        emit_advertisement(&f, &a);
        step(&c, 1);
    }
    assert(c.model.count == BEDJET_DEVICE_COUNT);
    /* The same address with a different address type is a distinct identity. */
    stop(&c);
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    a = advertisement(1);
    emit_advertisement(&f, &a);
    step(&c, 1);
    a.addr_type = 0;
    emit_advertisement(&f, &a);
    step(&c, 1);
    assert(c.model.count == 2);
    stop(&c);
}
static void test_uuid_discovery(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    BedjetAdvertisement a = advertisement(3);
    a.data_len = 18;
    a.data[0] = 17;
    a.data[1] = 7;
    memcpy(a.data + 2, bedjet_service_uuid, 16);
    emit_advertisement(&f, &a);
    step(&c, 1);
    assert(c.model.count == 1);
    a.data_len = 32;
    assert(!bedjet_controller_post_advertisement(&c, bedjet_controller_session(&c), &a));
    stop(&c);
}
static void test_setup_without_writes(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    assert(c.model.has_status && c.model.status.target_step == 60 && !f.write_count);
    assert(f.calls[ConnectOp] == 1 && f.calls[CommandOp] == 1 && f.calls[StatusOp] == 1);
    assert(f.calls[SubscribeOp] == 1 && f.calls[ReadOp] == 1);
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 1);
    assert(f.calls[ReadOp] == 2 && !f.write_count);
    stop(&c);
    assert(!f.write_count && !f.notify && !f.scan);
}
static void test_setup_failures(void) {
    const Operation failures[] = {ConnectOp, CommandOp, StatusOp, SubscribeOp};
    for(unsigned i = 0; i < sizeof(failures) / sizeof(*failures); i++) {
        BedjetController c;
        Fake f;
        configure(&c, &f);
        step(&c, 1);
        device_list(&c, &f);
        f.fail[failures[i]] = true;
        press(&c, BedjetOk);
        step(&c, 10);
        assert(c.model.screen == Home && !c.model.connected && !c.model.has_status);
        assert(strstr(c.model.message, "failed") && !f.write_count && !f.notify && !f.link);
        assert(f.calls[failures[i]] == 1 && clock_ms(&f) >= 12000);
        stop(&c);
    }
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    device_list(&c, &f);
    f.no_cccd = true;
    press(&c, BedjetOk);
    step(&c, 10);
    assert(c.model.screen == Home && !f.calls[SubscribeOp] && !f.write_count);
    stop(&c);
}
static void test_scan_and_advertising_failures(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    f.fail[ScanStartOp] = true;
    press(&c, BedjetOk);
    step(&c, 1);
    assert(c.model.screen == Home && !f.scanning);
    stop(&c);
    configure(&c, &f);
    step(&c, 1);
    f.refuse_advertising_stop = true;
    press(&c, BedjetOk);
    advance(&f, 500);
    step(&c, 1);
    assert(
        c.model.screen == Home && !f.calls[ScanStartOp] && strstr(c.model.message, "Advertising"));
    stop(&c);
}
static void test_off_state_and_failed_read(void) {
    for(unsigned fail = 0; fail < 2; fail++) {
        BedjetController c;
        Fake f;
        configure(&c, &f);
        step(&c, 1);
        f.short_read = !fail;
        f.fail[ReadOp] = fail;
        connect_device(&c, &f);
        assert(!c.model.has_status && !f.write_count);
        select_row(&c, 1);
        press(&c, BedjetRight);
        assert(!f.write_count);
        emit_status(&f);
        step(&c, 1);
        assert(c.model.has_status);
        press(&c, BedjetRight);
        assert(f.write_count == 1 && f.writes[0][0] == 3);
        stop(&c);
    }
}
static void test_callbacks_during_operations(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    f.notify_during_subscribe = f.notify_during_read = true;
    f.notification[8] = 62;
    connect_device(&c, &f);
    step(&c, 3);
    assert(c.model.status.target_step == 62 && !f.write_count);
    f.notification[8] = 64;
    f.notify_during_write = true;
    select_row(&c, 1);
    press(&c, BedjetRight);
    assert(f.writes[0][0] == 3 && f.writes[0][1] == 63);
    step(&c, 1);
    assert(c.model.status.target_step == 64);
    f.notify_during_remove = true;
    press(&c, BedjetBack);
    step(&c, 2);
    assert(!c.model.has_status && !f.notify && c.model.screen == Home);
    stop(&c);
}
static void test_cancel_at_every_setup_stage(void) {
    const Operation stages[] = {ConnectOp, CommandOp, StatusOp, SubscribeOp, ReadOp};
    for(unsigned shutdown = 0; shutdown < 2; shutdown++)
        for(unsigned i = 0; i < 5; i++) {
            BedjetController c;
            Fake f;
            configure(&c, &f);
            step(&c, 1);
            device_list(&c, &f);
            f.cancel_during = stages[i];
            f.cancel_enabled = true;
            f.cancel_shutdown = shutdown;
            press(&c, BedjetOk);
            step(&c, 12);
            assert(
                !c.model.connected && !c.model.has_status && !f.write_count && !f.notify &&
                !f.scan);
            assert(shutdown ? c.phase == BedjetDone : c.model.screen == Home);
            if(i + 1 < 5) assert(f.calls[stages[i + 1]] == 0);
            if(!shutdown) stop(&c);
        }
}
static void test_saturation_cancellation_and_session_tokens(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    uint32_t old = bedjet_controller_session(&c);
    for(unsigned i = 0; i < BEDJET_QUEUE_CAPACITY; i++)
        assert(bedjet_controller_post_key(&c, old, BedjetOk));
    assert(!bedjet_controller_post_key(&c, old, BedjetOk));
    assert(bedjet_controller_post_key(&c, old, BedjetBack));
    step(&c, 2);
    assert(!f.write_count && c.model.screen == Home && c.model.dropped_events == 1);
    assert(!bedjet_controller_post_key(&c, old, BedjetOk));
    connect_device(&c, &f);
    assert(!f.write_count);
    assert(!bedjet_controller_post_status(&c, old, 13, packet, 32, clock_ms(&f)));
    select_row(&c, 1);
    press(&c, BedjetRight);
    assert(f.write_count == 1);
    stop(&c);
}
static void test_contended_inbox_and_shutdown(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    uint32_t session = bedjet_controller_session(&c);
    atomic_store(&c.queue_busy, true); /* Deterministic producer contention injection. */
    assert(!bedjet_controller_post_status(&c, session, 13, packet, 32, clock_ms(&f)));
    assert(bedjet_controller_post_key(&c, session, BedjetBack));
    atomic_store(&c.queue_busy, false);
    step(&c, 2);
    assert(!f.write_count && c.model.screen == Home && c.model.dropped_events == 1);
    connect_device(&c, &f);
    for(unsigned i = 0; i < BEDJET_QUEUE_CAPACITY; i++)
        assert(bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOk));
    stop(&c);
    assert(!f.write_count);
}
static void test_peer_disconnect_and_no_replay(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    press(&c, BedjetOk);
    assert(f.write_count == 1 && f.writes[0][0] == 1 && f.writes[0][1] == 2);
    uint32_t old = bedjet_controller_session(&c);
    emit_status(&f);
    f.link = false;
    step(&c, 2);
    assert(!c.model.connected && !c.model.has_status && !f.notify);
    assert(!bedjet_controller_post_key(&c, old, BedjetOk));
    connect_device(&c, &f);
    step(&c, 3);
    assert(f.write_count == 1);
    stop(&c);
}
static void test_stale_and_out_of_order_status(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    uint32_t old = c.model.status_ms;
    advance(&f, BEDJET_STATUS_FRESH_MS);
    press(&c, BedjetRight);
    assert(!f.write_count && !bedjet_controller_fresh(&c.model, clock_ms(&f)));
    assert(bedjet_controller_post_status(&c, bedjet_controller_session(&c), 13, packet, 32, old));
    step(&c, 1);
    assert(c.model.status_ms == old);
    emit_status(&f);
    step(&c, 1);
    assert(bedjet_controller_fresh(&c.model, clock_ms(&f)));
    uint32_t newest = c.model.status_ms;
    assert(bedjet_controller_post_status(
        &c, bedjet_controller_session(&c), 13, packet, 32, newest - 1));
    step(&c, 1);
    assert(c.model.status_ms == newest);
    assert(
        bedjet_controller_post_status(&c, bedjet_controller_session(&c), 99, packet, 32, newest));
    step(&c, 1);
    uint8_t bad[32];
    memcpy(bad, packet, 32);
    bad[10] = 255;
    assert(bedjet_controller_post_status(&c, bedjet_controller_session(&c), 13, bad, 32, newest));
    step(&c, 1);
    assert(c.model.status.fan_step == 9 && !f.write_count);
    assert(
        !bedjet_controller_post_status(&c, bedjet_controller_session(&c), 13, packet, 17, newest));
    assert(
        !bedjet_controller_post_status(&c, bedjet_controller_session(&c), 13, packet, 33, newest));
    stop(&c);
}
static void test_clock_wrap(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    atomic_store(&f.clock, UINT32_MAX - 20000u);
    step(&c, 1);
    connect_device(&c, &f);
    advance(&f, 13000);
    assert(bedjet_controller_fresh(&c.model, clock_ms(&f)));
    advance(&f, 2000);
    assert(!bedjet_controller_fresh(&c.model, clock_ms(&f)));
    stop(&c);
}
static void test_write_failure_and_no_optimism(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    f.fail[WriteOp] = true;
    select_row(&c, 2);
    press(&c, BedjetRight);
    assert(
        f.write_count == 1 && c.model.status.fan_step == 9 &&
        strstr(c.model.message, "Write failed"));
    step(&c, 10);
    assert(f.write_count == 1);
    stop(&c);
}
static void test_deferred_teardown_and_callback_lifetime(void) {
    for(unsigned permanent = 0; permanent < 2; permanent++) {
        BedjetController* c = malloc(sizeof(*c));
        assert(c);
        Fake f;
        ready(c, &f);
        f.deinit_failures = permanent ? 99 : 2;
        f.notify_during_remove = true;
        bedjet_controller_shutdown(c);
        step(c, 1);
        assert(!f.notify && !f.scan && c->phase == BedjetStopping);
        step(c, 2);
        assert(c->phase == BedjetDone && f.calls[DeinitOp] == 3);
        assert(c->model.cleanup_deferred == (bool)permanent);
        assert(f.restored == (permanent ? 0u : 1u));
        free(c);
        /* Sanitizers would catch an app-context access if callback detach failed. */
        emit_status(&f);
        BedjetAdvertisement a = advertisement(1);
        emit_advertisement(&f, &a);
    }
}
static void test_failed_scan_stop_and_disconnect(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    f.fail[ScanStopOp] = true;
    advance(&f, 8000);
    step(&c, 1);
    assert(c.phase == BedjetScan && f.scan);
    f.scan_during_deinit = true;
    press(&c, BedjetBack);
    step(&c, 6);
    assert(c.phase == BedjetDone && !f.scan && !f.write_count);
    ready(&c, &f);
    f.fail[DisconnectOp] = true;
    press(&c, BedjetBack);
    step(&c, 6);
    assert(c.phase == BedjetDone && f.calls[DisconnectOp] == 3 && !f.notify && !f.write_count);
}

typedef struct {
    BedjetController* c;
    Fake* f;
    atomic_uint* finished;
} Producer;
static void* producer(void* context) {
    Producer* p = context;
    uint32_t session = bedjet_controller_session(p->c);
    for(unsigned i = 0; i < 5000; i++)
        bedjet_controller_post_status(p->c, session, 13, packet, 32, clock_ms(p->f));
    atomic_fetch_add(p->finished, 1);
    return NULL;
}
static void test_control_vectors_and_device_limits(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    const uint8_t expected_modes[] = {2, 3, 5, 6};
    for(unsigned i = 0; i < 4; i++) {
        press(&c, BedjetOk);
        assert(f.writes[i][0] == 1 && f.writes[i][1] == expected_modes[i]);
        assert(c.model.status.mode == 1); /* No optimistic observed-mode update. */
        press(&c, BedjetRight);
    }
    select_row(&c, 3);
    press(&c, BedjetLeft);
    press(&c, BedjetRight);
    press(&c, BedjetOk);
    assert(
        f.write_lengths[4] == 3 && f.writes[4][0] == 2 && f.writes[4][1] == 0 &&
        f.writes[4][2] == 30);
    f.notification[11] = 0;
    f.notification[12] = 10;
    emit_status(&f);
    step(&c, 1);
    press(&c, BedjetOk);
    assert(f.writes[5][2] == 10);
    f.notification[13] = f.notification[14] = f.notification[8] = 60;
    emit_status(&f);
    step(&c, 1);
    select_row(&c, 1);
    press(&c, BedjetLeft);
    press(&c, BedjetRight);
    assert(f.write_count == 6);
    select_row(&c, 2);
    f.notification[10] = 0;
    emit_status(&f);
    step(&c, 1);
    press(&c, BedjetLeft);
    f.notification[10] = 19;
    emit_status(&f);
    step(&c, 1);
    press(&c, BedjetRight);
    assert(f.write_count == 6);
    for(unsigned row = 4; row <= 6; row++) {
        select_row(&c, row);
        press(&c, BedjetOk);
        assert(f.writes[row + 2][0] == 1 && f.writes[row + 2][1] == 0x20 + row - 4);
    }
    select_row(&c, 7);
    press(&c, BedjetOk);
    assert(f.writes[9][0] == 1 && f.writes[9][1] == 1);
    stop(&c);
}

static void test_callback_payload_ownership_and_busy_input(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    device_list(&c, &f);
    press(&c, BedjetOk);
    unsigned setup_steps = 0;
    while(c.phase != BedjetReady && setup_steps++ < 10) {
        assert(!bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOk));
        step(&c, 1);
    }
    assert(c.phase == BedjetReady && !f.write_count);
    uint8_t bytes[32];
    memcpy(bytes, packet, sizeof(bytes));
    bytes[8] = 64;
    assert(bedjet_controller_post_status(
        &c, bedjet_controller_session(&c), 13, bytes, sizeof(bytes), clock_ms(&f)));
    memset(bytes, 0, sizeof(bytes));
    step(&c, 1);
    assert(c.model.status.target_step == 64 && !f.write_count);
    assert(!bedjet_controller_post_key(&c, bedjet_controller_session(&c), (BedjetKey)-1));
    stop(&c);
}

static void test_init_contract_and_unrelated_advertisements(void) {
    BedjetController c;
    Fake f;
    BedjetTransport incomplete = transport;
    incomplete.read = NULL;
    assert(!bedjet_controller_init(NULL, &transport, &f));
    assert(!bedjet_controller_init(&c, NULL, &f));
    assert(!bedjet_controller_init(&c, &incomplete, &f));
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    BedjetAdvertisement a = advertisement(1);
    memcpy(a.data + 2, "OTHER!", 6);
    emit_advertisement(&f, &a);
    step(&c, 1);
    assert(c.model.count == 0);
    const uint8_t combined[] = {2, 1, 6, 7, 9, 'B', 'E', 'D', 'J', 'E', 'T'};
    a.data_len = sizeof(combined);
    memcpy(a.data, combined, sizeof(combined));
    emit_advertisement(&f, &a);
    step(&c, 1);
    assert(c.model.count == 1);
    stop(&c);
    ready(&c, &f);
    press(&c, BedjetUp);
    assert(c.model.row == 7 && !f.write_count);
    press(&c, BedjetDown);
    assert(c.model.row == 0 && !f.write_count);
    stop(&c);
}

static void test_concurrent_producers(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    pthread_t threads[4];
    atomic_uint finished;
    atomic_init(&finished, 0);
    Producer p = {&c, &f, &finished};
    for(unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&threads[i], NULL, producer, &p));
    while(atomic_load(&finished) < 4)
        step(&c, 1);
    for(unsigned i = 0; i < 4; i++)
        assert(!pthread_join(threads[i], NULL));
    step(&c, BEDJET_QUEUE_CAPACITY + 1);
    assert(c.queue_count == 0 && c.model.status.target_step == 60 && !f.write_count);
    stop(&c);
}

int main(void) {
    unsigned count = 0;
#define RUN(test)                    \
    do {                             \
        test();                      \
        count++;                     \
        printf("PASS: %s\n", #test); \
    } while(0)
    RUN(test_unsupported);
    RUN(test_empty_scan);
    RUN(test_discovery_bounds);
    RUN(test_uuid_discovery);
    RUN(test_setup_without_writes);
    RUN(test_setup_failures);
    RUN(test_scan_and_advertising_failures);
    RUN(test_off_state_and_failed_read);
    RUN(test_callbacks_during_operations);
    RUN(test_cancel_at_every_setup_stage);
    RUN(test_saturation_cancellation_and_session_tokens);
    RUN(test_contended_inbox_and_shutdown);
    RUN(test_peer_disconnect_and_no_replay);
    RUN(test_stale_and_out_of_order_status);
    RUN(test_clock_wrap);
    RUN(test_write_failure_and_no_optimism);
    RUN(test_deferred_teardown_and_callback_lifetime);
    RUN(test_failed_scan_stop_and_disconnect);
    RUN(test_control_vectors_and_device_limits);
    RUN(test_callback_payload_ownership_and_busy_input);
    RUN(test_init_contract_and_unrelated_advertisements);
    RUN(test_concurrent_producers);
    printf(
        "PASS: %u lifecycle scenarios; controller=%zu bytes, model=%zu bytes\n",
        count,
        sizeof(BedjetController),
        sizeof(BedjetModel));
    return 0;
}
