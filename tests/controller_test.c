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
    AttachOp,
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
    /* A fast failure returns without using its wait; drop_on_fail also loses
     * the link. Plain `fail` uses the whole wait, as a firmware timeout does. */
    bool fail_fast[OpCount], drop_on_fail;
    uint8_t status;
    unsigned calls[OpCount], deinit_failures, restored;
    uint32_t duration[OpCount];
    unsigned write_count;
    uint8_t writes[64][3], write_lengths[64];
    Operation cancel_during;
    bool cancel_enabled, cancel_shutdown, cancel_off;
    bool notify_during_read, notify_during_write, notify_during_remove, notify_during_subscribe;
    bool scan_during_deinit, scan_during_stop;
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
    advance(f, f->duration[op]);
    if(f->cancel_enabled && f->cancel_during == op) {
        f->cancel_enabled = false;
        if(f->cancel_shutdown)
            bedjet_controller_shutdown(f->controller);
        else
            assert(bedjet_controller_post_key(
                f->controller,
                bedjet_controller_session(f->controller),
                f->cancel_off ? BedjetOff : BedjetBack));
    }
    if(f->fail[op] || f->fail_fast[op]) {
        if(f->fail[op]) advance(f, timeout);
        if(f->drop_on_fail) f->link = false;
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
    /* Reports can still arrive until the stop's callback drain completes. */
    for(unsigned i = 0; f->scan_during_stop && i < BEDJET_QUEUE_CAPACITY; i++) {
        BedjetAdvertisement a = advertisement(1);
        emit_advertisement(f, &a);
    }
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
    if(sink) operation(f, AttachOp, 0);
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
    return ((Fake*)context)->status;
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
    f->status = 0x42;
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
static void observe(BedjetController* c, Fake* f) {
    advance(f, 1);
    emit_status(f);
    step(c, 1);
}
static void select_row(BedjetController* c, unsigned row) {
    while(c->model.row != row)
        press(c, BedjetDown);
}
/* The mode draft follows the observed mode (Heat in the fixture); choose Cool
 * explicitly before sending it from the Mode row. */
static void draft_cool(BedjetController* c) {
    assert(c->model.row == BedjetRowMode);
    while(c->model.mode_choice != 0)
        press(c, BedjetLeft);
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
        assert(strstr(c.model.message, "no reply") && !f.write_count && !f.notify && !f.link);
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
        f.fail_fast[ReadOp] = fail;
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
    const Operation stages[] = {ConnectOp, CommandOp, StatusOp, AttachOp, SubscribeOp, ReadOp};
    for(unsigned shutdown = 0; shutdown < 2; shutdown++)
        for(unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
            BedjetController c;
            Fake f;
            configure(&c, &f);
            step(&c, 1);
            device_list(&c, &f);
            f.cancel_during = stages[i];
            f.cancel_enabled = true;
            f.cancel_shutdown = shutdown;
            press(&c, BedjetOk);
            for(unsigned tick = 0; tick < 12; tick++) {
                step(&c, 1);
                /* Inspect every published tick, including the cancellation
                 * tick itself. Eventual cleanup alone misses a Ready flicker. */
                assert(c.phase != BedjetReady && c.model.screen != Control);
            }
            assert(
                !c.model.connected && !c.model.has_status && !f.write_count && !f.notify &&
                !f.scan);
            assert(shutdown ? c.phase == BedjetDone : c.model.screen == Home);
            if(i + 1 < sizeof(stages) / sizeof(stages[0])) assert(f.calls[stages[i + 1]] == 0);
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
    draft_cool(&c);
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
    uint8_t changed[32];
    memcpy(changed, packet, 32);
    changed[8] = 62; /* A distinct target proves the stale frame was not applied. */
    assert(bedjet_controller_post_status(&c, bedjet_controller_session(&c), 13, changed, 32, old));
    step(&c, 1);
    assert(c.model.status_ms == old && c.model.status.target_step == 60);
    press(&c, BedjetRight);
    assert(!f.write_count);
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
    /* Short frames are admitted as evidence but rejected by the decoder;
     * oversized frames cannot be copied and are counted instead. */
    BedjetDiagnostics before = c.model.diagnostics;
    assert(
        bedjet_controller_post_status(&c, bedjet_controller_session(&c), 13, packet, 17, newest));
    step(&c, 1);
    BedjetDiagnostics* d = &c.model.diagnostics;
    assert(
        d->frames_rejected == before.frames_rejected + 1 && d->last_reason == BedjetStatusLength);
    assert(d->rejected_len == 17 && !memcmp(d->rejected, packet, 17));
    assert(c.model.status.fan_step == 9 && c.model.status_ms == newest);
    uint8_t large[40] = {0};
    assert(
        !bedjet_controller_post_status(&c, bedjet_controller_session(&c), 13, large, 33, newest));
    step(&c, 1);
    assert(d->frames_oversize == before.frames_oversize + 1);
    assert(d->frames_late >= 2 && !f.write_count);
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
        /* The detach assertion above is the lifetime check: the fake models the
         * pinned API by clearing callbacks in deinit, so no later firmware
         * callback can reach the freed controller. */
        free(c);
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
    atomic_uint* accepted;
} Producer;
static void* producer(void* context) {
    Producer* p = context;
    uint32_t session = bedjet_controller_session(p->c);
    for(unsigned i = 0; i < 5000; i++)
        if(bedjet_controller_post_status(p->c, session, 13, packet, 32, clock_ms(p->f)))
            atomic_fetch_add(p->accepted, 1);
    atomic_fetch_add(p->finished, 1);
    return NULL;
}
static void test_control_vectors_and_device_limits(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    draft_cool(&c);
    const uint8_t expected_modes[] = {2, 3, 5, 6};
    const uint8_t observed_modes[] = {4, 1, 5, 3};
    for(unsigned i = 0; i < 4; i++) {
        press(&c, BedjetOk);
        assert(f.writes[i][0] == 1 && f.writes[i][1] == expected_modes[i]);
        assert(c.model.status.mode == (i ? observed_modes[i - 1] : 1));
        f.notification[9] = observed_modes[i];
        observe(&c, &f);
        assert(c.model.command.state == BedjetCommandApplied);
        press(&c, BedjetRight);
    }
    select_row(&c, 3);
    press(&c, BedjetLeft);
    press(&c, BedjetRight);
    press(&c, BedjetOk);
    assert(
        f.write_lengths[4] == 3 && f.writes[4][0] == 2 && f.writes[4][1] == 0 &&
        f.writes[4][2] == 30);
    f.notification[4] = 0;
    f.notification[5] = 30;
    f.notification[6] = 0;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied);
    f.notification[11] = 0;
    f.notification[12] = 10;
    emit_status(&f);
    step(&c, 1);
    press(&c, BedjetOk);
    assert(f.writes[5][2] == 10);
    f.notification[5] = 10;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied);
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
        assert(c.model.command.state == BedjetCommandUnverified);
        /* A same-mode status unlocks only once the preset has settled. */
        advance(&f, BEDJET_PRESET_SETTLE_MS);
        observe(&c, &f);
    }
    select_row(&c, 7);
    press(&c, BedjetOk);
    assert(f.write_count == 9); /* Only a hold sends Off, never a short OK. */
    press(&c, BedjetOff);
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
    assert(c.model.row == BedjetRowCount - 1 && !f.write_count);
    press(&c, BedjetDown);
    assert(c.model.row == 0 && !f.write_count);
    stop(&c);
}

static void test_concurrent_producers(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    pthread_t threads[4];
    atomic_uint finished, accepted;
    atomic_init(&finished, 0);
    atomic_init(&accepted, 0);
    unsigned dropped = atomic_load(&c.dropped_events);
    Producer p = {&c, &f, &finished, &accepted};
    for(unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&threads[i], NULL, producer, &p));
    while(atomic_load(&finished) < 4)
        step(&c, 1);
    for(unsigned i = 0; i < 4; i++)
        assert(!pthread_join(threads[i], NULL));
    step(&c, BEDJET_QUEUE_CAPACITY + 1);
    assert(c.queue_count == 0 && c.model.status.target_step == 60 && !f.write_count);
    /* Conservation: every post was either admitted or counted as dropped. */
    assert(atomic_load(&accepted) + (atomic_load(&c.dropped_events) - dropped) == 20000);
    stop(&c);
}

static void test_priority_off_saturated_rows(void) {
    for(unsigned row = 0; row < BedjetRowCount; row++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        select_row(&c, row);
        uint32_t old = bedjet_controller_session(&c);
        BedjetStatus observed = c.model.status;
        for(unsigned i = 0; i < BEDJET_QUEUE_CAPACITY; i++) {
            if(i % 2)
                assert(bedjet_controller_post_status(&c, old, 13, packet, 32, clock_ms(&f)));
            else
                assert(bedjet_controller_post_key(
                    &c, old, row == 1 || row == 2 ? BedjetRight : BedjetOk));
        }
        assert(!bedjet_controller_post_status(&c, old, 13, packet, 32, clock_ms(&f)));
        for(unsigned i = 0; i < 100; i++)
            assert(bedjet_controller_post_key(&c, old, BedjetOff));
        assert(!bedjet_controller_post_key(&c, old, BedjetOk));
        assert(!bedjet_controller_refresh(&c, old));
        step(&c, 1);
        assert(
            f.write_count == 1 && f.write_lengths[0] == 2 && f.writes[0][0] == 1 &&
            f.writes[0][1] == 1);
        assert(c.model.has_status && !memcmp(&observed, &c.model.status, sizeof(observed)));
        assert(c.model.dropped_events == 1 && bedjet_controller_session(&c) != old);
        assert(!bedjet_controller_post_key(&c, old, BedjetBack));
        assert(!bedjet_controller_post_key(&c, old, BedjetOff));
        assert(!bedjet_controller_post_key(&c, old, BedjetOk));
        assert(!bedjet_controller_post_status(&c, old, 13, packet, 32, clock_ms(&f)));
        f.notification[8] = 64;
        emit_status(&f);
        step(&c, BEDJET_QUEUE_CAPACITY + 2);
        assert(f.write_count == 1 && c.model.status.target_step == 64);
        stop(&c);
    }
}

static void test_priority_off_contended_inbox(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    uint32_t old = bedjet_controller_session(&c);
    assert(bedjet_controller_post_key(&c, old, BedjetOk));
    atomic_store(&c.queue_busy, true);
    assert(!bedjet_controller_post_key(&c, old, BedjetRight));
    assert(bedjet_controller_post_key(&c, old, BedjetOff));
    step(&c, 1); /* Queue cannot be cleared: its old entry must still be stale. */
    assert(f.write_count == 1 && c.queue_count == 1);
    atomic_store(&c.queue_busy, false);
    step(&c, 2);
    assert(c.queue_count == 0 && f.write_count == 1 && c.model.dropped_events == 1);
    /* First observe Off, then a deliberate new command is allowed. Standby is
     * not a selectable mode, so the draft keeps the last observed one (Heat). */
    f.notification[9] = 0;
    observe(&c, &f);
    press(&c, BedjetOk);
    assert(f.write_count == 2 && f.writes[1][1] == 3);
    stop(&c);
}

static void test_priority_ordering(void) {
    for(unsigned order = 0; order < 3; order++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        uint32_t session = bedjet_controller_session(&c);
        if(order == 0) {
            assert(bedjet_controller_post_key(&c, session, BedjetBack));
            assert(!bedjet_controller_post_key(&c, session, BedjetOff));
        } else {
            assert(bedjet_controller_post_key(&c, session, BedjetOff));
            if(order == 1)
                assert(bedjet_controller_post_key(&c, session, BedjetBack));
            else
                bedjet_controller_shutdown(&c);
        }
        step(&c, 8);
        assert(!f.write_count && !f.notify && !f.link);
        assert(order == 2 ? c.phase == BedjetDone : c.model.screen == Home);
        stop(&c);
    }
}

static void test_priority_off_busy_and_failed(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    assert(!bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOff));
    step(&c, 1);
    assert(!bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOff));
    device_list(&c, &f);
    press(&c, BedjetOk);
    while(c.phase != BedjetReady) {
        assert(!bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOff));
        step(&c, 1);
    }
    f.fail[WriteOp] = true;
    press(&c, BedjetOff);
    assert(strstr(c.model.message, "Write failed") && c.model.status.mode == 1);
    step(&c, 30);
    assert(f.write_count == 1 && c.phase == BedjetReady);
    press(&c, BedjetBack);
    connect_device(&c, &f);
    assert(f.write_count == 1); /* Failed Off is never retried on reconnect. */
    stop(&c);
    assert(!bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOff));
}

static void test_off_during_read_and_write(void) {
    for(unsigned op = 0; op < 2; op++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        draft_cool(&c);
        f.cancel_enabled = f.cancel_off = true;
        f.cancel_during = op ? WriteOp : ReadOp;
        uint32_t session = bedjet_controller_session(&c);
        if(op)
            assert(bedjet_controller_post_key(&c, session, BedjetOk));
        else
            assert(bedjet_controller_refresh(&c, session));
        assert(bedjet_controller_post_key(&c, session, BedjetOk));
        step(&c, 1);
        assert(f.write_count == op); /* Only the already admitted write finishes. */
        step(&c, 30);
        assert(f.write_count == op + 1 && f.writes[op][1] == 1);
        stop(&c);
    }
}

static void test_back_during_off_and_refresh(void) {
    const Operation stages[] = {DetachOp, WriteOp, AttachOp, ReadOp};
    for(unsigned shutdown = 0; shutdown < 2; shutdown++)
        for(unsigned i = 0; i < 4; i++) {
            BedjetController c;
            Fake f;
            ready(&c, &f);
            f.cancel_enabled = true;
            f.cancel_during = stages[i];
            f.cancel_shutdown = shutdown;
            f.notify_during_remove = true;
            if(i == 3)
                assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
            else
                assert(bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOff));
            step(&c, 1);
            assert(c.phase != BedjetReady && !c.model.connected && !f.notify);
            assert(f.write_count == (i == 1 || i == 2 ? 1u : 0u));
            step(&c, 8);
            assert(!f.notify && !f.link);
            stop(&c);
        }
}

static void test_notification_flood_back_and_lost_link_off(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    for(unsigned i = 0; i < BEDJET_QUEUE_CAPACITY + 100; i++)
        emit_status(&f);
    assert(bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetBack));
    step(&c, 2);
    assert(!f.write_count && c.model.screen == Home && c.model.dropped_events == 100);
    connect_device(&c, &f);
    assert(bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetOff));
    f.link = false;
    step(&c, 2);
    assert(!f.write_count && c.model.screen == Home && !f.notify);
    connect_device(&c, &f);
    assert(!f.write_count);
    stop(&c);
}

static void test_scan_boundary_cancellation(void) {
    for(unsigned shutdown = 0; shutdown < 2; shutdown++)
        for(unsigned stage = 0; stage < 3; stage++) {
            BedjetController c;
            Fake f;
            configure(&c, &f);
            if(stage) {
                step(&c, 1);
                press(&c, BedjetOk);
                if(stage == 2) {
                    step(&c, 1);
                    advance(&f, 8000);
                }
            }
            f.cancel_enabled = true;
            f.cancel_shutdown = shutdown;
            f.cancel_during = stage == 0 ? InitOp : stage == 1 ? ScanStartOp : ScanStopOp;
            step(&c, 1);
            assert(!f.cancel_enabled && c.phase != BedjetScan && c.model.screen != Devices);
            step(&c, 8);
            assert(!f.write_count && !f.scan && !f.notify);
            assert(shutdown || !stage ? c.phase == BedjetDone : c.model.screen == Home);
            stop(&c);
        }
}

static void test_repeated_back_failed_teardown(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    uint32_t session = bedjet_controller_session(&c);
    for(unsigned i = 0; i < BEDJET_QUEUE_CAPACITY; i++)
        assert(bedjet_controller_post_key(&c, session, BedjetOk));
    for(unsigned i = 0; i < 100; i++)
        assert(bedjet_controller_post_key(&c, session, BedjetBack));
    f.fail[DisconnectOp] = true;
    f.deinit_failures = 100;
    f.notify_during_remove = true;
    step(&c, 1);
    assert(c.phase == BedjetDisconnecting && !f.notify && !f.write_count);
    session = bedjet_controller_session(&c);
    for(unsigned i = 0; i < 100; i++)
        assert(bedjet_controller_post_key(&c, session, BedjetBack));
    step(&c, 1);
    for(unsigned i = 0; i < 100; i++)
        assert(!bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetBack));
    step(&c, 5);
    assert(c.phase == BedjetDone && c.model.cleanup_deferred && f.calls[DeinitOp] == 3);
    assert(!f.notify && !f.scan && !f.write_count && !f.restored);
}

typedef struct {
    BedjetController* controller;
    uint32_t session;
    BedjetKey key;
    unsigned accepted;
} PriorityProducer;
static void* priority_producer(void* context) {
    PriorityProducer* p = context;
    for(unsigned i = 0; i < 5000; i++)
        p->accepted += bedjet_controller_post_key(p->controller, p->session, p->key);
    return NULL;
}
static void test_concurrent_priority_producers(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    pthread_t threads[4];
    PriorityProducer p[4];
    uint32_t old = bedjet_controller_session(&c);
    atomic_store(&c.queue_busy, true);
    for(unsigned i = 0; i < 4; i++) {
        p[i] = (PriorityProducer){&c, old, i % 2 ? BedjetBack : BedjetOff, 0};
        assert(!pthread_create(&threads[i], NULL, priority_producer, &p[i]));
    }
    for(unsigned i = 0; i < 4; i++)
        assert(!pthread_join(threads[i], NULL));
    assert(p[1].accepted == 5000 && p[3].accepted == 5000);
    step(&c, 2);
    atomic_store(&c.queue_busy, false);
    assert(!f.write_count && c.model.screen == Home);
    connect_device(&c, &f);
    for(unsigned i = 0; i < 4; i++) {
        p[i].accepted = 0;
        assert(!pthread_create(&threads[i], NULL, priority_producer, &p[i]));
    }
    for(unsigned i = 0; i < 4; i++) {
        assert(!pthread_join(threads[i], NULL));
        assert(p[i].accepted == 0);
    }
    step(&c, 2);
    assert(c.phase == BedjetReady && !f.write_count);
    stop(&c);
}

static void test_off_without_fresh_status(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    advance(&f, BEDJET_STATUS_FRESH_MS);
    press(&c, BedjetOff);
    assert(f.write_count == 1 && !bedjet_controller_fresh(&c.model, clock_ms(&f)));
    press(&c, BedjetBack);
    f.fail_fast[ReadOp] = true;
    connect_device(&c, &f);
    assert(!c.model.has_status);
    press(&c, BedjetOff);
    assert(f.write_count == 2 && f.writes[1][1] == 1 && !c.model.has_status);
    stop(&c);
}

static void test_rapid_temperature_intent(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    assert(f.write_count == 1 && f.writes[0][1] == 61);
    assert(c.model.command.sent == 61 && c.model.command.desired == 63);
    assert(c.model.status.target_step == 60 && c.model.command.state == BedjetCommandPending);
    select_row(&c, 2);
    press(&c, BedjetRight); /* A different kind is rejected, never queued. */
    assert(f.write_count == 1 && c.model.command.kind == BedjetCommandTemperature);
    f.notification[8] = 61;
    observe(&c, &f);
    assert(f.write_count == 2 && f.writes[1][1] == 63);
    assert(c.model.status.target_step == 61 && c.model.command.state == BedjetCommandPending);
    f.notification[8] = 62;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandPending && f.write_count == 2);
    f.notification[8] = 63;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied && c.model.status.target_step == 63);
    step(&c, 50);
    assert(f.write_count == 2);
    press(&c, BedjetRight);
    assert(f.write_count == 3 && f.writes[2][0] == 7 && f.writes[2][1] == 10);
    stop(&c);
}
static void test_fan_coalescing_clamp_and_reversal(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    f.notification[10] = 18;
    observe(&c, &f);
    select_row(&c, 2);
    press(&c, BedjetRight);
    for(unsigned i = 0; i < 30; i++)
        press(&c, BedjetRight);
    assert(c.model.command.desired == 19 && f.write_count == 1);
    press(&c, BedjetLeft);
    press(&c, BedjetLeft);
    assert(c.model.command.desired == 17 && c.model.status.fan_step == 18);
    f.notification[10] = 19;
    observe(&c, &f);
    assert(f.write_count == 2 && f.writes[1][0] == 7 && f.writes[1][1] == 17);
    f.notification[10] = 17;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied);
    press(&c, BedjetLeft);
    press(&c, BedjetRight); /* Undo unsent repeat; still need the first write confirmed. */
    assert(c.model.command.desired == 17 && f.write_count == 3);
    f.notification[10] = 16;
    observe(&c, &f);
    assert(f.write_count == 4 && f.writes[3][1] == 17);
    f.notification[10] = 17;
    observe(&c, &f);
    stop(&c);
}
static void test_no_redundant_coalesced_write(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    press(&c, BedjetLeft);
    assert(c.model.command.desired == 61);
    f.notification[8] = 61;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied && f.write_count == 1);
    stop(&c);
}
static void test_confirmation_requires_new_receive_time(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    f.notification[8] = 61;
    f.notify_during_write = true;
    press(&c, BedjetRight);
    step(&c, 1);
    assert(c.model.status.target_step == 61 && c.model.command.state == BedjetCommandPending);
    uint32_t boundary = c.model.command.sent_ms;
    advance(&f, 10);
    assert(bedjet_controller_post_status(
        &c, bedjet_controller_session(&c), 13, f.notification, 32, boundary));
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandPending);
    uint8_t bad[32];
    memcpy(bad, f.notification, 32);
    bad[10] = 200;
    assert(bedjet_controller_post_status(
        &c, bedjet_controller_session(&c), 13, bad, 32, clock_ms(&f)));
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandPending);
    assert(bedjet_controller_post_status(
        &c, bedjet_controller_session(&c), 99, f.notification, 32, clock_ms(&f)));
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandPending);
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied);
    assert(bedjet_controller_post_status(
        &c, bedjet_controller_session(&c), 13, packet, 32, boundary));
    step(&c, 1);
    assert(c.model.status.target_step == 61 && c.model.command.state == BedjetCommandApplied);
    stop(&c);
}
static void test_timeout_discards_intent_and_old_keys(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    f.notification[10] = 10; /* Unrelated fresh fan telemetry cannot confirm temperature. */
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandPending && c.model.status.fan_step == 10);
    advance(&f, BEDJET_COMMAND_TIMEOUT_MS - 2);
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandPending);
    assert(bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetRight));
    advance(&f, 1);
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandTimedOut && f.write_count == 1);
    assert(
        c.model.command.desired == c.model.command.sent &&
        strstr(c.model.message, "Not confirmed"));
    f.notification[8] = 61;
    observe(&c, &f); /* Late match updates actual state without resurrecting intent. */
    assert(c.model.status.target_step == 61 && c.model.command.state == BedjetCommandTimedOut);
    step(&c, 50);
    assert(f.write_count == 1 && strstr(c.model.message, "Not confirmed"));
    press(&c, BedjetRight); /* New deliberate input rebases on the actual value. */
    assert(f.write_count == 2 && f.writes[1][1] == 62);
    stop(&c);
}
static void test_rejected_write_stays_rejected(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    f.fail[WriteOp] = true;
    press(&c, BedjetRight);
    assert(c.model.command.state == BedjetCommandRejected && c.model.status.target_step == 60);
    f.notification[8] = 61;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandRejected && strstr(c.model.message, "0x42"));
    advance(&f, BEDJET_COMMAND_TIMEOUT_MS);
    step(&c, 10);
    assert(c.model.command.state == BedjetCommandRejected && f.write_count == 1);
    f.fail[WriteOp] = false;
    press(&c, BedjetRight);
    assert(f.write_count == 2 && f.writes[1][1] == 62);
    stop(&c);
}
static void test_mode_limits_require_matching_status(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    draft_cool(&c);
    press(&c, BedjetOk); /* Cool: button 2, observed mode 4. */
    assert(!c.model.limits_ready && c.model.command.sent == 4 && c.model.status.mode == 1);
    observe(&c, &f); /* Old mode, even with a newer receive timestamp. */
    assert(!c.model.limits_ready && c.model.command.state == BedjetCommandPending);
    select_row(&c, 1);
    press(&c, BedjetRight);
    select_row(&c, 3);
    press(&c, BedjetOk);
    assert(f.write_count == 1);
    f.notification[9] = 6; /* Transitional Wait is not a rejection or confirmation. */
    observe(&c, &f);
    assert(!c.model.limits_ready && c.model.command.state == BedjetCommandPending);
    f.notification[9] = 4;
    f.notification[13] = 40;
    f.notification[14] = 60;
    f.notification[8] = 58;
    f.notification[11] = 0;
    f.notification[12] = 10;
    observe(&c, &f);
    assert(c.model.limits_ready && c.model.command.state == BedjetCommandApplied);
    select_row(&c, 1);
    press(&c, BedjetRight);
    assert(f.write_count == 2 && f.writes[1][1] == 59);
    f.notification[8] = 59;
    observe(&c, &f);
    select_row(&c, 3);
    press(&c, BedjetOk);
    assert(f.write_count == 3 && f.writes[2][0] == 2 && f.writes[2][2] == 10);
    stop(&c);
}
static void test_mode_timeout_and_late_limits(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    draft_cool(&c);
    press(&c, BedjetOk);
    advance(&f, BEDJET_COMMAND_TIMEOUT_MS);
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandTimedOut && !c.model.limits_ready);
    observe(&c, &f);
    select_row(&c, 1);
    press(&c, BedjetRight);
    select_row(&c, 3);
    press(&c, BedjetOk);
    assert(!c.model.limits_ready && f.write_count == 1);
    f.notification[9] = 4;
    f.notification[14] = 60;
    observe(&c, &f);
    assert(c.model.limits_ready && c.model.command.state == BedjetCommandTimedOut);
    select_row(&c, 1);
    press(&c, BedjetLeft);
    assert(f.write_count == 2 && f.writes[1][1] == 59);
    stop(&c);
}
static void test_memory_is_unverified_and_limits_gate(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 4);
    press(&c, BedjetOk);
    assert(f.write_count == 1 && c.model.command.state == BedjetCommandUnverified);
    assert(!c.model.limits_ready);
    press(&c, BedjetOk);
    select_row(&c, 1);
    press(&c, BedjetRight);
    assert(f.write_count == 1);
    f.notification[9] = 3;
    observe(&c, &f);
    assert(c.model.limits_ready && c.model.command.state == BedjetCommandUnverified);
    assert(strstr(c.model.message, "unverified") && strstr(c.model.command.result, "unverified"));
    press(&c, BedjetRight);
    assert(f.write_count == 2 && f.writes[1][0] == 3);
    stop(&c);
}
static void set_remaining(Fake* f, unsigned seconds) {
    f->notification[4] = seconds / 3600;
    f->notification[5] = (seconds / 60) % 60;
    f->notification[6] = seconds % 60;
}
static void test_timer_confirmation_and_device_limit_rejection(void) {
    for(unsigned reject = 0; reject < 2; reject++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        select_row(&c, 3);
        press(&c, BedjetOk);
        set_remaining(&f, 1805);
        observe(&c, &f);
        assert(c.model.command.state == BedjetCommandPending);
        advance(&f, 1200);
        if(reject) {
            f.notification[11] = 0;
            f.notification[12] = 10;
            set_remaining(&f, 600);
        } else
            set_remaining(&f, 1797);
        observe(&c, &f);
        assert(c.model.command.state == (reject ? BedjetCommandRejected : BedjetCommandApplied));
        step(&c, 20);
        assert(f.write_count == 1);
        stop(&c);
    }
}
static void test_changed_bounds_clamp_or_drop_intent(void) {
    for(unsigned reject = 0; reject < 2; reject++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        select_row(&c, 1);
        for(unsigned i = 0; i < 5; i++)
            press(&c, BedjetRight);
        assert(c.model.command.desired == 65 && f.write_count == 1);
        f.notification[14] = reject ? 60 : 62;
        f.notification[8] = reject ? 60 : 61;
        observe(&c, &f);
        if(reject)
            assert(c.model.command.state == BedjetCommandRejected && f.write_count == 1);
        else {
            assert(f.write_count == 2 && f.writes[1][1] == 62);
            f.notification[8] = 62;
            observe(&c, &f);
            assert(c.model.command.state == BedjetCommandApplied);
        }
        stop(&c);
    }
}
static void test_external_remote_conflict_policy(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 2);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    f.notification[10] = 6; /* No immediate retry to fight an external fan value. */
    observe(&c, &f);
    assert(c.model.status.fan_step == 6 && c.model.command.state == BedjetCommandPending);
    f.notification[9] = 0; /* External mode change abandons the old-mode intent. */
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandConflict && c.model.status.mode == 0);
    assert(c.model.command.desired == c.model.command.sent && f.write_count == 1);
    observe(&c, &f);
    assert(strstr(c.model.message, "Mode changed"));
    press(&c, BedjetRight);
    assert(f.write_count == 2 && f.writes[1][1] == 7);
    stop(&c);
}
static void test_off_preempts_coalesced_setting(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    uint32_t old = bedjet_controller_session(&c);
    press(&c, BedjetOff);
    assert(f.write_count == 2 && f.writes[1][0] == 1 && f.writes[1][1] == 1);
    assert(
        c.model.command.kind == BedjetCommandOff && c.model.command.state == BedjetCommandPending);
    assert(!bedjet_controller_post_status(&c, old, 13, packet, 32, clock_ms(&f)));
    f.notification[8] = 61;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandPending);
    f.notification[9] = 0;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied && c.model.status.mode == 0);
    step(&c, 40);
    assert(f.write_count == 2);
    stop(&c);
}
static void test_pending_setting_never_replays_after_reconnect(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    uint32_t old = bedjet_controller_session(&c);
    f.link = false;
    step(&c, 2);
    connect_device(&c, &f);
    assert(c.model.command.state == BedjetCommandIdle && f.write_count == 1);
    assert(!bedjet_controller_post_status(&c, old, 13, f.notification, 32, clock_ms(&f)));
    observe(&c, &f);
    step(&c, 40);
    assert(f.write_count == 1);
    stop(&c);
}
static void test_command_clock_wrap_and_read_confirmation(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    atomic_store(&f.clock, UINT32_MAX - 10000u);
    step(&c, 1);
    connect_device(&c, &f);
    select_row(&c, 1);
    press(&c, BedjetRight);
    f.read_packet[8] = 61;
    advance(&f, 1);
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandApplied);
    select_row(&c, 2);
    press(&c, BedjetRight);
    advance(&f, BEDJET_COMMAND_TIMEOUT_MS);
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandTimedOut && f.write_count == 2);
    stop(&c);
}
static void test_confirmation_lost_to_queue_overflow(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    press(&c, BedjetRight);
    press(&c, BedjetRight);
    for(unsigned i = 0; i < BEDJET_QUEUE_CAPACITY; i++)
        emit_status(&f);
    f.notification[8] = 61;
    advance(&f, 1);
    emit_status(&f); /* Matching notification is dropped and counted. */
    advance(&f, BEDJET_COMMAND_TIMEOUT_MS);
    step(&c, BEDJET_QUEUE_CAPACITY + 2);
    assert(c.model.command.state == BedjetCommandTimedOut && c.model.dropped_events == 1);
    assert(f.write_count == 1 && c.model.command.desired == c.model.command.sent);
    stop(&c);
}
static void test_mode_discards_pre_dispatch_inputs(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    draft_cool(&c);
    uint32_t session = bedjet_controller_session(&c);
    assert(bedjet_controller_post_key(&c, session, BedjetOk));
    assert(bedjet_controller_post_key(&c, session, BedjetDown));
    assert(bedjet_controller_post_key(&c, session, BedjetRight));
    step(&c, 3);
    assert(c.model.row == 0 && f.write_count == 1 && !c.model.limits_ready);
    f.notification[9] = 4;
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied && f.write_count == 1);
    select_row(&c, 1);
    press(&c, BedjetRight);
    assert(f.write_count == 2);
    stop(&c);
}

static void test_write_return_is_confirmation_boundary(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    f.duration[WriteOp] = 100;
    f.notification[8] = 61;
    f.notify_during_write = true;
    press(&c, BedjetRight);
    step(&c, 1);
    assert(c.model.status.target_step == 61 && c.model.command.state == BedjetCommandPending);
    assert(c.model.command.sent_ms == clock_ms(&f));
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied && f.write_count == 1);
    stop(&c);
}
static void test_delayed_confirmation_cannot_revive_timeout(void) {
    for(unsigned read = 0; read < 2; read++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        select_row(&c, 1);
        press(&c, BedjetRight);
        press(&c, BedjetRight);
        if(read) {
            f.read_packet[8] = 61;
            f.duration[ReadOp] = BEDJET_COMMAND_TIMEOUT_MS;
            assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
        } else {
            f.notification[8] = 61;
            advance(&f, 1);
            emit_status(&f);
            advance(&f, BEDJET_COMMAND_TIMEOUT_MS);
        }
        step(&c, 1);
        assert(c.model.status.target_step == 61 && c.model.command.state == BedjetCommandTimedOut);
        assert(c.model.command.desired == c.model.command.sent && f.write_count == 1);
        step(&c, 20);
        assert(f.write_count == 1);
        stop(&c);
    }
}
static void test_rf_mode_switch_discards_older_inputs(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 1);
    f.notification[9] = 4;
    f.notification[14] = 60;
    f.notification[8] = 58;
    advance(&f, 1);
    emit_status(&f);
    assert(bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetRight));
    step(&c, 2);
    assert(c.model.status.mode == 4 && c.model.limits_ready && !f.write_count);
    press(&c, BedjetRight);
    assert(f.write_count == 1 && f.writes[0][1] == 59);
    stop(&c);
}
static void test_zero_timer_limit_and_failed_mode_resync(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    f.notification[11] = f.notification[12] = 0;
    observe(&c, &f);
    select_row(&c, 3);
    press(&c, BedjetRight);
    press(&c, BedjetOk);
    assert(!f.write_count && strstr(c.model.message, "No timer allowed"));
    select_row(&c, 0);
    draft_cool(&c);
    f.fail[WriteOp] = true;
    press(&c, BedjetOk);
    assert(c.model.command.state == BedjetCommandRejected && !c.model.limits_ready);
    observe(&c, &f); /* Rejected mode: resync from a new actual-mode observation. */
    assert(c.model.limits_ready && c.model.command.state == BedjetCommandRejected);
    assert(strstr(c.model.message, "0x42"));
    f.fail[WriteOp] = false;
    select_row(&c, 1);
    press(&c, BedjetRight);
    assert(f.write_count == 2 && f.writes[1][1] == 61);
    stop(&c);
}
static void test_coalesced_write_failure_or_cancellation(void) {
    for(unsigned cancel = 0; cancel < 2; cancel++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        select_row(&c, 1);
        press(&c, BedjetRight);
        press(&c, BedjetRight);
        press(&c, BedjetRight);
        if(cancel) {
            f.cancel_enabled = true;
            f.cancel_during = WriteOp;
        } else
            f.fail[WriteOp] = true;
        f.notification[8] = 61;
        observe(&c, &f);
        assert(f.write_count == 2 && f.writes[1][1] == 63);
        assert(c.model.command.state == (cancel ? BedjetCommandCancelled : BedjetCommandRejected));
        if(cancel) {
            step(&c, 2);
            connect_device(&c, &f);
        } else {
            f.notification[8] = 63;
            observe(&c, &f);
            assert(c.model.command.state == BedjetCommandRejected);
        }
        step(&c, 20);
        assert(f.write_count == 2);
        stop(&c);
    }
}

static void test_preset_limits_wait_for_settled_status(void) {
    for(unsigned path = 0; path < 3; path++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        select_row(&c, 4);
        press(&c, BedjetOk);
        assert(f.write_count == 1 && c.model.command.state == BedjetCommandUnverified);
        if(path == 1) f.notification[9] = BedjetModeExtHeat;
        if(path == 2) f.notification[9] = BedjetModeWait;
        observe(&c, &f);
        /* A newer frame still reporting the pre-press mode, or Wait, keeps the
         * temperature and timer limits locked; a mode change unlocks them. */
        assert(c.model.limits_ready == (path == 1));
        select_row(&c, 5);
        press(&c, BedjetOk);
        assert(f.write_count == 1 || path == 1);
        if(path != 1) assert(strstr(c.model.message, "Wait for preset status"));
        if(path == 1) {
            stop(&c);
            continue;
        }
        select_row(&c, 1);
        press(&c, BedjetRight);
        assert(f.write_count == 1);
        advance(&f, BEDJET_PRESET_SETTLE_MS);
        observe(&c, &f);
        assert(c.model.limits_ready == (path == 0));
        if(path == 2) {
            f.notification[9] = BedjetModeExtHeat;
            observe(&c, &f);
            assert(c.model.limits_ready);
        }
        press(&c, BedjetRight);
        assert(f.write_count == 2 && f.writes[1][0] == 3 && f.writes[1][1] == 61);
        stop(&c);
    }
}
static void test_timed_out_mode_or_off_allows_preset(void) {
    for(unsigned off = 0; off < 2; off++) {
        BedjetController c;
        Fake f;
        ready(&c, &f);
        draft_cool(&c);
        press(&c, off ? BedjetOff : BedjetOk);
        assert(f.write_count == 1 && f.writes[0][1] == (off ? 1 : 2));
        advance(&f, BEDJET_COMMAND_TIMEOUT_MS);
        step(&c, 1);
        assert(c.model.command.state == BedjetCommandTimedOut);
        observe(&c, &f);
        assert(!c.model.limits_ready && bedjet_controller_fresh(&c.model, clock_ms(&f)));
        select_row(&c, 4);
        press(&c, BedjetOk);
        assert(f.write_count == 2 && f.writes[1][0] == 1 && f.writes[1][1] == 0x20);
        stop(&c);
    }
}
static void test_off_timeout_is_never_retried(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    press(&c, BedjetOff);
    assert(f.write_count == 1 && c.model.command.kind == BedjetCommandOff);
    advance(&f, BEDJET_COMMAND_TIMEOUT_MS);
    step(&c, 1);
    assert(c.model.command.state == BedjetCommandTimedOut);
    for(unsigned i = 0; i < 5; i++)
        observe(&c, &f);
    step(&c, 20);
    assert(f.write_count == 1 && c.model.command.state == BedjetCommandTimedOut);
    f.link = false;
    step(&c, 3);
    connect_device(&c, &f);
    step(&c, 20);
    assert(f.write_count == 1);
    stop(&c);
}
static void test_timer_tolerance_boundaries(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, 3);
    press(&c, BedjetOk);
    assert(f.write_count == 1 && f.writes[0][1] == 0 && f.writes[0][2] == 30);
    advance(&f, 999);
    set_remaining(&f, 1796); /* 1 s elapsed: 1800 s - (1 + 2) is the lowest match. */
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandPending);
    set_remaining(&f, 1801); /* Never above the requested runtime. */
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandPending);
    set_remaining(&f, 1797);
    observe(&c, &f);
    assert(c.model.command.state == BedjetCommandApplied && f.write_count == 1);
    stop(&c);
}
static void test_scan_stop_retry_budget(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    f.fail[ScanStopOp] = true;
    advance(&f, 8000);
    step(&c, BEDJET_CLEANUP_ATTEMPTS - 1);
    assert(c.phase == BedjetScan && strstr(c.model.message, "retrying"));
    step(&c, 1);
    assert(c.phase == BedjetDisconnecting && c.model.screen == Closing);
    assert(strstr(c.model.message, "Scan stop"));
    step(&c, 2 * BEDJET_CLEANUP_ATTEMPTS);
    assert(c.phase == BedjetDone && !f.write_count);
    assert(f.calls[ScanStopOp] == 2 * BEDJET_CLEANUP_ATTEMPTS);
}
static void test_scan_end_clears_residue(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    BedjetAdvertisement a = advertisement(1);
    emit_advertisement(&f, &a);
    step(&c, 1);
    for(unsigned i = 0; i < BEDJET_QUEUE_CAPACITY; i++)
        emit_advertisement(&f, &a);
    assert(c.queue_count == BEDJET_QUEUE_CAPACITY);
    f.scan_during_stop = true;
    advance(&f, 8000);
    step(&c, 1);
    assert(c.model.screen == Devices && c.model.count == 1 && !c.queue_count);
    unsigned dropped = atomic_load(&c.dropped_events);
    assert(bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetDown));
    assert(bedjet_controller_post_key(&c, bedjet_controller_session(&c), BedjetDown));
    assert(atomic_load(&c.dropped_events) == dropped);
    stop(&c);
}
static void test_inherited_radio_state_at_boot(void) {
    for(unsigned stuck = 0; stuck < 2; stuck++) {
        BedjetController c;
        Fake f;
        configure(&c, &f);
        f.scanning = f.link = true;
        f.fail[ScanStopOp] = stuck;
        step(&c, 1);
        assert(c.phase == BedjetIdle && c.model.screen == Home);
        assert(f.calls[ScanStopOp] == 1 && f.calls[DisconnectOp] == 1 && !f.link);
        assert(strstr(c.model.message, stuck ? "BT busy" : "Cleared old"));
        if(!stuck) start_scan(&c);
        assert(!f.write_count);
        stop(&c);
    }
}

static void test_status_read_tail_is_recorded_not_applied(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    f.short_read = true; /* References read an 11-byte tail, not a status packet. */
    connect_device(&c, &f);
    BedjetDiagnostics* d = &c.model.diagnostics;
    assert(!c.model.has_status && d->reads_ok == 1 && d->read_len == 11);
    assert(d->read_result == BedjetStatusLength && !d->frames_rejected && !d->frames_ok);
    assert(!memcmp(d->read, packet, 11) && strstr(c.model.message, "Waiting"));
    unsigned subscribes = f.calls[SubscribeOp];
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 1);
    assert(f.calls[SubscribeOp] == subscribes + 1 && f.calls[ReadOp] == 2 && d->reads_ok == 2);
    assert(strstr(c.model.message, "read 11 bytes (length)"));
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 1);
    assert(f.calls[ReadOp] == 2 && strstr(c.model.message, "wait a moment"));
    advance(&f, BEDJET_REFRESH_MIN_MS);
    f.short_read = false;
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 1);
    assert(f.calls[ReadOp] == 3 && d->read_result == BedjetStatusOk && c.model.has_status);
    f.fail_fast[ReadOp] = true;
    advance(&f, BEDJET_REFRESH_MIN_MS);
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 1);
    assert(d->reads_failed == 1 && d->ble_status == 0x42 && strstr(c.model.message, "failed"));
    assert(c.phase == BedjetReady);
    assert(!f.write_count);
    stop(&c);
}
static void test_rejected_notifications_are_captured(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    BedjetDiagnostics* d = &c.model.diagnostics;
    unsigned ok = d->frames_ok;
    f.notification[9] = 7; /* Undocumented mode: never applied, but kept as evidence. */
    observe(&c, &f);
    observe(&c, &f);
    assert(
        d->frames_rejected == 2 && d->rejected_since_ok == 2 &&
        d->last_reason == BedjetStatusMode);
    assert(d->rejected_len == 32 && d->rejected[9] == 7 && c.model.status.mode == 1);
    assert(d->last_frame_ms == clock_ms(&f));
    f.notification[9] = 1;
    observe(&c, &f);
    assert(d->frames_ok == ok + 1 && !d->rejected_since_ok && d->accepted[9] == 1);
    assert(!f.write_count);
    f.link = false;
    step(&c, 3);
    connect_device(&c, &f);
    /* Evidence is per connection: a new session starts from zero. */
    assert(!d->frames_rejected && d->reads_ok == 1 && d->frames_ok <= 1);
    stop(&c);
}

static BedjetAdvertisement report(unsigned address, uint8_t event, const uint8_t* ad, uint8_t n) {
    BedjetAdvertisement a = advertisement(address);
    a.event_type = event;
    a.data_len = n;
    memset(a.data, 0, sizeof(a.data));
    memcpy(a.data, ad, n);
    return a;
}
static void test_scan_response_merges_into_unit(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    uint8_t uuid[18] = {17, 7};
    memcpy(uuid + 2, bedjet_service_uuid, 16);
    const uint8_t name[] = {8, 9, 'B', 'e', 'd', 'r', 'o', 'o', 'm'};
    BedjetAdvertisement unknown = report(9, BedjetAdvScanRsp, name, sizeof(name));
    /* A name-only scan response for an address not yet matched is filtered. */
    assert(!bedjet_controller_post_advertisement(&c, bedjet_controller_session(&c), &unknown));
    BedjetAdvertisement primary = report(1, BedjetAdvInd, uuid, sizeof(uuid));
    BedjetAdvertisement response = report(1, BedjetAdvScanRsp, name, sizeof(name));
    response.rssi = -61;
    emit_advertisement(&f, &primary);
    emit_advertisement(&f, &response);
    step(&c, 2);
    assert(c.model.count == 1 && !strcmp(c.model.devices[0].name, "Bedroom"));
    uint8_t want = BedjetDeviceService | BedjetDeviceConnectable | BedjetDeviceCompleteName;
    assert(c.model.devices[0].flags == want && c.model.devices[0].rssi == -61);
    /* The next primary keeps the merged name rather than overwriting it. */
    emit_advertisement(&f, &primary);
    step(&c, 1);
    assert(!strcmp(c.model.devices[0].name, "Bedroom") && c.model.devices[0].rssi == -50);
    stop(&c);
}
static void test_unrelated_flood_cannot_evict_bedjet(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    const uint8_t other[] = {6, 9, 'O', 't', 'h', 'e', 'r'};
    unsigned dropped = atomic_load(&c.dropped_events);
    for(unsigned i = 0; i < 4 * BEDJET_QUEUE_CAPACITY; i++) {
        BedjetAdvertisement a = report(10 + i, BedjetAdvInd, other, sizeof(other));
        emit_advertisement(&f, &a);
    }
    BedjetAdvertisement bedjet = advertisement(1);
    emit_advertisement(&f, &bedjet);
    assert(c.queue_count == 1 && atomic_load(&c.dropped_events) == dropped);
    /* Diagnostics account for every report: one match, the flood filtered. */
    assert(atomic_load(&c.scan_matched) == 1);
    assert(atomic_load(&c.scan_filtered) == 4 * BEDJET_QUEUE_CAPACITY);
    assert(atomic_load(&c.scan_refused) == 0);
    advance(&f, 8000);
    step(&c, 2);
    assert(c.model.screen == Devices && c.model.count == 1);
    /* A report after the scan closed is refused and counted as such. */
    assert(!bedjet_controller_post_advertisement(&c, bedjet_controller_session(&c), &bedjet));
    assert(atomic_load(&c.scan_refused) == 1 && atomic_load(&c.scan_matched) == 1);
    stop(&c);
}
static void test_device_names_flags_and_overflow(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    start_scan(&c);
    const uint8_t noisy[] = {25,  8,   'B', 'E', 'D', 'J', 'E', 'T', 1,   'x', 0x7f, 'a', 'b',
                             'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',  'n', 'o'};
    BedjetAdvertisement a = report(1, BedjetAdvNonConnInd, noisy, sizeof(noisy));
    emit_advertisement(&f, &a);
    step(&c, 1);
    BedjetDevice* d = &c.model.devices[0];
    /* Short name: sanitized, truncated to 20 characters, no connectable claim. */
    assert(c.model.count == 1 && !strcmp(d->name, "BEDJET?x?abcdefghijk"));
    assert(d->flags == BedjetDeviceNamed);
    const uint8_t complete[] = {10, 9, 'B', 'E', 'D', 'J', 'E', 'T', '_', 'V', '3'};
    a = report(1, BedjetAdvScanRsp, complete, sizeof(complete));
    emit_advertisement(&f, &a);
    step(&c, 1);
    a = report(1, BedjetAdvNonConnInd, noisy, sizeof(noisy));
    emit_advertisement(&f, &a);
    step(&c, 1);
    assert(!strcmp(d->name, "BEDJET_V3") && (d->flags & BedjetDeviceCompleteName));
    for(unsigned i = 2; i <= BEDJET_DEVICE_COUNT + 1; i++) {
        a = advertisement(i);
        emit_advertisement(&f, &a);
        step(&c, 1);
    }
    assert(c.model.count == BEDJET_DEVICE_COUNT && c.model.more_devices);
    stop(&c);
}
static void test_setup_failure_messages(void) {
    enum {
        Timeout,
        Fast,
        Drop,
        NoCccd
    };
    const struct {
        Operation op;
        int how;
        uint8_t status;
        const char* text;
    } cases[] = {
        {ConnectOp, Timeout, 0, "Connect: no reply"},
        {ConnectOp, Fast, 0, "Connect failed: app open?"},
        {ConnectOp, Fast, 0x0C, "Connect: BLE 0x0C"},
        {CommandOp, Fast, 0, "Discovery failed: no cmd"},
        {CommandOp, Timeout, 0, "Find cmd: no reply"},
        {StatusOp, Fast, 0, "Discovery failed: status"},
        {StatusOp, Drop, 0, "Find status: link lost"},
        {OpCount, NoCccd, 0, "Setup failed: no notify"},
        {SubscribeOp, Fast, 0, "Subscribe failed (pair?)"},
        {SubscribeOp, Fast, 0x46, "Subscribe: BLE 0x46"},
        {SubscribeOp, Timeout, 0x42, "Subscribe: no reply"},
    };
    for(unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        BedjetController c;
        Fake f;
        configure(&c, &f);
        step(&c, 1);
        device_list(&c, &f);
        f.status = cases[i].status;
        if(cases[i].how == Timeout) f.fail[cases[i].op] = true;
        if(cases[i].how == Fast || cases[i].how == Drop) f.fail_fast[cases[i].op] = true;
        f.drop_on_fail = cases[i].how == Drop;
        f.no_cccd = cases[i].how == NoCccd;
        press(&c, BedjetOk);
        assert(!strcmp(c.model.message, "Connecting..."));
        step(&c, 10);
        assert(c.model.screen == Home && !strcmp(c.model.message, cases[i].text));
        assert(c.model.diagnostics.ble_status == cases[i].status && !f.write_count && !f.link);
        stop(&c);
    }
}
/* A timed-out GATT call leaves the firmware link unusable while it may still
 * report connected. The app disconnects instead of failing every control, and
 * a new connection replays nothing. A fast failure keeps the session. */
static void test_timed_out_call_drops_stuck_link(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    BedjetDiagnostics* d = &c.model.diagnostics;
    f.fail[ReadOp] = true;
    advance(&f, BEDJET_REFRESH_MIN_MS);
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 1);
    assert(d->reads_failed == 1 && !c.model.connected && c.phase == BedjetDisconnecting);
    assert(!strcmp(c.model.message, "No reply: reconnect"));
    step(&c, 1);
    assert(c.model.screen == Home && !f.link && f.calls[DisconnectOp] == 1);
    assert(!strcmp(c.model.message, "No reply: reconnect") && !f.write_count);
    f.fail[ReadOp] = false;
    connect_device(&c, &f);
    assert(c.model.has_status && !f.write_count);
    stop(&c);

    /* Same for the resubscribe of a manual refresh. */
    ready(&c, &f);
    f.fail[SubscribeOp] = true;
    advance(&f, BEDJET_REFRESH_MIN_MS);
    unsigned reads = f.calls[ReadOp];
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 2);
    assert(c.model.screen == Home && f.calls[ReadOp] == reads && !f.link);
    assert(!strcmp(c.model.message, "No reply: reconnect") && !f.write_count);
    stop(&c);

    /* And for the initial read: the control screen is never published. */
    configure(&c, &f);
    step(&c, 1);
    device_list(&c, &f);
    f.fail[ReadOp] = true;
    press(&c, BedjetOk);
    step(&c, 10);
    assert(c.model.screen == Home && !c.model.connected && !f.link);
    assert(!strcmp(c.model.message, "No reply: reconnect") && !f.write_count);
    stop(&c);

    /* A refresh that fails fast leaves a usable link alone. */
    ready(&c, &f);
    f.fail_fast[ReadOp] = true;
    f.status = 0;
    advance(&f, BEDJET_REFRESH_MIN_MS);
    assert(bedjet_controller_refresh(&c, bedjet_controller_session(&c)));
    step(&c, 1);
    assert(c.phase == BedjetReady && c.model.connected && f.calls[DisconnectOp] == 0);
    assert(strstr(c.model.message, "read failed"));
    stop(&c);
}

/* Replays the pinned input service: Long at 300 ms, Repeat every 150 ms from
 * 450 ms, Short only when released before Long, then Release. The worker
 * ticks between events as it would on the device. */
static void input_hold(BedjetController* c, Fake* f, BedjetKey key, uint32_t ms) {
    uint32_t elapsed = 0;
    bedjet_controller_input(c, key, BedjetInputPress, clock_ms(f));
    for(uint32_t at = BEDJET_LONG_PRESS_MS; at <= ms; at += 150) {
        advance(f, at - elapsed);
        elapsed = at;
        BedjetInputType type = at == BEDJET_LONG_PRESS_MS ? BedjetInputLong : BedjetInputRepeat;
        bedjet_controller_input(c, key, type, clock_ms(f));
        step(c, 1);
    }
    advance(f, ms - elapsed);
    if(ms < BEDJET_LONG_PRESS_MS) bedjet_controller_input(c, key, BedjetInputShort, clock_ms(f));
    bedjet_controller_input(c, key, BedjetInputRelease, clock_ms(f));
    step(c, 1);
}
static void test_off_needs_a_deliberate_hold(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, BedjetRowM2);
    /* A 0.35 s press on a preset row starts neither the preset nor Off. */
    input_hold(&c, &f, BedjetOk, 350);
    assert(!f.write_count && strstr(c.model.message, "Hold OK 1 s"));
    input_hold(&c, &f, BedjetOk, 900);
    assert(!f.write_count);
    input_hold(&c, &f, BedjetOk, 1100);
    assert(f.write_count == 1 && f.writes[0][0] == 1 && f.writes[0][1] == 1);
    f.notification[9] = 0;
    observe(&c, &f);
    input_hold(&c, &f, BedjetOk, 2500); /* One hold sends Off at most once. */
    assert(f.write_count == 2 && f.writes[1][1] == 1);
    observe(&c, &f);
    input_hold(&c, &f, BedjetOk, 100); /* A short press keeps its row action. */
    assert(f.write_count == 3 && f.writes[2][1] == 0x21);
    stop(&c);
}
static void test_long_presses_count_and_back_acts_on_press(void) {
    BedjetController c;
    Fake f;
    configure(&c, &f);
    step(&c, 1);
    input_hold(&c, &f, BedjetOk, 400); /* No Short follows Long: still OK at Home. */
    step(&c, 1);
    assert(c.phase == BedjetScan);
    stop(&c);
    ready(&c, &f);
    input_hold(&c, &f, BedjetDown, 100);
    assert(c.model.row == 1);
    input_hold(&c, &f, BedjetDown, 350); /* Long alone is one step. */
    assert(c.model.row == 2);
    input_hold(&c, &f, BedjetUp, 800); /* Long plus Repeats at 450, 600, 750 ms. */
    assert(c.model.row == (2 + BedjetRowCount - 4) % BedjetRowCount);
    select_row(&c, BedjetRowFan);
    input_hold(&c, &f, BedjetRight, 350);
    assert(f.write_count == 1 && f.writes[0][0] == 7 && f.writes[0][1] == 10);
    assert(!bedjet_controller_input(&c, BedjetBack, BedjetInputShort, clock_ms(&f)));
    assert(bedjet_controller_input(&c, BedjetBack, BedjetInputPress, clock_ms(&f)));
    step(&c, 1);
    assert(c.model.screen == Home && !c.model.connected && f.write_count == 1);
    stop(&c);
}
static void test_mode_draft_follows_observed_mode(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    assert(c.model.row == BedjetRowMode && c.model.mode_choice == 1); /* Heat */
    press(&c, BedjetOk);
    assert(!f.write_count && !strcmp(c.model.message, "Already Heat"));
    f.notification[9] = BedjetModeDry; /* RF remote */
    observe(&c, &f);
    assert(c.model.mode_choice == 2);
    f.notification[9] = BedjetModeTurbo; /* Not selectable here: keep the draft. */
    observe(&c, &f);
    assert(c.model.mode_choice == 2);
    press(&c, BedjetRight);
    press(&c, BedjetOk);
    assert(f.write_count == 1 && f.writes[0][1] == 6); /* Extended heat */
    stop(&c);
}
static void test_units_and_info_rows_never_write(void) {
    BedjetController c;
    Fake f;
    ready(&c, &f);
    select_row(&c, BedjetRowUnits);
    press(&c, BedjetOk);
    assert(c.model.celsius);
    press(&c, BedjetLeft);
    assert(!c.model.celsius);
    select_row(&c, BedjetRowInfo);
    press(&c, BedjetRight);
    assert(c.model.info_page == 1);
    press(&c, BedjetLeft);
    press(&c, BedjetLeft);
    assert(c.model.info_page == BEDJET_INFO_PAGES - 1);
    unsigned reads = f.calls[ReadOp];
    press(&c, BedjetOk); /* Refresh: resubscribe and read once. */
    assert(f.calls[ReadOp] == reads + 1 && strstr(c.model.message, "Refresh"));
    select_row(&c, BedjetRowOff);
    press(&c, BedjetOk);
    assert(strstr(c.model.message, "Hold OK 1 s") && !f.write_count);
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
    RUN(test_priority_off_saturated_rows);
    RUN(test_priority_off_contended_inbox);
    RUN(test_priority_ordering);
    RUN(test_priority_off_busy_and_failed);
    RUN(test_off_during_read_and_write);
    RUN(test_back_during_off_and_refresh);
    RUN(test_notification_flood_back_and_lost_link_off);
    RUN(test_scan_boundary_cancellation);
    RUN(test_repeated_back_failed_teardown);
    RUN(test_concurrent_priority_producers);
    RUN(test_off_without_fresh_status);
    RUN(test_rapid_temperature_intent);
    RUN(test_fan_coalescing_clamp_and_reversal);
    RUN(test_no_redundant_coalesced_write);
    RUN(test_confirmation_requires_new_receive_time);
    RUN(test_timeout_discards_intent_and_old_keys);
    RUN(test_rejected_write_stays_rejected);
    RUN(test_mode_limits_require_matching_status);
    RUN(test_mode_timeout_and_late_limits);
    RUN(test_memory_is_unverified_and_limits_gate);
    RUN(test_timer_confirmation_and_device_limit_rejection);
    RUN(test_changed_bounds_clamp_or_drop_intent);
    RUN(test_external_remote_conflict_policy);
    RUN(test_off_preempts_coalesced_setting);
    RUN(test_pending_setting_never_replays_after_reconnect);
    RUN(test_command_clock_wrap_and_read_confirmation);
    RUN(test_confirmation_lost_to_queue_overflow);
    RUN(test_mode_discards_pre_dispatch_inputs);
    RUN(test_write_return_is_confirmation_boundary);
    RUN(test_delayed_confirmation_cannot_revive_timeout);
    RUN(test_rf_mode_switch_discards_older_inputs);
    RUN(test_zero_timer_limit_and_failed_mode_resync);
    RUN(test_coalesced_write_failure_or_cancellation);
    RUN(test_preset_limits_wait_for_settled_status);
    RUN(test_timed_out_mode_or_off_allows_preset);
    RUN(test_off_timeout_is_never_retried);
    RUN(test_timer_tolerance_boundaries);
    RUN(test_scan_stop_retry_budget);
    RUN(test_scan_end_clears_residue);
    RUN(test_inherited_radio_state_at_boot);
    RUN(test_status_read_tail_is_recorded_not_applied);
    RUN(test_rejected_notifications_are_captured);
    RUN(test_scan_response_merges_into_unit);
    RUN(test_unrelated_flood_cannot_evict_bedjet);
    RUN(test_device_names_flags_and_overflow);
    RUN(test_setup_failure_messages);
    RUN(test_timed_out_call_drops_stuck_link);
    RUN(test_off_needs_a_deliberate_hold);
    RUN(test_long_presses_count_and_back_acts_on_press);
    RUN(test_mode_draft_follows_observed_mode);
    RUN(test_units_and_info_rows_never_write);
    printf(
        "PASS: %u lifecycle scenarios; controller=%zu bytes, model=%zu bytes\n",
        count,
        sizeof(BedjetController),
        sizeof(BedjetModel));
    return 0;
}
