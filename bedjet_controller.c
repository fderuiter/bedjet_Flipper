// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_controller.h"
#include <stdio.h>
#include <string.h>

_Static_assert(
    ATOMIC_BOOL_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
    "Callback inbox requires lock-free atomics");
_Static_assert(sizeof(BedjetController) <= 4096, "Controller exceeds its fixed memory budget");

enum {
    BackPending = 1u,
    OffPending = 2u,
    PriorityMask = 3u,
    SessionStep = 4u
};

static uint32_t now(BedjetController* c) {
    return c->transport.now_ms(c->transport_context);
}
uint32_t bedjet_controller_session(const BedjetController* c) {
    return atomic_load(&c->priority) & ~PriorityMask;
}
static bool pending(BedjetController* c) {
    return atomic_load(&c->shutdown_requested) || (atomic_load(&c->priority) & PriorityMask);
}
static bool cancelled(BedjetController* c) {
    return atomic_load(&c->shutdown_requested) || (atomic_load(&c->priority) & BackPending);
}
static bool request_priority(BedjetController* c, uint32_t session, unsigned flag) {
    unsigned state = atomic_load(&c->priority);
    /* Each of two bits can only be added once in a generation. Three strong
     * CAS attempts suffice: a failed attempt either observes another bit or a
     * new session (rejected). No spinning on a producer-held lock. */
    for(unsigned i = 0; i < 3; i++) {
        if((state & ~PriorityMask) != session) return false;
        if(flag == OffPending && (state & BackPending)) return false;
        if(state & flag) return true;
        if(atomic_compare_exchange_strong(&c->priority, &state, state | flag)) return true;
    }
    return false;
}
bool bedjet_controller_fresh(const BedjetModel* m, uint32_t time) {
    return m->has_status && time - m->status_ms < BEDJET_STATUS_FRESH_MS;
}

static bool queue_lock(BedjetController* c) {
    bool expected = false;
    return atomic_compare_exchange_strong_explicit(
        &c->queue_busy, &expected, true, memory_order_acquire, memory_order_relaxed);
}
static void queue_unlock(BedjetController* c) {
    atomic_store_explicit(&c->queue_busy, false, memory_order_release);
}
static bool post(BedjetController* c, const BedjetEvent* e) {
    if(e->session != bedjet_controller_session(c)) return false;
    if(!queue_lock(c)) {
        atomic_fetch_add(&c->dropped_events, 1);
        return false;
    }
    bool ok = c->queue_count < BEDJET_QUEUE_CAPACITY;
    if(ok) {
        c->queue[(c->queue_head + c->queue_count) % BEDJET_QUEUE_CAPACITY] = *e;
        c->queue_count++;
    } else
        atomic_fetch_add(&c->dropped_events, 1);
    queue_unlock(c);
    return ok;
}
static bool pop(BedjetController* c, BedjetEvent* e) {
    if(!queue_lock(c)) return false;
    bool ok = c->queue_count != 0;
    if(ok) {
        *e = c->queue[c->queue_head];
        c->queue_head = (c->queue_head + 1) % BEDJET_QUEUE_CAPACITY;
        c->queue_count--;
    }
    queue_unlock(c);
    return ok;
}
static bool command_active(const BedjetCommand* command) {
    return command->state == BedjetCommandPending || command->state == BedjetCommandQueued;
}
static void command_result(BedjetController* c, const char* text) {
    snprintf(c->model.command.result, sizeof(c->model.command.result), "%s", text);
    snprintf(c->model.message, sizeof(c->model.message), "%s", c->model.command.result);
}
static void finish_command(BedjetController* c, BedjetCommandState state, const char* text) {
    c->model.command.state = state;
    c->model.command.desired = c->model.command.sent;
    /* Discard raw inputs captured before failure/conflict/timeout as well as
     * the coalesced target. A delayed key must not become an implicit retry. */
    atomic_fetch_add(&c->input_epoch, 1);
    command_result(c, text);
}
static void clear_queue(BedjetController* c) {
    /* Old events are rejected by session/epoch even if a producer holds the
     * inbox briefly. Clearing is only an optimization; never spin in callbacks. */
    if(queue_lock(c)) {
        c->queue_count = 0;
        c->queue_head = 0;
        queue_unlock(c);
    }
}
static void invalidate_state(BedjetController* c, bool clear_status) {
    atomic_store(&c->accepting_input, false);
    atomic_store(&c->accepting_callbacks, false);
    atomic_store(&c->control_ready, false);
    clear_queue(c);
    if(command_active(&c->model.command))
        finish_command(c, BedjetCommandCancelled, "Command cancelled");
    c->model.limits_ready = false;
    c->limits_waiting = false;
    if(clear_status) c->model.has_status = false;
}
static unsigned next_session(unsigned state) {
    unsigned next = (state & ~PriorityMask) + SessionStep;
    return next ? next : SessionStep;
}
static void invalidate(BedjetController* c, bool clear_status) {
    /* Close admission before exposing the replacement token, as try_invalidate
     * does. Otherwise a producer could read the new token while input is still
     * open and admit Off or a key into the cleanup generation. */
    atomic_store(&c->accepting_input, false);
    atomic_store(&c->control_ready, false);
    atomic_store(&c->priority, next_session(atomic_load(&c->priority)));
    invalidate_state(c, clear_status);
}
static bool try_invalidate(BedjetController* c, bool clear_status, unsigned flags) {
    unsigned state = atomic_load(&c->priority);
    if((state & PriorityMask) != flags) return false;
    /* Close input before exposing the replacement token. Producers which had
     * already passed admission still carry the old token and will be stale. */
    atomic_store(&c->accepting_input, false);
    atomic_store(&c->control_ready, false);
    /* Starting scan/connect or servicing Off must never erase a racing Back.
     * A failed CAS leaves the priority request for apply_cancellation. */
    if(!atomic_compare_exchange_strong(&c->priority, &state, next_session(state))) return false;
    invalidate_state(c, clear_status);
    return true;
}

bool bedjet_controller_post_key(BedjetController* c, uint32_t session, BedjetKey key) {
    if(session != bedjet_controller_session(c) || atomic_load(&c->shutdown_requested))
        return false;
    if(key == BedjetBack) return request_priority(c, session, BackPending);
    if(key == BedjetOff)
        return atomic_load(&c->control_ready) && request_priority(c, session, OffPending);
    if((unsigned)key > (unsigned)BedjetOffHint || !atomic_load(&c->accepting_input) || pending(c))
        return false;
    BedjetEvent e = {
        .type = BedjetEventKey,
        .session = session,
        .input_epoch = atomic_load(&c->input_epoch),
        .value.key = key};
    return post(c, &e);
}
/* Parse AD records: returns BedjetDeviceService/Named flags and, when `name`
 * is given, copies a printable local name (complete preferred over short).
 * A malformed record ends parsing without trusting anything after it. */
static uint8_t advertisement_identity(const BedjetAdvertisement* a, BedjetDevice* name) {
    uint8_t flags = 0;
    for(size_t i = 0; i < a->data_len;) {
        size_t size = a->data[i++];
        if(!size || size > a->data_len - i) break;
        uint8_t type = a->data[i];
        const uint8_t* value = &a->data[i + 1];
        size_t len = size - 1;
        if(type == 0x08 || type == 0x09) {
            bool match = len >= 6;
            for(size_t j = 0; match && j < 6; j++) {
                uint8_t ch = value[j];
                if(ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                match = ch == (uint8_t) "BEDJET"[j];
            }
            if(match) flags |= BedjetDeviceNamed;
            bool complete = type == 0x09;
            if(name && len && (complete || !(name->flags & BedjetDeviceCompleteName))) {
                size_t n = len < sizeof(name->name) - 1 ? len : sizeof(name->name) - 1;
                for(size_t j = 0; j < n; j++)
                    name->name[j] = value[j] >= 0x20 && value[j] < 0x7f ? (char)value[j] : '?';
                name->name[n] = '\0';
                if(complete) name->flags |= BedjetDeviceCompleteName;
            }
        }
        if(type == 0x06 || type == 0x07)
            for(size_t j = 0; j + 16 <= len; j += 16)
                if(!memcmp(value + j, bedjet_service_uuid, 16)) flags |= BedjetDeviceService;
        i += size;
    }
    return flags;
}
static bool scan_seen(const BedjetController* c, const BedjetAdvertisement* a) {
    for(unsigned i = 0; i < c->scan_seen_count; i++)
        if(c->scan_seen[i].addr_type == a->addr_type && !memcmp(c->scan_seen[i].addr, a->addr, 6))
            return true;
    return false;
}
bool bedjet_controller_input(
    BedjetController* c,
    BedjetKey key,
    BedjetInputType type,
    uint32_t time) {
    uint32_t session = bedjet_controller_session(c);
    if(key == BedjetBack)
        return type == BedjetInputPress && bedjet_controller_post_key(c, session, BedjetBack);
    if(key == BedjetOk) {
        bool control = atomic_load(&c->control_ready);
        switch(type) {
        case BedjetInputPress:
            c->ok_press_ms = time;
            c->ok_down = true;
            c->ok_long = c->ok_off_sent = false;
            return false;
        case BedjetInputShort:
            return bedjet_controller_post_key(c, session, BedjetOk);
        case BedjetInputLong:
            c->ok_long = true;
            /* No Short follows a Long. Off screens, a slightly long press is OK. */
            return !control && bedjet_controller_post_key(c, session, BedjetOk);
        case BedjetInputRepeat:
            if(!control || !c->ok_down || c->ok_off_sent ||
               time - c->ok_press_ms < BEDJET_OFF_HOLD_MS)
                return false;
            c->ok_off_sent = true;
            return bedjet_controller_post_key(c, session, BedjetOff);
        case BedjetInputRelease: {
            bool hint = c->ok_down && c->ok_long && !c->ok_off_sent && control;
            c->ok_down = false;
            return hint && bedjet_controller_post_key(c, session, BedjetOffHint);
        }
        }
        return false;
    }
    if(key > BedjetRight) return false;
    if(type == BedjetInputShort || type == BedjetInputLong || type == BedjetInputRepeat)
        return bedjet_controller_post_key(c, session, key);
    return false;
}
bool bedjet_controller_post_advertisement(
    BedjetController* c,
    uint32_t session,
    const BedjetAdvertisement* a) {
    if(!a || a->data_len > sizeof(a->data) || !atomic_load(&c->accepting_callbacks)) {
        atomic_fetch_add(&c->scan_refused, 1);
        return false;
    }
    /* Filter before the lossy inbox so unrelated devices cannot crowd out a
     * BedJet. A scan response may carry only a custom name, so it passes when
     * its address already matched in this scan. The cache is touched only by
     * the single scan producer (see BedjetController). */
    if(advertisement_identity(a, NULL)) {
        atomic_fetch_add(&c->scan_matched, 1);
        if(!scan_seen(c, a) && c->scan_seen_count < BEDJET_DEVICE_COUNT) {
            memcpy(c->scan_seen[c->scan_seen_count].addr, a->addr, 6);
            c->scan_seen[c->scan_seen_count++].addr_type = a->addr_type;
        }
    } else if(a->event_type != BedjetAdvScanRsp || !scan_seen(c, a)) {
        atomic_fetch_add(&c->scan_filtered, 1);
        return false;
    }
    BedjetEvent e = {
        .type = BedjetEventAdvertisement, .session = session, .value.advertisement = *a};
    return post(c, &e);
}
bool bedjet_controller_post_status(
    BedjetController* c,
    uint32_t session,
    uint16_t handle,
    const uint8_t* bytes,
    size_t length,
    uint32_t time) {
    if(!bytes || !atomic_load(&c->accepting_callbacks)) return false;
    if(length > sizeof(((BedjetEvent*)0)->value.status.bytes)) {
        /* Too large to copy into the inbox: count it rather than drop silently. */
        atomic_fetch_add(&c->oversize_frames, 1);
        return false;
    }
    /* Short frames are admitted so the worker can record them as evidence;
     * the decoder still rejects anything that is not a validated status. */
    BedjetEvent e = {.type = BedjetEventStatus, .session = session, .received_ms = time};
    e.value.status.handle = handle;
    e.value.status.length = length;
    memcpy(e.value.status.bytes, bytes, length);
    return post(c, &e);
}
bool bedjet_controller_refresh(BedjetController* c, uint32_t session) {
    if(!atomic_load(&c->accepting_input) || pending(c)) return false;
    BedjetEvent e = {.type = BedjetEventRefresh, .session = session};
    return post(c, &e);
}
void bedjet_controller_shutdown(BedjetController* c) {
    atomic_store(&c->shutdown_requested, true);
}

static void scan_sink(void* context, const BedjetAdvertisement* a) {
    BedjetCallbackContext* sink = context;
    bedjet_controller_post_advertisement(sink->controller, sink->session, a);
}
static void notify_sink(void* context, uint16_t handle, const uint8_t* bytes, size_t length) {
    BedjetCallbackContext* sink = context;
    bedjet_controller_post_status(
        sink->controller, sink->session, handle, bytes, length, now(sink->controller));
}
static void message(BedjetController* c, const char* text) {
    snprintf(c->model.message, sizeof(c->model.message), "%s", text);
}
/* The pinned central API reports only synchronous controller results; see
 * docs/upstream-central-accessors.md. Infer why a call failed from what the
 * app observes: the link state, the elapsed time against the call's own wait
 * and the raw status. This is an inference, not a firmware-reported cause. */
typedef enum {
    FailureError, /* failed early with no raw status: peer or procedure error */
    FailureRefused, /* failed early with a raw status: controller refused */
    FailureTimeout, /* used its whole wait: no reply */
    FailureLost, /* the link dropped during the call */
} Failure;
static Failure classify(BedjetController* c, uint32_t started, uint32_t wait_ms, bool linked) {
    uint8_t status = c->transport.last_status(c->transport_context);
    c->model.diagnostics.ble_status = status;
    if(linked && !c->transport.connected(c->transport_context)) return FailureLost;
    /* A procedure timeout also marks the firmware link unusable, even if it
     * still reports connected, so every later GATT call would fail too. */
    if(now(c) - started >= wait_ms) return FailureTimeout;
    return status ? FailureRefused : FailureError;
}
static void begin_disconnect(BedjetController* c, bool failure);
/* After a timed-out GATT call the firmware refuses further procedures but may
 * still report the link as connected. Disconnect now rather than let every
 * control fail; nothing is resent after the user reconnects. */
static bool link_stuck(BedjetController* c, Failure failure) {
    if(failure != FailureTimeout || !c->transport.connected(c->transport_context)) return false;
    begin_disconnect(c, true);
    message(c, "No reply: reconnect");
    return true;
}
static void callbacks_for_session(BedjetController* c) {
    c->callback_context = (BedjetCallbackContext){c, bedjet_controller_session(c)};
    atomic_store(&c->accepting_callbacks, true);
}
static void idle(BedjetController* c) {
    c->phase = BedjetIdle;
    c->model.screen = Home;
    c->model.connected = false;
    c->model.has_status = false;
    c->model.limits_ready = false;
    c->model.command = (BedjetCommand){0};
    c->status_handle = c->command_handle = c->cccd = 0;
    atomic_store(&c->accepting_input, true);
}

bool bedjet_controller_init(BedjetController* c, const BedjetTransport* t, void* context) {
    if(!c || !t || !t->supported || !t->init || !t->is_advertising || !t->advertise ||
       !t->scan_start || !t->scan_stop || !t->is_scanning || !t->connect || !t->connected ||
       !t->disconnect || !t->find || !t->set_notify || !t->subscribe || !t->read || !t->write ||
       !t->deinit || !t->last_status || !t->now_ms)
        return false;
    memset(c, 0, sizeof(*c));
    c->transport = *t;
    c->transport_context = context;
    c->phase = BedjetBoot;
    c->model.timer_choice = 30;
    atomic_init(&c->queue_busy, false);
    atomic_init(&c->shutdown_requested, false);
    atomic_init(&c->accepting_input, false);
    atomic_init(&c->accepting_callbacks, false);
    atomic_init(&c->control_ready, false);
    atomic_init(&c->priority, SessionStep);
    atomic_init(&c->dropped_events, 0);
    atomic_init(&c->input_epoch, 0);
    atomic_init(&c->oversize_frames, 0);
    atomic_init(&c->scan_matched, 0);
    atomic_init(&c->scan_filtered, 0);
    atomic_init(&c->scan_refused, 0);
    return true;
}

static const uint8_t mode_buttons[] = {2, 3, 5, 6};
static const uint8_t mode_values[] = {4, 1, 5, 3};
/* Limits wait for a newer status in a specific mode, in any mode (resync after
 * a failed write), or for a settled status after a preset start. */
#define ANY_MODE    0xffu
#define PRESET_MODE 0xfeu

static void command_message(BedjetController* c, const char* result) {
    BedjetCommand* command = &c->model.command;
    char value[24];
    const char* name;
    switch(command->kind) {
    case BedjetCommandTemperature: {
        unsigned f = bedjet_fahrenheit_tenths(command->desired);
        name = "Temp";
        snprintf(value, sizeof(value), "%u.%uF", f / 10, f % 10);
        break;
    }
    case BedjetCommandFan:
        name = "Fan";
        snprintf(value, sizeof(value), "%u%%", (command->desired + 1) * 5);
        break;
    case BedjetCommandTimer:
        name = "Timer";
        snprintf(value, sizeof(value), "%um", command->desired);
        break;
    case BedjetCommandMode:
        name = "Mode";
        snprintf(value, sizeof(value), "%s", bedjet_mode_name(command->desired));
        break;
    case BedjetCommandMemory:
        name = "Preset";
        snprintf(value, sizeof(value), "M%u", command->desired);
        break;
    default:
        name = "Off";
        value[0] = '\0';
        break;
    }
    char text[64];
    if(value[0])
        snprintf(text, sizeof(text), "%s %s: %s", name, result, value);
    else
        snprintf(text, sizeof(text), "%s %s", name, result);
    command_result(c, text);
}
static void command_timeout(BedjetController* c) {
    if(c->model.command.state == BedjetCommandPending &&
       now(c) - c->model.command.sent_ms >= BEDJET_COMMAND_TIMEOUT_MS)
        finish_command(c, BedjetCommandTimedOut, "Not confirmed; no retry");
}
static uint16_t clamp_target(BedjetController* c, BedjetCommandKind kind, int value) {
    int minimum = kind == BedjetCommandTemperature ? c->model.status.min_step : 0;
    int maximum = kind == BedjetCommandTemperature ? c->model.status.max_step : 19;
    return value < minimum ? minimum : value > maximum ? maximum : value;
}
static bool limits_observed(const BedjetController* c, uint32_t time) {
    const BedjetStatus* s = &c->model.status;
    int32_t age = (int32_t)(time - c->limits_after_ms);
    if(age <= 0) return false;
    if(c->limits_mode == ANY_MODE) return true;
    if(c->limits_mode != PRESET_MODE) return s->mode == c->limits_mode;
    /* Decoded status cannot identify a preset, and its first newer frame can
     * still report the pre-press mode. Unlock on a non-transitional status
     * showing a mode change, or the same mode after a settle window. This is an
     * initial policy pending hardware evidence. */
    return s->mode != BedjetModeWait &&
           (s->mode != c->limits_from_mode || age >= (int32_t)BEDJET_PRESET_SETTLE_MS);
}
static void reconcile_command(BedjetController* c, uint32_t time) {
    BedjetModel* m = &c->model;
    BedjetCommand* command = &m->command;
    if(c->limits_waiting) {
        if(limits_observed(c, time)) {
            c->limits_waiting = false;
            m->limits_ready = true;
        }
    } else
        m->limits_ready = true;
    command_timeout(c);
    if(command->state != BedjetCommandPending || (int32_t)(time - command->sent_ms) <= 0) return;
    if(command->kind == BedjetCommandTemperature || command->kind == BedjetCommandFan ||
       command->kind == BedjetCommandTimer) {
        if(m->status.mode != command->sent_mode) {
            finish_command(c, BedjetCommandConflict, "Mode changed; intent dropped");
            return;
        }
        if((command->kind == BedjetCommandTemperature &&
            (command->sent < m->status.min_step || command->sent > m->status.max_step)) ||
           (command->kind == BedjetCommandTimer && command->sent > m->status.max_minutes)) {
            finish_command(c, BedjetCommandRejected, "Limits changed; intent dropped");
            return;
        }
    }
    bool match = false;
    switch(command->kind) {
    case BedjetCommandTemperature:
        match = m->status.target_step == command->sent;
        break;
    case BedjetCommandFan:
        match = m->status.fan_step == command->sent;
        break;
    case BedjetCommandMode:
    case BedjetCommandOff:
        match = m->status.mode == command->sent;
        break;
    case BedjetCommandTimer: {
        uint32_t seconds = command->sent * 60u;
        uint32_t tolerance = (time - command->sent_ms) / 1000u + 2u;
        match = m->status.remaining_seconds <= seconds &&
                seconds - m->status.remaining_seconds <= tolerance;
        break;
    }
    default:
        break;
    }
    if(!match) return;
    command->state = BedjetCommandApplied;
    if(command->kind == BedjetCommandTemperature || command->kind == BedjetCommandFan) {
        command->desired = clamp_target(c, command->kind, command->desired);
        if(command->desired != command->sent) command->state = BedjetCommandQueued;
    }
    command_message(c, command->state == BedjetCommandQueued ? "queued" : "applied");
}
static void
    record(uint8_t* destination, uint8_t* destination_length, const uint8_t* bytes, size_t n) {
    *destination_length = (uint8_t)(n > 32 ? 32 : n);
    memcpy(destination, bytes, *destination_length);
}
static void
    accept_status(BedjetController* c, const uint8_t* bytes, size_t length, uint32_t time) {
    BedjetDiagnostics* d = &c->model.diagnostics;
    uint32_t current = now(c);
    if(current - time >= BEDJET_STATUS_FRESH_MS ||
       (c->model.has_status && (int32_t)(time - c->model.status_ms) < 0)) {
        d->frames_late++;
        return;
    }
    bool had_status = c->model.has_status;
    uint8_t old_mode = c->model.status.mode;
    BedjetStatusResult result = bedjet_status_parse(bytes, length, &c->model.status);
    d->last_frame_ms = time;
    if(result != BedjetStatusOk) {
        /* Rejected frames never update state; keep them for capture instead. */
        d->frames_rejected++;
        d->rejected_since_ok++;
        d->last_reason = result;
        record(d->rejected, &d->rejected_len, bytes, length);
    } else {
        d->frames_ok++;
        d->rejected_since_ok = 0;
        record(d->accepted, &d->accepted_len, bytes, length);
        /* Inputs captured before observing a mode switch must not execute
         * later under another mode's limits, including RF-remote switches. */
        if(had_status && old_mode != c->model.status.mode) atomic_fetch_add(&c->input_epoch, 1);
        /* The mode draft follows each observed mode, so the row never shows a
         * different mode as if it were current. */
        if(!had_status || old_mode != c->model.status.mode)
            for(unsigned i = 0; i < sizeof(mode_values); i++)
                if(mode_values[i] == c->model.status.mode) c->model.mode_choice = i;
        c->model.has_status = true;
        c->model.status_ms = time;
        reconcile_command(c, time);
        if(c->model.command.state == BedjetCommandIdle)
            message(c, "Live device status");
        else
            message(c, c->model.command.result);
    }
}
static void read_status(BedjetController* c) {
    BedjetDiagnostics* d = &c->model.diagnostics;
    uint8_t bytes[32], length = 0;
    uint32_t started = now(c);
    if(!c->transport.read(
           c->transport_context,
           c->status_handle,
           bytes,
           sizeof(bytes),
           &length,
           BEDJET_BLE_TIMEOUT_MS)) {
        d->reads_failed++;
        link_stuck(c, classify(c, started, BEDJET_BLE_TIMEOUT_MS, true));
        return;
    }
    d->reads_ok++;
    record(d->read, &d->read_len, bytes, length);
    /* Reference implementations read an 11-byte tail here rather than a full
     * status packet. Record what this unit returns; only a validated status
     * frame may update observed state. */
    BedjetStatus probe;
    d->read_result = bedjet_status_parse(bytes, d->read_len, &probe);
    if(d->read_result == BedjetStatusOk && !pending(c))
        accept_status(c, bytes, d->read_len, now(c));
}
static void resync(BedjetController* c) {
    uint32_t time = now(c);
    if(c->refreshed && time - c->refresh_ms < BEDJET_REFRESH_MIN_MS) {
        message(c, "Refresh: wait a moment");
        return;
    }
    c->refreshed = true;
    c->refresh_ms = time;
    bool subscribed = c->transport.subscribe(c->transport_context, c->cccd, BEDJET_BLE_TIMEOUT_MS);
    if(!subscribed && link_stuck(c, classify(c, time, BEDJET_BLE_TIMEOUT_MS, true))) return;
    if(pending(c)) return;
    uint32_t reads = c->model.diagnostics.reads_ok;
    read_status(c);
    if(c->phase != BedjetReady) return;
    char text[32];
    if(c->model.diagnostics.reads_ok == reads)
        snprintf(text, sizeof(text), "Refresh: read failed%s", subscribed ? "" : ", no notify");
    else
        snprintf(
            text,
            sizeof(text),
            "Refresh: read %u bytes (%s)",
            c->model.diagnostics.read_len,
            bedjet_status_result_name(c->model.diagnostics.read_result));
    message(c, text);
}
static void send_intent(BedjetController* c, BedjetCommandKind kind, uint16_t value) {
    if(pending(c) || !c->model.connected || !c->transport.connected(c->transport_context)) return;
    uint8_t bytes[3];
    size_t length = 0;
    if(kind == BedjetCommandTemperature)
        length =
            bedjet_temperature(value, c->model.status.min_step, c->model.status.max_step, bytes);
    else if(kind == BedjetCommandFan)
        length = bedjet_fan(value, bytes);
    else if(kind == BedjetCommandTimer)
        length = bedjet_timer(value, c->model.status.max_minutes, bytes);
    else if(kind == BedjetCommandOff)
        length = bedjet_button(1, bytes);
    else if(kind == BedjetCommandMemory)
        length = bedjet_button(0x20 + value - 1, bytes);
    else if(kind == BedjetCommandMode)
        for(unsigned i = 0; i < 4; i++)
            if(mode_values[i] == value) length = bedjet_button(mode_buttons[i], bytes);
    if(!length) return;
    c->model.command = (BedjetCommand){
        .kind = kind,
        .state = BedjetCommandPending,
        .desired = value,
        .sent = value,
        .sent_mode = c->model.status.mode};
    bool changes_mode = kind == BedjetCommandMode || kind == BedjetCommandMemory ||
                        kind == BedjetCommandOff;
    if(changes_mode) {
        c->model.limits_ready = false;
        c->limits_waiting = true;
        c->limits_mode = kind == BedjetCommandMemory ? PRESET_MODE : value;
        c->limits_from_mode = c->model.status.mode;
        atomic_fetch_add(&c->input_epoch, 1);
    }
    bool ok = c->transport.write(
        c->transport_context, c->command_handle, bytes, length, BEDJET_BLE_TIMEOUT_MS);
    /* A notification received during the call cannot confirm it. The next
     * strictly newer receive timestamp is an observation, never a protocol ACK. */
    c->model.command.sent_ms = now(c);
    if(changes_mode) c->limits_after_ms = now(c);
    if(!ok) {
        finish_command(c, BedjetCommandRejected, "Write failed; no retry");
        char error[32];
        snprintf(
            error,
            sizeof(error),
            "Write failed: 0x%02X",
            c->transport.last_status(c->transport_context));
        command_result(c, error);
        if(changes_mode) c->limits_mode = ANY_MODE;
    } else if(kind == BedjetCommandMemory) {
        c->model.command.state = BedjetCommandUnverified;
        command_message(c, "unverified");
    } else
        command_message(c, "pending");
}
static void dispatch_desired(BedjetController* c) {
    BedjetCommand* command = &c->model.command;
    if(c->phase != BedjetReady || command->state != BedjetCommandQueued || pending(c)) return;
    if(!bedjet_controller_fresh(&c->model, now(c)) ||
       (command->kind == BedjetCommandTemperature && !c->model.limits_ready)) {
        finish_command(c, BedjetCommandRejected, "Stale limits; intent dropped");
        return;
    }
    uint16_t target = clamp_target(c, command->kind, command->desired);
    send_intent(c, command->kind, target);
}
static void control_key(BedjetController* c, BedjetKey key) {
    BedjetModel* m = &c->model;
    if(key == BedjetOffHint) {
        message(c, "Hold OK 1 s to turn off");
        return;
    }
    if(key == BedjetUp) {
        m->row = (m->row + BedjetRowCount - 1) % BedjetRowCount;
        return;
    }
    if(key == BedjetDown) {
        m->row = (m->row + 1) % BedjetRowCount;
        return;
    }
    int delta = key == BedjetRight ? 1 : key == BedjetLeft ? -1 : 0;
    if(m->row == BedjetRowOff) {
        if(key == BedjetOk) message(c, "Hold OK 1 s to turn off");
        return;
    }
    if(m->row == BedjetRowUnits) {
        m->celsius = !m->celsius; /* Display only; nothing is sent. */
        return;
    }
    if(m->row == BedjetRowInfo) {
        if(delta)
            m->info_page = (m->info_page + BEDJET_INFO_PAGES + delta) % BEDJET_INFO_PAGES;
        else
            resync(c);
        return;
    }
    BedjetCommandKind kind = m->row == BedjetRowMode        ? BedjetCommandMode :
                             m->row == BedjetRowTemperature ? BedjetCommandTemperature :
                             m->row == BedjetRowFan         ? BedjetCommandFan :
                             m->row == BedjetRowTimer       ? BedjetCommandTimer :
                                                              BedjetCommandMemory;
    bool coalescing = command_active(&m->command) && m->command.kind == kind &&
                      (kind == BedjetCommandTemperature || kind == BedjetCommandFan);
    if(m->row == BedjetRowMode && delta) {
        m->mode_choice = (m->mode_choice + 4 + delta) % 4;
        return;
    }
    if(command_active(&m->command) && !coalescing) {
        message(c, "Wait for pending command");
        return;
    }
    if(m->row >= BedjetRowTemperature && m->row <= BedjetRowTimer) {
        if(!bedjet_controller_fresh(m, now(c))) {
            message(c, "Need fresh device status");
            return;
        }
        if((m->row == BedjetRowTemperature || m->row == BedjetRowTimer) && !m->limits_ready) {
            message(c, "Need matching mode status");
            return;
        }
        if((m->row == BedjetRowTemperature || m->row == BedjetRowFan) && delta) {
            int base = coalescing                     ? m->command.desired :
                       m->row == BedjetRowTemperature ? m->status.target_step :
                                                        m->status.fan_step;
            uint16_t target = clamp_target(c, kind, base + delta);
            if(target == base) return;
            if(coalescing) {
                m->command.desired = target;
                command_message(c, "pending");
            } else
                send_intent(c, kind, target);
        } else if(m->row == BedjetRowTimer) {
            int maximum = m->status.max_minutes > 600 ? 600 : m->status.max_minutes;
            if(maximum < 1) {
                message(c, "No timer allowed in this mode");
                return;
            }
            int choice = m->timer_choice + delta * 5;
            m->timer_choice = choice > maximum ? maximum : choice < 1 ? 1 : choice;
            if(key == BedjetOk) send_intent(c, BedjetCommandTimer, m->timer_choice);
        }
    } else if(m->row == BedjetRowMode && key == BedjetOk) {
        if(m->has_status && m->status.mode == mode_values[m->mode_choice]) {
            /* Pressing the button for the current mode has unverified effects
             * on the unit; nothing is sent. */
            char text[32];
            snprintf(text, sizeof(text), "Already %s", bedjet_mode_name(m->status.mode));
            message(c, text);
        } else
            send_intent(c, BedjetCommandMode, mode_values[m->mode_choice]);
    } else if(m->row >= BedjetRowM1 && m->row <= BedjetRowM3 && key == BedjetOk) {
        /* Only an unobserved earlier preset blocks another; a timed-out Mode
         * or Off leaves limits locked but does not block a deliberate preset. */
        if(c->limits_waiting && c->limits_mode == PRESET_MODE)
            message(c, "Wait for preset status");
        else
            send_intent(c, BedjetCommandMemory, m->row - BedjetRowM1 + 1);
    }
}

/* Merge an advertisement or scan response into the unit list by address and
 * address type. Only a matching report can create an entry; a scan response
 * may add the name of a unit already listed. */
static void merge_device(BedjetController* c, const BedjetAdvertisement* a) {
    BedjetModel* m = &c->model;
    unsigned i;
    for(i = 0; i < m->count; i++)
        if(m->devices[i].addr_type == a->addr_type && !memcmp(m->devices[i].addr, a->addr, 6))
            break;
    BedjetDevice candidate = {0};
    BedjetDevice* d = i < m->count ? &m->devices[i] : &candidate;
    uint8_t identity = advertisement_identity(a, d);
    if(i == m->count) {
        if(!identity) return;
        if(m->count == BEDJET_DEVICE_COUNT) {
            m->more_devices = true;
            return;
        }
        memcpy(candidate.addr, a->addr, 6);
        candidate.addr_type = a->addr_type;
        m->devices[m->count++] = candidate;
        d = &m->devices[i];
    }
    d->flags |= identity;
    if(a->event_type == BedjetAdvInd || a->event_type == BedjetAdvDirectInd)
        d->flags |= BedjetDeviceConnectable;
    d->rssi = a->rssi;
}
static void begin_disconnect(BedjetController* c, bool failure) {
    invalidate(c, true);
    c->phase = BedjetDisconnecting;
    c->cleanup_attempts = 0;
    c->setup_failed = failure;
    c->model.connected = false;
    c->model.screen = Closing;
    c->transport.set_notify(c->transport_context, NULL, NULL);
}
/* The pinned firmware records only synchronous controller results in its
 * status, so procedure timeouts and ATT errors usually read 0x00 or an older
 * call's value. Show actionable stage text; keep the raw code as diagnostics. */
static void setup_failed(BedjetController* c, const char* text) {
    message(c, text);
    c->model.diagnostics.ble_status = c->transport.last_status(c->transport_context);
    begin_disconnect(c, true);
}
/* `text` is the stage's own advice for an early failure with no raw status. */
static void stage_failed(
    BedjetController* c,
    const char* stage,
    const char* text,
    uint32_t started,
    uint32_t wait_ms,
    bool linked) {
    Failure failure = classify(c, started, wait_ms, linked);
    char reason[sizeof(c->model.message)];
    if(failure == FailureLost)
        snprintf(reason, sizeof(reason), "%s: link lost", stage);
    else if(failure == FailureTimeout)
        snprintf(reason, sizeof(reason), "%s: no reply", stage);
    else if(failure == FailureRefused)
        snprintf(reason, sizeof(reason), "%s: BLE 0x%02X", stage, c->model.diagnostics.ble_status);
    else
        snprintf(reason, sizeof(reason), "%s", text);
    begin_disconnect(c, true);
    message(c, reason);
}
static void event(BedjetController* c, const BedjetEvent* e) {
    if(e->session != bedjet_controller_session(c)) return;
    BedjetModel* m = &c->model;
    if(e->type == BedjetEventAdvertisement && c->phase == BedjetScan) {
        merge_device(c, &e->value.advertisement);
    } else if(
        e->type == BedjetEventStatus && m->connected &&
        e->value.status.handle == c->status_handle) {
        accept_status(c, e->value.status.bytes, e->value.status.length, e->received_ms);
    } else if(e->type == BedjetEventRefresh && c->phase == BedjetReady) {
        resync(c);
    } else if(e->type == BedjetEventKey) {
        if(e->input_epoch != atomic_load(&c->input_epoch)) return;
        BedjetKey key = e->value.key;
        if(c->phase == BedjetIdle && m->screen == Home && key == BedjetOk) {
            if(!try_invalidate(c, true, 0)) return;
            m->count = m->selected = 0;
            m->more_devices = false;
            c->scan_seen_count = 0; /* No scan callback is registered here. */
            atomic_store(&c->scan_matched, 0);
            atomic_store(&c->scan_filtered, 0);
            atomic_store(&c->scan_refused, 0);
            c->phase = BedjetPauseAdvertising;
            c->phase_started_ms = now(c);
            c->was_advertising |= c->transport.is_advertising(c->transport_context);
            if(c->transport.is_advertising(c->transport_context))
                c->transport.advertise(c->transport_context, false);
            message(c, "Preparing Bluetooth scan");
        } else if(c->phase == BedjetIdle && m->screen == Devices && m->count) {
            if(key == BedjetUp) m->selected = (m->selected + m->count - 1) % m->count;
            if(key == BedjetDown) m->selected = (m->selected + 1) % m->count;
            if(key == BedjetOk) {
                if(!try_invalidate(c, true, 0)) return;
                m->diagnostics = (BedjetDiagnostics){0};
                c->refreshed = false;
                atomic_store(&c->oversize_frames, 0);
                m->screen = Connecting;
                c->phase = BedjetConnect;
                message(c, "Connecting...");
            }
        } else if(c->phase == BedjetReady)
            control_key(c, key);
    }
}

static void apply_cancellation(BedjetController* c) {
    if(c->phase == BedjetDone) return;
    if(atomic_load(&c->priority) & BackPending) {
        if(c->phase == BedjetBoot || c->model.screen == Home || c->model.screen == Unsupported ||
           c->phase == BedjetDisconnecting)
            bedjet_controller_shutdown(c);
        else if(c->initialized)
            begin_disconnect(c, false);
    }
    if(atomic_load(&c->shutdown_requested) && c->phase != BedjetStopping) {
        invalidate(c, true);
        c->model.connected = false;
        c->model.screen = Closing;
        c->phase = BedjetStopping;
        c->cleanup_attempts = 0;
        if(c->initialized) c->transport.set_notify(c->transport_context, NULL, NULL);
    }
}

static void priority_off(BedjetController* c) {
    if(!c->transport.connected(c->transport_context)) {
        begin_disconnect(c, true);
        message(c, "Connection lost; Off not sent");
        return;
    }
    /* Rotate the session before the write: every older key/refresh/notification
     * is now stale, even if a producer still holds the queue. Drain the old
     * callback before replacing its captured session. Keep observed values. */
    if(!try_invalidate(c, false, OffPending)) return;
    c->transport.set_notify(c->transport_context, NULL, NULL);
    send_intent(c, BedjetCommandOff, 0);
    apply_cancellation(c);
    if(c->phase != BedjetReady) return;
    callbacks_for_session(c);
    c->transport.set_notify(c->transport_context, notify_sink, &c->callback_context);
    if(!cancelled(c)) {
        atomic_store(&c->control_ready, true);
        atomic_store(&c->accepting_input, true);
    }
}

void bedjet_controller_step(BedjetController* c) {
    if(c->phase == BedjetDone) return;
    apply_cancellation(c);
    command_timeout(c);
    if(c->phase == BedjetReady && (atomic_load(&c->priority) & OffPending)) {
        priority_off(c);
        apply_cancellation(c);
        /* Never dispatch queued control in the same tick as Off. */
        c->model.dropped_events = atomic_load(&c->dropped_events);
        c->model.diagnostics.frames_oversize = atomic_load(&c->oversize_frames);
        return;
    }
    void* t = c->transport_context;
    switch(c->phase) {
    case BedjetBoot:
        if(!c->transport.supported(t)) {
            c->model.screen = Unsupported;
            message(c, "Requires Full BLE radio stack");
            c->phase = BedjetIdle;
        } else {
            c->transport.init(t);
            c->initialized = true;
            /* A previous run whose teardown was deferred can leave the shared
             * firmware central scanning or connected. Make one bounded attempt
             * to clear it instead of failing later with a misleading error. */
            bool inherited = c->transport.is_scanning(t) || c->transport.connected(t);
            if(inherited) {
                if(c->transport.is_scanning(t)) c->transport.scan_stop(t);
                if(c->transport.connected(t)) c->transport.disconnect(t);
            }
            bool busy = c->transport.is_scanning(t) || c->transport.connected(t);
            idle(c);
            message(
                c,
                busy      ? "BT busy: reboot Flipper" :
                inherited ? "Cleared old BT session" :
                            "OK: scan for your BedJet");
        }
        break;
    case BedjetPauseAdvertising:
        if(c->transport.is_advertising(t)) {
            if(now(c) - c->phase_started_ms >= 500) {
                idle(c);
                message(c, "Advertising did not stop");
            }
            break;
        }
        callbacks_for_session(c);
        if(c->transport.scan_start(t, scan_sink, &c->callback_context)) {
            c->phase = BedjetScan;
            c->phase_started_ms = now(c);
            c->cleanup_attempts = 0;
            c->model.screen = Scanning;
            message(c, "Scanning for 8 seconds...");
        } else {
            atomic_store(&c->accepting_callbacks, false);
            idle(c);
            message(c, "Scan failed; OK to retry");
        }
        break;
    case BedjetScan:
        if(now(c) - c->phase_started_ms >= 8000) {
            /* Merge reports already queued (bounded by the inbox) so a unit
             * seen in the last ticks is not lost by the clear below. */
            BedjetEvent queued;
            for(unsigned i = 0; i < BEDJET_QUEUE_CAPACITY && !pending(c) && pop(c, &queued); i++)
                event(c, &queued);
            if(c->transport.scan_stop(t)) {
                atomic_store(&c->accepting_callbacks, false);
                /* Reports that arrived during the stop call are stale; do not
                 * let them occupy inbox slots needed by the first key presses. */
                clear_queue(c);
                c->phase = BedjetIdle;
                c->model.screen = Devices;
                atomic_store(&c->accepting_input, true);
                message(
                    c,
                    c->model.count ? "Select unit; OK to connect" : "None found: phone app open?");
            } else if(++c->cleanup_attempts >= BEDJET_CLEANUP_ATTEMPTS) {
                setup_failed(c, "Scan stop failed");
            } else
                message(c, "Scan stop failed; retrying");
        }
        break;
    case BedjetConnect: {
        BedjetDevice* a = &c->model.devices[c->model.selected];
        uint32_t started = now(c);
        if(c->transport.connect(t, a->addr, a->addr_type, BEDJET_CONNECT_TIMEOUT_MS))
            c->phase = BedjetFindCommand;
        else
            stage_failed(
                c,
                "Connect",
                "Connect failed: app open?",
                started,
                BEDJET_CONNECT_TIMEOUT_MS,
                false);
        break;
    }
    case BedjetFindCommand: {
        uint32_t started = now(c);
        if(c->transport.find(
               t, bedjet_command_uuid, &c->command_handle, NULL, BEDJET_BLE_TIMEOUT_MS))
            c->phase = BedjetFindStatus;
        else
            stage_failed(
                c, "Find cmd", "Discovery failed: no cmd", started, BEDJET_BLE_TIMEOUT_MS, true);
        break;
    }
    case BedjetFindStatus: {
        uint32_t started = now(c);
        if(!c->transport.find(
               t, bedjet_status_uuid, &c->status_handle, &c->cccd, BEDJET_BLE_TIMEOUT_MS))
            stage_failed(
                c, "Find status", "Discovery failed: status", started, BEDJET_BLE_TIMEOUT_MS, true);
        else if(!c->cccd)
            setup_failed(c, "Setup failed: no notify");
        else
            c->phase = BedjetSubscribe;
        break;
    }
    case BedjetSubscribe: {
        callbacks_for_session(c);
        c->transport.set_notify(t, notify_sink, &c->callback_context);
        if(cancelled(c)) break;
        uint32_t started = now(c);
        if(c->transport.subscribe(t, c->cccd, BEDJET_BLE_TIMEOUT_MS)) {
            if(!cancelled(c)) {
                c->model.connected = true;
                c->phase = BedjetInitialRead;
            }
        } else
            /* An early failure with no raw status may be the peer's ATT
             * security error; the firmware does not report which. */
            stage_failed(
                c, "Subscribe", "Subscribe failed (pair?)", started, BEDJET_BLE_TIMEOUT_MS, true);
        break;
    }
    case BedjetInitialRead:
        c->model.row = c->model.mode_choice = c->model.info_page = 0;
        c->model.timer_choice = 30;
        read_status(c);
        if(cancelled(c) || c->phase != BedjetInitialRead) break;
        c->model.screen = Control;
        c->phase = BedjetReady;
        /* Publish control before input so a hold is never read as plain OK. */
        atomic_store(&c->control_ready, true);
        atomic_store(&c->accepting_input, true);
        if(!c->model.has_status) message(c, "Waiting for live status");
        break;
    case BedjetDisconnecting: {
        bool stopped = !c->transport.is_scanning(t) || c->transport.scan_stop(t);
        bool disconnected = !c->transport.connected(t) || c->transport.disconnect(t);
        if(stopped && disconnected) {
            idle(c);
            if(!c->setup_failed) message(c, "OK: scan for your BedJet");
        } else if(++c->cleanup_attempts >= BEDJET_CLEANUP_ATTEMPTS)
            bedjet_controller_shutdown(c);
        break;
    }
    case BedjetStopping: {
        bool done = !c->initialized || c->transport.deinit(t);
        if(done || ++c->cleanup_attempts >= BEDJET_CLEANUP_ATTEMPTS) {
            c->model.cleanup_deferred = !done;
            if(done && c->was_advertising) c->transport.advertise(t, true);
            c->phase = BedjetDone;
            c->model.screen = Stopped;
        }
        break;
    }
    default:
        break;
    }
    /* Apply cancellation before publishing this tick's model, including after
     * setup calls. Off waits for the next tick but blocks ordinary dispatch. */
    apply_cancellation(c);
    if(!pending(c) && c->phase != BedjetDisconnecting && c->phase != BedjetStopping &&
       c->phase != BedjetDone) {
        BedjetEvent e;
        if(pop(c, &e)) event(c, &e);
    }
    apply_cancellation(c);
    if(c->model.connected && !c->transport.connected(t) && c->phase != BedjetStopping &&
       c->phase != BedjetDone) {
        begin_disconnect(c, true);
        message(c, "Connection lost. Back/exit");
    }
    command_timeout(c);
    dispatch_desired(c);
    apply_cancellation(c);
    c->model.dropped_events = atomic_load(&c->dropped_events);
    c->model.diagnostics.frames_oversize = atomic_load(&c->oversize_frames);
}
