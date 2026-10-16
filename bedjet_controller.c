// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_controller.h"
#include <stdio.h>
#include <string.h>

_Static_assert(
    ATOMIC_BOOL_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
    "Callback inbox requires lock-free atomics");
_Static_assert(sizeof(BedjetController) <= 4096, "Controller exceeds its fixed memory budget");

static uint32_t now(BedjetController* c) {
    return c->transport.now_ms(c->transport_context);
}
uint32_t bedjet_controller_session(const BedjetController* c) {
    return atomic_load(&c->session);
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
static void invalidate(BedjetController* c) {
    atomic_store(&c->accepting_input, false);
    atomic_store(&c->accepting_callbacks, false);
    atomic_fetch_add(&c->session, 1);
    /* Old events are rejected by epoch even if the producer holds the inbox
     * briefly. Queue clearing is only an optimization; never spin in callbacks. */
    if(queue_lock(c)) {
        c->queue_count = 0;
        c->queue_head = 0;
        queue_unlock(c);
    }
    c->model.has_status = false;
}

bool bedjet_controller_post_key(BedjetController* c, uint32_t session, BedjetKey key) {
    if(session != bedjet_controller_session(c) || atomic_load(&c->shutdown_requested))
        return false;
    if(key == BedjetBack) {
        atomic_store(&c->back_requested, true);
        return true;
    }
    if((unsigned)key > (unsigned)BedjetBack || !atomic_load(&c->accepting_input)) return false;
    BedjetEvent e = {.type = BedjetEventKey, .session = session, .value.key = key};
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
    if(!bytes || length < 18 || length > 32 || !atomic_load(&c->accepting_callbacks)) return false;
    BedjetEvent e = {.type = BedjetEventStatus, .session = session, .received_ms = time};
    e.value.status.handle = handle;
    e.value.status.length = length;
    memcpy(e.value.status.bytes, bytes, length);
    return post(c, &e);
}
bool bedjet_controller_refresh(BedjetController* c, uint32_t session) {
    if(!atomic_load(&c->accepting_input) || atomic_load(&c->shutdown_requested)) return false;
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
    atomic_init(&c->back_requested, false);
    atomic_init(&c->shutdown_requested, false);
    atomic_init(&c->accepting_input, false);
    atomic_init(&c->accepting_callbacks, false);
    atomic_init(&c->session, 1);
    atomic_init(&c->dropped_events, 0);
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

static void
    accept_status(BedjetController* c, const uint8_t* bytes, size_t length, uint32_t time) {
    uint32_t current = now(c);
    if(current - time >= BEDJET_STATUS_FRESH_MS) return;
    /* Receive times must move forward within the short session window, also
     * across a uint32 clock wrap. Old queued notifications cannot refresh age. */
    if(c->model.has_status && (int32_t)(time - c->model.status_ms) < 0) return;
    if(bedjet_status_decode(bytes, length, &c->model.status)) {
        c->model.has_status = true;
        c->model.status_ms = time;
        message(c, "Live device status");
    }
}

static void read_status(BedjetController* c) {
    uint8_t bytes[32], length = 0;
    if(c->transport.read(
           c->transport_context,
           c->status_handle,
           bytes,
           sizeof(bytes),
           &length,
           BEDJET_BLE_TIMEOUT_MS))
        accept_status(c, bytes, length, now(c));
}
static void write_command(BedjetController* c, const uint8_t* bytes, size_t length) {
    if(!length || atomic_load(&c->shutdown_requested) || atomic_load(&c->back_requested)) return;
    if(!c->model.connected || !c->transport.connected(c->transport_context)) return;
    bool ok = c->transport.write(
        c->transport_context, c->command_handle, bytes, length, BEDJET_BLE_TIMEOUT_MS);
    if(ok)
        message(c, "Queued; check device status");
    else
        snprintf(
            c->model.message,
            sizeof(c->model.message),
            "Write failed: 0x%02X",
            c->transport.last_status(c->transport_context));
}
static const uint8_t mode_buttons[] = {2, 3, 5, 6};
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
    uint8_t bytes[3];
    size_t length = 0;
    if(m->row == 0) {
        if(delta) m->mode_choice = (m->mode_choice + 4 + delta) % 4;
        if(key == BedjetOk) length = bedjet_button(mode_buttons[m->mode_choice], bytes);
    } else if(m->row == 7 && key == BedjetOk)
        length = bedjet_button(1, bytes);
    else if(m->row >= 4 && m->row <= 6 && key == BedjetOk)
        length = bedjet_button(0x20 + m->row - 4, bytes);
    else if(m->row >= 1 && m->row <= 3) {
        if(!bedjet_controller_fresh(m, now(c))) {
            message(c, "Need fresh device status");
            return;
        }
        if(m->row == 1 && delta) {
            int step = m->status.target_step + delta;
            if(step >= m->status.min_step && step <= m->status.max_step)
                length = bedjet_temperature(step, m->status.min_step, m->status.max_step, bytes);
        } else if(m->row == 2 && delta) {
            int step = m->status.fan_step + delta;
            if(step >= 0 && step <= 19) length = bedjet_fan(step, bytes);
        } else if(m->row == 3) {
            int maximum = m->status.max_minutes > 600 ? 600 : m->status.max_minutes;
            int choice = m->timer_choice + delta * 5;
            if(choice > maximum) choice = maximum;
            if(choice < 1) choice = 1;
            m->timer_choice = choice;
            if(key == BedjetOk) length = bedjet_timer(m->timer_choice, maximum, bytes);
        }
    }
    write_command(c, bytes, length);
}

static void begin_disconnect(BedjetController* c, bool failure) {
    invalidate(c);
    c->phase = BedjetDisconnecting;
    c->cleanup_attempts = 0;
    c->setup_failed = failure;
    c->model.connected = false;
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
        read_status(c);
    } else if(e->type == BedjetEventKey) {
        BedjetKey key = e->value.key;
        if(c->phase == BedjetIdle && m->screen == Home && key == BedjetOk) {
            invalidate(c);
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
                invalidate(c);
                m->screen = Connecting;
                c->phase = BedjetConnect;
                message(c, "Connecting; confirm pairing");
            }
        } else if(c->phase == BedjetReady)
            control_key(c, key);
    }
}

void bedjet_controller_step(BedjetController* c) {
    if(c->phase == BedjetDone) return;
    if(atomic_exchange(&c->back_requested, false)) {
        if(c->phase == BedjetBoot || c->model.screen == Home || c->model.screen == Unsupported ||
           c->phase == BedjetDisconnecting)
            bedjet_controller_shutdown(c);
        else if(c->initialized)
            begin_disconnect(c, false);
    }
    if(atomic_load(&c->shutdown_requested) && c->phase != BedjetStopping) {
        invalidate(c);
        c->model.connected = false;
        c->phase = BedjetStopping;
        c->cleanup_attempts = 0;
        if(c->initialized) c->transport.set_notify(c->transport_context, NULL, NULL);
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
            idle(c);
            message(c, "OK: scan for your BedJet");
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
                c->phase = BedjetIdle;
                c->model.screen = Devices;
                atomic_store(&c->accepting_input, true);
                message(
                    c,
                    c->model.count ? "Select unit; OK to connect" : "No units. Back to try again");
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
        if(c->transport.subscribe(t, c->cccd, BEDJET_BLE_TIMEOUT_MS)) {
            c->model.connected = true;
            c->phase = BedjetInitialRead;
        } else
            setup_failed(c, "Subscribe");
        break;
    case BedjetInitialRead:
        read_status(c);
        c->model.screen = Control;
        c->model.row = c->model.mode_choice = 0;
        c->model.timer_choice = 30;
        c->phase = BedjetReady;
        atomic_store(&c->accepting_input, true);
        if(!c->model.has_status) message(c, "Waiting for live status");
        break;
    case BedjetDisconnecting: {
        bool stopped = !c->transport.is_scanning(t) || c->transport.scan_stop(t);
        bool disconnected = !c->transport.connected(t) || c->transport.disconnect(t);
        if(stopped && disconnected) {
            idle(c);
            if(!c->setup_failed) message(c, "OK: scan for your BedJet");
        } else if(++c->cleanup_attempts >= 3)
            bedjet_controller_shutdown(c);
        break;
    }
    case BedjetStopping: {
        bool done = !c->initialized || c->transport.deinit(t);
        if(done || ++c->cleanup_attempts >= 3) {
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
    /* No queued control can run in a step in which cancellation was requested
     * during a bounded transport operation. The next step performs cleanup. */
    if(!atomic_load(&c->back_requested) && !atomic_load(&c->shutdown_requested) &&
       c->phase != BedjetDisconnecting && c->phase != BedjetStopping && c->phase != BedjetDone) {
        BedjetEvent e;
        if(pop(c, &e)) event(c, &e);
    }
    if(c->model.connected && !c->transport.connected(t) && c->phase != BedjetStopping &&
       c->phase != BedjetDone) {
        begin_disconnect(c, true);
        message(c, "Connection lost. Back/exit");
    }
    c->model.dropped_events = atomic_load(&c->dropped_events);
}
