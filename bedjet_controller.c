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
    if((unsigned)key > (unsigned)BedjetOff || !atomic_load(&c->accepting_input) || pending(c))
        return false;
    BedjetEvent e = {
        .type = BedjetEventKey,
        .session = session,
        .input_epoch = atomic_load(&c->input_epoch),
        .value.key = key};
    return post(c, &e);
}
bool bedjet_controller_post_advertisement(
    BedjetController* c,
    uint32_t session,
    const BedjetAdvertisement* a) {
    if(!a || a->data_len > sizeof(a->data) || !atomic_load(&c->accepting_callbacks)) return false;
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
    return true;
}

static bool advertisement_matches(const BedjetAdvertisement* a) {
    for(size_t i = 0; i < a->data_len;) {
        size_t size = a->data[i++];
        if(!size) break;
        if(size > a->data_len - i) return false;
        uint8_t type = a->data[i];
        const uint8_t* value = &a->data[i + 1];
        size_t len = size - 1;
        if((type == 0x08 || type == 0x09) && len >= 6) {
            bool match = true;
            for(size_t j = 0; j < 6; j++) {
                uint8_t ch = value[j];
                if(ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                if(ch != (uint8_t) "BEDJET"[j]) match = false;
            }
            if(match) return true;
        }
        if(type == 0x06 || type == 0x07)
            for(size_t j = 0; j + 16 <= len; j += 16)
                if(!memcmp(value + j, bedjet_service_uuid, 16)) return true;
        i += size;
    }
    return false;
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
    if(!c->transport.read(
           c->transport_context,
           c->status_handle,
           bytes,
           sizeof(bytes),
           &length,
           BEDJET_BLE_TIMEOUT_MS)) {
        d->reads_failed++;
        d->ble_status = c->transport.last_status(c->transport_context);
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
    if(!subscribed)
        c->model.diagnostics.ble_status = c->transport.last_status(c->transport_context);
    if(pending(c)) return;
    uint32_t reads = c->model.diagnostics.reads_ok;
    read_status(c);
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
    if(key == BedjetUp) {
        m->row = (m->row + 7) % 8;
        return;
    }
    if(key == BedjetDown) {
        m->row = (m->row + 1) % 8;
        return;
    }
    int delta = key == BedjetRight ? 1 : key == BedjetLeft ? -1 : 0;
    BedjetCommandKind kind = m->row == 0 ? BedjetCommandMode :
                             m->row == 1 ? BedjetCommandTemperature :
                             m->row == 2 ? BedjetCommandFan :
                             m->row == 3 ? BedjetCommandTimer :
                                           BedjetCommandMemory;
    bool coalescing = command_active(&m->command) && m->command.kind == kind &&
                      (kind == BedjetCommandTemperature || kind == BedjetCommandFan);
    if(m->row == 0 && delta) {
        m->mode_choice = (m->mode_choice + 4 + delta) % 4;
        return;
    }
    if(command_active(&m->command) && !coalescing) {
        message(c, "Wait for pending command");
        return;
    }
    if(m->row >= 1 && m->row <= 3) {
        if(!bedjet_controller_fresh(m, now(c))) {
            message(c, "Need fresh device status");
            return;
        }
        if((m->row == 1 || m->row == 3) && !m->limits_ready) {
            message(c, "Need matching mode status");
            return;
        }
        if((m->row == 1 || m->row == 2) && delta) {
            int base = coalescing  ? m->command.desired :
                       m->row == 1 ? m->status.target_step :
                                     m->status.fan_step;
            uint16_t target = clamp_target(c, kind, base + delta);
            if(target == base) return;
            if(coalescing) {
                m->command.desired = target;
                command_message(c, "pending");
            } else
                send_intent(c, kind, target);
        } else if(m->row == 3) {
            int maximum = m->status.max_minutes > 600 ? 600 : m->status.max_minutes;
            if(maximum < 1) {
                message(c, "No timer allowed in this mode");
                return;
            }
            int choice = m->timer_choice + delta * 5;
            m->timer_choice = choice > maximum ? maximum : choice < 1 ? 1 : choice;
            if(key == BedjetOk) send_intent(c, BedjetCommandTimer, m->timer_choice);
        }
    } else if(m->row == 0 && key == BedjetOk)
        send_intent(c, BedjetCommandMode, mode_values[m->mode_choice]);
    else if(m->row >= 4 && m->row <= 6 && key == BedjetOk) {
        /* Only an unobserved earlier preset blocks another; a timed-out Mode
         * or Off leaves limits locked but does not block a deliberate preset. */
        if(c->limits_waiting && c->limits_mode == PRESET_MODE)
            message(c, "Wait for preset status");
        else
            send_intent(c, BedjetCommandMemory, m->row - 3);
    }
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
static void setup_failed(BedjetController* c, const char* stage) {
    snprintf(
        c->model.message,
        sizeof(c->model.message),
        "%s failed: 0x%02X",
        stage,
        c->transport.last_status(c->transport_context));
    begin_disconnect(c, true);
}
static void event(BedjetController* c, const BedjetEvent* e) {
    if(e->session != bedjet_controller_session(c)) return;
    BedjetModel* m = &c->model;
    if(e->type == BedjetEventAdvertisement && c->phase == BedjetScan) {
        const BedjetAdvertisement* a = &e->value.advertisement;
        if(!advertisement_matches(a)) return;
        unsigned i;
        for(i = 0; i < m->count; i++)
            if(m->devices[i].addr_type == a->addr_type && !memcmp(m->devices[i].addr, a->addr, 6))
                break;
        if(i < BEDJET_DEVICE_COUNT) {
            m->devices[i] = *a;
            if(i == m->count) m->count++;
        }
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
                message(c, "Connecting; confirm pairing");
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
            if(c->transport.scan_stop(t)) {
                atomic_store(&c->accepting_callbacks, false);
                /* Reports queued during the scan are now stale; do not let
                 * them occupy inbox slots needed by the first key presses. */
                clear_queue(c);
                c->phase = BedjetIdle;
                c->model.screen = Devices;
                atomic_store(&c->accepting_input, true);
                message(
                    c,
                    c->model.count ? "Select unit; OK to connect" : "No units. Back to try again");
            } else if(++c->cleanup_attempts >= BEDJET_CLEANUP_ATTEMPTS) {
                setup_failed(c, "Scan stop");
            } else
                message(c, "Scan stop failed; retrying");
        }
        break;
    case BedjetConnect: {
        BedjetAdvertisement* a = &c->model.devices[c->model.selected];
        if(c->transport.connect(t, a->addr, a->addr_type, 8000))
            c->phase = BedjetFindCommand;
        else
            setup_failed(c, "Connect");
        break;
    }
    case BedjetFindCommand:
        if(c->transport.find(
               t, bedjet_command_uuid, &c->command_handle, NULL, BEDJET_BLE_TIMEOUT_MS))
            c->phase = BedjetFindStatus;
        else
            setup_failed(c, "Command discovery");
        break;
    case BedjetFindStatus:
        if(c->transport.find(
               t, bedjet_status_uuid, &c->status_handle, &c->cccd, BEDJET_BLE_TIMEOUT_MS) &&
           c->cccd)
            c->phase = BedjetSubscribe;
        else
            setup_failed(c, "Status discovery");
        break;
    case BedjetSubscribe:
        callbacks_for_session(c);
        c->transport.set_notify(t, notify_sink, &c->callback_context);
        if(cancelled(c)) break;
        if(c->transport.subscribe(t, c->cccd, BEDJET_BLE_TIMEOUT_MS)) {
            if(!cancelled(c)) {
                c->model.connected = true;
                c->phase = BedjetInitialRead;
            }
        } else
            setup_failed(c, "Subscribe");
        break;
    case BedjetInitialRead:
        read_status(c);
        if(cancelled(c)) break;
        c->model.screen = Control;
        c->model.row = c->model.mode_choice = 0;
        c->model.timer_choice = 30;
        c->phase = BedjetReady;
        atomic_store(&c->accepting_input, true);
        atomic_store(&c->control_ready, true);
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
